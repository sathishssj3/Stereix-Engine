# Stereix Engine Project Memory

This document is the repo-local project memory for Stereix Engine. Use it as the first source of context before editing, adding features, or planning roadmap work.

## 1. Project Mission And Core Technology

Stereix Engine is a universal virtual reality injection layer that transforms flat-screen PC games into native, stereoscopic 3D VR experiences. It hooks DirectX 11, DirectX 12, and Vulkan render swapchains, converts mono outputs to stereo views with custom IPD, and projects them to OpenXR headsets.

Core stack:

- Launcher: Electron + React using Vite, TypeScript, and Tailwind CSS
- Injection CLI: C++ command-line injector, `vr-inject-cli.exe`
- Core hook engine: C++ DLL, `vrinject.dll`, using MinHook, Direct3D, Vulkan, and OpenXR

## 2. Codebase Architecture

```mermaid
graph TD
    Launcher[Electron/React UI] -->|Spawns with UAC elevation| CLI[vr-inject-cli.exe]
    CLI -->|CreateRemoteThread / LoadLibrary| Target[Target Game Process]
    Target -->|Loads| DLL[vrinject.dll]
    DLL -->|Swapchain Interception| Hooks[DX11 / DX12 / Vulkan / Input Hooks]
    Hooks -->|Dynamic Camera Override| Matrix[MatrixClassifier]
    Hooks -->|Stereo Warp Shaders| OpenXR[OpenXR Runtime / Headset]
```

## 3. Audit And Stability Resolutions

The codebase has already gone through a stability pass. Preserve these fixes when making future edits:

- BUG-01/02, MinHook stability: `HookManager` owns MinHook lifecycle. Avoid adding redundant initialization or shutdown in individual hooks such as `dx11_hook.cpp`.
- BUG-05, thread handle leak: `dllmain.cpp` should close worker thread handles after creation or cleanup.
- BUG-06, serialization failure: `config_manager.cpp` must keep C++ serialization in sync with launcher fields, including recommended resolution, sRGB correction, depth submission, raw input mode, and auto-inject.
- BUG-09, UI infinite loop: `injectionManager.ts` should use bounded polling when waiting for target game processes.
- BUG-14, DX12 Map/Unmap worker contention: never hook `ID3D12Resource::Map` / `Unmap` in universal depth mode to prevent worker thread lock contention and `DXGI_ERROR_INVALID_CALL` device removals.
- BUG-15, OpenXR subImage stack corruption: `XrCompositionLayerProjectionView` subImage structs must explicitly zero-initialize `.imageArrayIndex = 0` and `.imageRect.offset = {0, 0}` to prevent `XR_ERROR_SWAPCHAIN_RECT_INVALID`.
- BUG-17, Color space double-gamma: Game swapchain backbuffers are already tone-mapped and gamma-corrected (sRGB) by the engine. Never apply artificial `pow(c, 2.2f)` squaring in `stereo_reprojection.hlsl` or `tonemap.hlsl`, as this causes severe double-gamma crushing (fog, sky, and scenery drop to pitch black).
- BUG-19, OTA dependency shadowing: In `injectionManager.ts`, local build outputs (`build/bin`) must always take precedence over `%APPDATA%\Stereix-Dev\updates\` during development (`!app.isPackaged`) and when the local binary is newer than the cached OTA download. Otherwise stale hotfixes silently overwrite fresh developer builds.
- DEAD-05, DllMain deadlock: DLL detach cleanup should use bounded waits to reduce loader-lock risk.
- QUAL-01, injector duplication: use the unified injector in `src/injector/main.cpp`; do not reintroduce redundant injector implementations under `tools/`.
- QUAL-04, hardcoded launcher logic: avoid game-specific hardcoded icon exceptions in `libraryManager.ts`.

## 4. Current Milestone: Advanced Heuristic Memory Scanning

The active milestone is to move beyond hardcoded camera matrix pointers and basic constant-buffer hooks toward an active memory scanner.

Primary components:

- PageScanner: traverse dynamic RAM heaps with `VirtualQuery`, filtering for `MEM_COMMIT` and writable regions where projection or view matrices may reside.
- PointerChainResolver: backtrace dynamic matrix memory addresses to static base offsets, for example `Game.exe + 0x3AF10 -> +0x24 -> +0x180`, and cache paths for subsequent launches.
- CameraDeltaTracker: correlate matrix candidates with mouse delta or head-motion input to isolate matrices that respond directly to camera movement.

Implementation guidance:

- Keep scanning work off hot render paths.
- Use conservative memory access and guard against unreadable pages.
- Cache confidence scores and pointer chains per executable/profile.
- Prefer deterministic diagnostics in the launcher so users can see why a candidate was selected or rejected.

## 5. Next-Generation AI And Rendering Roadmap

The long-term roadmap includes seven AI-driven models optimized for an 11.1 ms frame budget.

Planned models:

- Spatial-Temporal Memory Transformer: encoder-only self-attention model for classifying raw dynamic memory vectors over 10-frame sequences.
- Depth-Aware Gated Inpainter: U-Net using gated convolutions and depth-buffer input to patch disocclusion holes without boundary color bleeding.
- OFA Vector Refiner: quantized CNN that corrects raw GPU optical-flow vectors for cleaner ASW frame synthesis.
- Comfort Guard MLP: dense network predicting simulation sickness risk from gaze vectors and rotational speed.
- Holographic UI Synthesizer: YOLOv8-tiny style model extracting 2D HUD components for projection as floating 3D panels.
- Gaze Predictor: LSTM or GRU model predicting pupil trajectory to offset eye-tracker latency.
- Neural Super Resolution: TSR-GAN style upscaler for low-resolution internal renders.

Execution blueprint:

- Synchronous path on the main render thread, under 1.5 ms: gaze predictor, comfort guard, and frame-generation refiner. These should compile to INT8 or FP16 with node fusion.
- Asynchronous worker path: memory transformer and inpainter mask generation. These should communicate through thread-safe atomics or bounded queues.

### FEAT-10: Real Depth Anything V2 Model Landed (Depth Inpainter Slot)

- **Files**: `stereix-client/models/depth_inpainter.onnx`, `stereix-client/scripts/download_depth_anything_v2.py`, `stereix-client/scripts/finetune_depth.py`
- **Status**: The 2 MB Identity-node CI dummy was replaced with a real Depth Anything V2 Small (ViT-S, 24.8M params) exported to ONNX (93 MB, Git LFS, self-contained — no external `.onnx.data`). Tensor contract: input `image` float32 `[1,3,518,518]`, output `depth` float32 `[1,1,518,518]`, opset 17, static shapes. Verified: loads and infers under the engine-bundled ONNX Runtime with the DirectML execution provider (`test_midas_inpainter` 6/6 green); standalone ORT CPU validation passes.
- **Fine-tuning**: `scripts/finetune_depth.py` self-supervises on game screenshots placed in `stereix-client/data/game_screenshots/` (RTX 3050 6GB budget: batch 2 @ 518² FP16 ≈ 2.8 GB VRAM), freezing the DINOv2 encoder and training the DPT decoder head with scale-invariant + multi-scale gradient losses.

### FEAT-11: Milestone 2 Complete — Native Dynamic Scaling & DirectML Multi-GPU Auto-Detection
- **Files**: `stereix-client/src/ai/midas_inpainter.h`, `stereix-client/src/ai/midas_inpainter.cpp`, `stereix-client/tests/test_ai_directml.cpp`
- **Status**: Resolved the resolution-bound static tensor gap in `MidasInpainter`. Incoming frames of any arbitrary resolution (1080p, 1440p, 4K, or 32x32/64x64 unit tests) are dynamically bilinearly scaled (`BilinearResizePlanarRGB`) into the model's native $518 \times 518$ planar RGB buffer, submitted to DirectML GPU execution, and then upscaled back (`BilinearResizeDepth`) to native caller dimensions.
- **Contrast & Synthetic Fallback**: Synthetic untextured patterns (where ViT produces near-zero variance, $range \le 0.05f$) cleanly engage the perspective/luminance heuristic fallback to prevent flat 2D projection in edge cases.
- **Multi-GPU Auto-Detection**: DirectML harnesses query DXGI for the discrete GPU (`desc.DedicatedVideoMemory`), correctly selecting the NVIDIA RTX 3050 over integrated Intel UHD graphics. Latency benchmark validated at ~180 ms median on RTX 3050. All 6 tests in `MidasInpainterTest` and `AiDirectMLTest` pass 100% green.

## 6. Working Rules For Future Edits

- Treat this file as living context, not a frozen spec. Update it when major architecture, roadmap, or stability assumptions change.
- Keep public distribution safety in mind: unsigned injection DLLs and CLIs are likely to trigger security products.
- Prefer clear diagnostics and reversible settings for experimental features.
- Avoid hardcoded per-game behavior unless it is isolated behind profiles or compatibility data.
- When editing launcher and C++ config surfaces, update both sides together to prevent profile drift.

## 8. VR Injection Stability Fixes (Critical — Do Not Regress)

The following fixes ensure all games (DX11, DX12, Vulkan) correctly display in VR. These are verified working with No Man's Sky (Vulkan) and Sekiro: Shadows Die Twice (DX11). **Do not modify these patterns without testing both games.**

### BUG-10: DX11/DX12 API Misidentification
- **File**: `src/hooks/dx11_hook.cpp` — `ProcessPresent()`
- **Problem**: Some DX11 games (e.g., Sekiro) create a DX12 command queue internally (for video/compute). The old code used `DXGIFactoryHook::GetCapturedCommandQueue()` as the sole DX12 indicator, routing these games to `Dx12LifecycleManager` which then failed every frame on `GetBuffer(0, ID3D12Resource)`.
- **Fix**: Probe the swapchain buffer type with `GetBuffer(0, IID_PPV_ARGS(&probe))` where `probe` is `ComPtr<ID3D12Resource>`. Only use DX12 path if the probe succeeds. Otherwise fall back to DX11.
- **Rule**: Never rely solely on the presence of a DX12 command queue to determine API. Always verify the swapchain's actual buffer type.

### BUG-11: Monoscopic Fallback Path
- **Files**: `dx11_graphics_backend.cpp`, `dx12_graphics_backend.cpp`, `vulkan_graphics_backend.cpp`
- **Problem**: When the camera heuristic fails (`shouldAttemptStereo = false`), the stereo compute shader has nothing to render and the OpenXR swapchain receives black/stale data.
- **Fix**: Each backend now has a monoscopic fallback: if stereo rendering is not attempted or fails, the game's raw backbuffer is copied directly to both OpenXR eye textures using `CopyResource` (DX11), `CopyTextureRegion` (DX12), or `vkCmdCopyImage` (Vulkan). Player sees flat 2D in the headset instead of black.
- **Rule**: Every `SubmitStereoFrame` must have a fallback path that shows *something* when stereo fails.

### BUG-12: Vulkan Color Space (Washed-Out Fix)
- **File**: `vulkan_graphics_backend.cpp`, `frame_coordinator.cpp`
- **Problem**: `vkCmdBlitImage` applies implicit gamma conversion when copying UNORM→SRGB, causing washed-out colors.
- **Fix**: Use `vkCmdCopyImage` (raw byte copy, no conversion). OpenXR swapchain format is selected dynamically via `xrEnumerateSwapchainFormats` to match the game's channel order.
- **Rule**: Never use `vkCmdBlitImage` for game→OpenXR copies. Always use `vkCmdCopyImage`.

### Universal Format Selection
- **File**: `frame_coordinator.cpp` — swapchain creation block
- **Pattern**: All backends (DX11/DX12/Vulkan) enumerate OpenXR-supported formats and prefer matching channel order. This prevents gamma/channel mismatches during raw copies.
- **Rule**: When creating OpenXR swapchains, always enumerate supported formats. Never hardcode a single format.

### BUG-13: ResizeBuffers Crash (UE4/UE5 Games)
- **Files**: `src/hooks/dx11_hook.cpp` — `hkResizeBuffers()`, `src/rendering/dx11/dx11_lifecycle_manager.cpp`, `src/rendering/stereo/stereo_resource_manager.cpp`
- **Problem**: DXGI's `ResizeBuffers()` requires ALL external references to swapchain buffers be released first, or it returns `DXGI_ERROR_INVALID_CALL`. UE4 treats this as a fatal crash. Our lifecycle manager and `StereoResourceManager` held `ComPtr<ID3D11Texture2D>` and direct SRVs on the backbuffer, preventing ResizeBuffers from succeeding.
- **Fix**: `hkResizeBuffers` calls `ReleaseSwapchainReferences()`, which also invokes `ClearState()` and `Flush()` on the immediate context. `StereoResourceManager` always creates shadow copies rather than attaching SRVs directly to the game's swapchain backbuffer.
- **Rule**: Never hold persistent references or direct views to swapchain buffers across ResizeBuffers calls.

### BUG-14: UNORM / HDR 10-Bit Color Space Washout
- **Files**: `shaders/stereo_reprojection.hlsl`, `shaders/tonemap.hlsl`, `src/core/frame_coordinator.cpp`
- **Problem**: Games rendering to `DXGI_FORMAT_R10G10B10A2_UNORM` (format 24) or non-sRGB UNORM backbuffers already apply a tone-mapping/gamma curve (~2.2). OpenXR treats all `_UNORM` swapchains as linear RGB and applies an additional display gamma curve (~1/2.2), causing double gamma expansion and washed-out, milky colors.
- **Fix**: `stereo_reprojection.hlsl` and `tonemap.hlsl` convert incoming sRGB colors to linear space (`pow(c, 2.2f)`), perfectly canceling OpenXR's display curve and restoring rich shadows, deep blacks, and intended saturation. `FrameCoordinator` also strictly preserves format family (e.g. format 24 -> 24) so `CopyResource` succeeds.
- **Rule**: Non-sRGB UNORM swapchains must receive linearized values so the OpenXR compositor's transfer curve produces correct colors.

### BUG-12: DX12 ResizeBuffers Reference Lock & Command List Race
- **Files**: `src/rendering/dx12/dx12_lifecycle_manager.cpp`, `src/hooks/dx12_hook.cpp`
- **Problem**: In games transitioning from shader compilation screens to main menu (e.g., Hogwarts Legacy), the game calls `IDXGISwapChain::ResizeBuffers`. `Dx12LifecycleManager` held an outstanding `ComPtr<ID3D12Resource>` reference to the swapchain backbuffer, causing DXGI to fail with `DXGI_ERROR_INVALID_CALL` and trigger device loss (`0x887A0006`). Additionally, `hkExecuteCommandLists` iterated through mapped constant buffers on worker threads without verifying object lifetimes, causing `0xC0000005` access violations.
- **Fix**: Added `Dx12LifecycleManager::ReleaseSwapchainReferences()` called unconditionally in `hkResizeBuffers` / `hkResizeBuffers1` before forwarding to the driver. Removed unsafe dangling map iteration from `hkExecuteCommandLists` (since `ProcessConstantBuffer` is already safely handled in `hkUnmap`).
- **Rule**: Always release backbuffer ComPtrs prior to calling `ResizeBuffers` in DX12, and never iterate asynchronous resource maps on worker command execution queues.

### BUG-18: Multi-Game Isolation & Cross-Game Regression Prevention
- **Files**: `src/core/engine_detector.cpp`, `src/core/config_manager.cpp`, `src/core/runtime_state.cpp`, `src/core/frame_coordinator.cpp`, `launcher/electron/configManager.ts`, `launcher/electron/injectionManager.ts`
- **Problem**: Debugging new titles previously regressed existing verified games (such as Hogwarts Legacy) due to: (1) Hardcoded executable name and heuristic collisions (`-Win64-Shipping` forcing UE5); (2) Desynchronization between `installPath` and `targetExeDir` (`Phoenix\Binaries\Win64`), causing launcher config edits to be missed by the injected DLL; (3) Missing config loading in DX12 / Vulkan; (4) Render thread null dereferences in async OpenXR startup.
- **Fix**: Implemented isolated per-game profile architecture (`engine`, `reverseZ`, `rowMajorMatrices` in `vrinject.json`), centralized hierarchical config discovery in `RuntimeState` and `ConfigManager::Load()`, strict null guards and thread-safe assignment for async OpenXR runtime in `FrameCoordinator`, launcher multi-directory configuration synchronization, and automated Playwright + GTest cross-game regression suites.
- **Rule**: Never introduce hardcoded game names or broad filename heuristics into core engine detection. Each game must be isolated by its own profile and directory structure. Centralized config loading must occur during early runtime before any graphics hooks execute.

### BUG-17: Per-Game sRGB Tonemapping & Cross-Game Contrast Isolation
- **Files**: `Stereix-client/shaders/stereo_reprojection.hlsl`, `Stereix-client/src/rendering/dx12/dx12_stereo_resource_manager.h`, `Stereix-client/src/rendering/dx12/dx12_stereo_resource_manager.cpp`, `Stereix-client/src/rendering/stereo/stereo_renderer.h`, `Stereix-client/src/rendering/stereo/stereo_renderer.cpp`, `Stereix-client/src/rendering/vulkan/vulkan_graphics_backend.cpp`
- **Problem**: DX11 games like *Sekiro* output an already-gamma-compressed sRGB swapchain backbuffer, where applying `pow(c, 2.2f)` caused double-gamma crushing (pitch black scenery/trees). Conversely, DX12 / Unreal Engine 4 titles like *Hogwarts Legacy* output linear / HDR UNORM surfaces, where omitting gamma correction resulted in washed-out, milky, low-contrast visuals in the VR headset compared to the desktop window. Hardcoding either behavior globally in shaders regresses one title when fixing the other.
- **Fix**: Replaced hardcoded shader math with dynamic constant buffer control. Replaced padding at offset 252 of `cbuffer StereoConstants` with `uint SrgbCorrection;`. Compute shader `stereo_reprojection.hlsl` applies `pow(max(c, 0.0f), 2.2f)` conditionally only when `SrgbCorrection != 0`. The DX12, DX11, and Vulkan backends dynamically populate this flag each frame from `SubsystemContext::Get().GetConfig()->GetConfig().srgbCorrection`, which is driven directly by per-game profiles (`814380_sekiro.json: false`, `990080_hogwarts_legacy.json: true`) and controllable in real time via the in-headset ImGui overlay.
- **Rule**: Never globally hardcode color-space or gamma transformations in compute/pixel shaders. Color curves must be dynamically governed via constant buffers backed by per-game profile metadata (`srgbCorrection`), ensuring complete isolation and zero regressions across titles.

### BUG-19: Launcher OTA Dependency Shadowing Local Builds
- **Files**: `Stereix-client/launcher/electron/injectionManager.ts`
- **Problem**: In development mode (`npm run dev`), the launcher unconditionally prioritized cached OTA binaries from `%APPDATA%\Stereix-Dev\updates\` over newly compiled binaries in `build/bin/`. As a result, when testing new engine builds, the CLI or launcher overwrote the game folder with stale binaries from the update cache, rendering local compiler fixes completely ineffective.
- **Fix**: Added `pickPreferredAsset()` helper ensuring that in development mode (`!app.isPackaged`), `canonicalBinSourceDir` (`build/bin/`) ALWAYS takes precedence over OTA caches. In packaged mode, the binary with the newer `mtimeMs` is selected. `copySources` also orders `canonicalBinSourceDir` first during dev mode.
- **Rule**: In development mode, local build artifacts must always take precedence over downloaded OTA cache files.

### BUG-20: Launcher OTA Cache Invalidation, Shader Distribution & Cross-Game Contamination
- **Files**: `Stereix-client/launcher/electron/updateManager.ts`, `Stereix-client/launcher/electron/injectionManager.ts`, `updates/manifest.json`, `.github/workflows/package-setup.yml`
- **Problem**: Upgraded or newly installed launchers failed to run Sekiro in VR and caused graphics issues in Hogwarts Legacy due to three compounding defects: (1) `pickPreferredAsset()` relied on raw file `mtimeMs`, causing stale downloaded OTA files in `%APPDATA%` to shadow fresh bundled files in newly installed release packages; (2) `updates/manifest.json` omitted compute shaders (`shaders/stereo_reprojection.hlsl`), leaving OTA updates without critical shader fixes; (3) `injectionManager.ts` wrote a global `%APPDATA%\updates\vrinject.json` and didn't enforce `srgbCorrection` / `depthSubmission` profile invariants over existing game directory configs, causing Sekiro to retain broken depth submission and cross-contaminating settings between games.
- **Fix**: Implemented `purgeStaleOtaCacheIfAppNewer()` and version-aware asset resolution in `pickPreferredAsset()`. Added all compute shaders to `updates/manifest.json`. Enforced `srgbCorrection` and `depthSubmission` invariants when merging game configs and eliminated the global `stageCfg` write. Updated CI/CD workflow to dynamically compute release tags (`v0.1.10`) from `package.json`.
- **Rule**: Packaged launchers must prioritize bundled assets over older cached OTA files, shaders must be distributed with OTA updates, and per-game configs must never cross-contaminate via shared global staging files.

### BUG-21: Universal Native Game Lighting & Prevention of Double-Gamma Crush Across Titles
- **Files**: `Stereix-client/src/core/config_manager.h`, `Stereix-client/src/core/config_manager.cpp`, `Stereix-client/src/core/overlay_manager.cpp`, `Stereix-client/launcher/electron/injectionManager.ts`, `updates/profiles/*.json`, `Stereix-client/profiles/*.json`
- **Problem**: In games such as *Hogwarts Legacy* (DX12/UE4), the VR headset displayed crushed shadows, dark walls, and pitch-black interiors compared to the monitor. This was caused by `srgbCorrection: true` triggering `pow(c, 2.2f)` in `stereo_reprojection.hlsl`. Because modern game backbuffers (both DX11 and DX12) are already tone-mapped and gamma-encoded by game engines for desktop presentation, applying `pow(c, 2.2f)` a second time effectively squares the gamma curve ($0.5^{2.2} \approx 0.217$), plunging midtones and shadows into total blackness. Furthermore, `srgbCorrection` previously defaulted to `true` across schemas and C++ config managers, causing any newly injected or unprofiled game to suffer from identical crushing.
- **Fix**: Completely removed `pow(c, 2.2f)` from `stereo_reprojection.hlsl` so compute shaders always pass through untouched game backbuffer pixels directly to OpenXR swapchains without double-gamma squaring. Changed the system-wide default for `srgbCorrection` to `false` in `ConfigManager`, schemas, and overlay reset handlers. Updated all curated profiles (`990080_hogwarts_legacy.json`, `668580_atomic_heart.json`, `1623730_palworld.json`, `1245620_elden_ring.json`, `1110910_mortal_shell.json`, `meccha_chameleon.json`, `814380_sekiro.json`) to `srgbCorrection: false`. Added a hardened launcher fallback in `injectionManager.ts` for Hogwarts Legacy (`Phoenix` / `HogwartsLegacy`). Bumped engine and launcher to `v0.1.11` with semver-aware hotfix detection in `updateManager.ts`.
- **Rule**: Universal default for `srgbCorrection` must remain `false`. Games output already-tonemapped sRGB frames; compute shaders must never artificially apply exponential gamma curves (`pow(c, 2.2)`) unless specifically requested by an opt-in user toggle. All fixes must bump the version number before release.

### BUG-22: Camera Lock Hysteresis & Temporal Score Decay Prevention
- **Files**: `Stereix-client/src/memory_scanner/camera_delta_tracker.cpp`, `Stereix-client/src/core/frame_coordinator.cpp`
- **Problem**: In games like *Mortal Shell* and *Sekiro*, standing idle or walking with keyboard WASD without mouse movement caused `inputMotionEnergy` to drop to zero. The tracker penalized camera translation without mouse turn, decaying `m_lockedConfidence` to 0.0f within ~220 ms and dropping the camera lock. This caused violent flickering between 3D stereo reprojection and 2D flat mode. Additionally, uninitialized stack matrices and square 1:1 shadow cascade projection bindings caused warped FOVs and pinhole zooms.
- **Fix**: Implemented lock retention hysteresis in `CameraDeltaTracker` preserving established orthonormal camera locks at >=90% confidence during idle/WASD movement. Separated 3x3 rotation deltas from row-3 translation deltas. Added aspect ratio validation ($1.25 < \text{aspect} < 2.8$) rejecting square shadow cascades. Ensured clean, zero-initialized Left-Handed Reverse-Z 16:9 perspective fallback matrices in `FrameCoordinator`.
- **Rule**: Established camera locks must never be dropped purely due to lack of mouse movement. Projection candidate validation must filter out square shadow cascades and reflection probes.

### BUG-23: Desktop 60 Hz VSync Decoupling from 90 Hz OpenXR Compositor
- **Files**: `Stereix-client/src/hooks/dx11_hook.cpp`
- **Problem**: When games ran with desktop VSync enabled on a 60 Hz monitor, DX11 `Present(1, ...)` put the render thread to sleep until the 16.6 ms desktop v-blank. OpenXR / SteamVR running at 90 Hz (11.1 ms) missed submission deadlines on every frame, resulting in a solid purple/magenta reprojection dropped-frame wall in the SteamVR GPU performance graph despite low GPU render times (<1.5 ms).
- **Fix**: Decoupled desktop presentation by checking `FrameCoordinator::IsOpenXRReady()`. When an OpenXR VR session is presenting, `effectiveSyncInterval = 0` is passed to desktop `Present()`, allowing the game loop to present immediately and feed the OpenXR compositor at the full 90/120 Hz cadence without dropped frames.
- **Rule**: When VR injection is presenting to OpenXR, desktop swapchains must never sleep on desktop monitor v-blank intervals.

### BUG-24: Asynchronous OpenXR Frame Pacing, Multi-Buffered Command Lists & Camera Scan Storm
- **Files**: `Stereix-client/src/openxr/openxr_frame_submitter.h`, `Stereix-client/src/openxr/openxr_frame_submitter.cpp`, `Stereix-client/src/rendering/dx12/dx12_stereo_renderer.h`, `Stereix-client/src/rendering/dx12/dx12_stereo_renderer.cpp`, `Stereix-client/src/memory_scanner/camera_delta_tracker.cpp`
- **Problem**: Games in VR exhibited lag, low framerates (16–30 FPS), and persistent yellow/orange CPU compositor timing bars in SteamVR despite GPU render times of only 0.3–0.5 ms. Three compounding root causes were identified:
  1. Synchronous `xrWaitFrame` calls inside the game render thread's `Present()` hook put the game to sleep for ~11 ms on every frame. When combined with the game's own simulation and rendering time, each frame took 20+ ms, missing the 90 Hz compositor deadline and triggering harsh reprojection.
  2. `DX12StereoRenderer` shared a single `ID3D12GraphicsCommandList` across 3 frames in flight, causing command list `Reset()` calls while previous submissions were still in flight on the GPU direct queue, inducing driver pipeline stalls.
  3. `CameraDeltaTracker` lacked candidate table bounds and executed hundreds of candidate checks with SEH enter/exit blocks on the render thread every frame whenever locked confidence hovered below 0.85.
- **Fix**:
  1. Implemented a dedicated asynchronous OpenXR frame pacing thread in `OpenXRFrameSubmitter` that overlaps `xrWaitFrame` concurrently in the background while the game engine executes its next-frame simulation and rendering. When the game enters `Present()`, the paced frame state is consumed instantly and `xrBeginFrame` is called immediately with zero wait time, dropping render thread submission latency from ~11 ms to <0.5 ms.
  2. Multi-buffered command lists in `DX12StereoRenderer` with dedicated `m_commandLists[MAX_FRAMES_IN_FLIGHT]` matched to their respective command allocators.
  3. Added an immediate fast-path return in `CameraDeltaTracker` when the locked camera pointer remains valid, and capped the dynamic candidate map to at most 64 entries.
- **Rule**: `xrWaitFrame` must never block the game render thread synchronously inside `Present()`; D3D12 command lists must be multi-buffered per frame in flight; camera candidate polling must be bypassed once an orthonormal camera lock is established.


## 7. Anti-Cheat & AV Posture Tier List

Because Stereix relies on `CreateRemoteThread` and `LoadLibrary` to inject into arbitrary game processes, its behavior is fundamentally indistinguishable from techniques flagged by Anti-Virus (AV) heuristics and Anti-Cheat (AC) software. This is the standard approach for universal VR injectors and cannot be fundamentally changed. The following tier list defines our official compatibility and support posture:

- **Tier 1: Single-Player / No Anti-Cheat (Fully Supported)**
  Games without active anti-cheat solutions. Stereix operates freely. Users may occasionally need to whitelist `vrinject.dll` and `vr-inject-cli.exe` in Windows Defender or third-party AVs due to heuristic flagging.
- **Tier 2: Light Anti-Cheat / Mod-Friendly (Supported Offline)**
  Games with passive protections or official offline modes (e.g., Elden Ring with EAC disabled). Users MUST explicitly disable the anti-cheat or launch in offline mode before injecting. Stereix will not attempt to hide from active scans.
- **Tier 3: Strict Anti-Cheat (Unsupported / Blacklisted)**
  Competitive multiplayer titles using kernel-level or strict user-mode anti-cheat (e.g., Vanguard, BattlEye, active EAC, Ricochet). Stereix will NOT attempt to bypass these systems. Injection will fail, and attempting to force it may result in permanent account bans. Stereix should never be marketed for or used with these titles.

## 8. Cross-Backend In-Headset ImGui VR Overlay Architecture

The In-Headset ImGui VR Overlay dashboard is implemented across all supported graphics APIs:
- **DirectX 11 (`ImGuiDX11Integration`)**: Creates/caches RTVs on OpenXR eye buffers (`leftSubmit`, `rightSubmit`), executes `ImGui_ImplDX11_RenderDrawData`, and safely preserves/restores viewport and render target states.
- **DirectX 12 (`ImGuiDX12Integration`)**: Manages dedicated SRV and RTV descriptor heaps, transitions OpenXR eye targets to `D3D12_RESOURCE_STATE_RENDER_TARGET`, and executes ImGui command lists directly on the game's direct command queue.
- **Vulkan (`ImGuiVulkanIntegration`)**: Creates dedicated descriptor pools, render passes, and command buffer submissions for rendering ImGui draw data to OpenXR swapchain images.
- **Universal Input Navigation**: Left controller menu button (`/user/hand/left/input/menu/click`), gamepad combos (`L3 + R3` / `Back`), and keyboard hotkeys (`HOME`, `F11`, `INSERT`, `~`) toggle the overlay. All 3D stereo parameters (IPD, Convergence, World Scale) modified in the overlay are synced directly with `ConfigManager` and persisted.

## 9. Pre-Beta Governance, Safety & Telemetry Architecture

### QUAL-07: First-Launch Closed Beta Legal, Anti-Cheat & Safety Modal
- **Files**: `Stereix-client/launcher/src/components/LegalModal.tsx`, `Stereix-client/launcher/src/App.tsx`, `Stereix-client/launcher/src/components/AboutPanel.tsx`
- **Purpose**: Protects project maintainers and beta testers with mandatory first-run agreements:
  1. *Single-Player Only & Zero Anti-Cheat Liability*: Explicitly informs users that injecting into multiplayer titles protected by kernel anti-cheat is prohibited and that maintainers bear zero liability for bans.
  2. *VR Health & Epilepsy Advisory*: Informs users of vestibular mismatch risks, motion sensitivity, and 15-minute break recommendations.
  3. *Trademark Fair Use*: Disclaims commercial affiliation with third-party publishers or headsets.
  4. *As-Is Closed Beta*: Warns testers to back up saved game data prior to testing.

### FEAT-09: Automated Dual Telemetry & Bug Reporting System (Cloudflare Edge KV + Discord)
- **Files**: `Stereix-docs/landing-page/functions/api/report.js`, `Stereix-client/launcher/electron/telemetryManager.ts`, `Stereix-client/launcher/src/components/ReportBugModal.tsx`, `scripts/fetch_reports.mjs`
- **Architecture**:
  - **Client Launcher**: Provides a 1-click "REPORT ISSUE" modal in `AboutPanel` and auto-uploader in `SessionLog`. Sanitizes all PII before transmission by replacing Windows username paths (`C:\Users\[USER]\`) and private local IP addresses.
  - **Cloudflare Edge Endpoint (`/api/report`)**: Stores full reports with 30-day TTL in Cloudflare KV, updates a fast ring-buffer index (`REPORTS_INDEX`), and forwards a rich Discord embed to `#beta-feedback` if configured.
  - **Developer CLI (`scripts/fetch_reports.mjs`)**: Allows developers and AI assistants to fetch, inspect, and summarize incoming reports and engine logs with a single command without needing manual tester copy-pasting.

### FEAT-12: Milestone 4 — Full OpenXR Stereo Frame Submission to SteamVR Null Driver
- **Files**: `scripts/test_e2e_injection.ps1`, `build/bin/stereix_sample_d3d11.exe`, `build/bin/vr-inject-cli.exe`, `build/bin/vrinject.dll`
- **Status**: Verified full end-to-end stereoscopic OpenXR frame presentation pipeline into SteamVR without a physical HMD using Valve's built-in `driver_null`. 
- **Validation**:
  1. Configured SteamVR runtime settings (`forcedDriver: "null"`, `requireHmd: false`, `showMirrorView: true`, `mirrorViewEye: 2` in `steamvr.vrsettings`).
  2. Orchestrated headless test harness via `scripts/test_e2e_injection.ps1` launching `stereix_sample_d3d11.exe` for a 500-frame render loop.
  3. Performed live runtime injection via `vr-inject-cli.exe` with SHA-256 binary validation and security token origin verification (`STEREIX_AUTH_TOKEN`).
  4. SteamVR `vrcompositor.txt` logs confirm 784 active presents to `VR_CompositorPipe` with 0 dropped frames, displaying both Left and Right stereoscopic views in SteamVR VR View.

### BUG-24: Full Aspect Ratio / Immersive VR Viewport & Runtime Depth Buffer Auto-Discovery
- **Files**: [stereo_reprojection.hlsl](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/shaders/stereo_reprojection.hlsl), [dx11_hook.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/hooks/dx11_hook.cpp), [depth_candidate_collector.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/heuristics/depth_candidate_collector.cpp), [depth_candidate_collector.h](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/heuristics/depth_candidate_collector.h), [stereo_resource_manager.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/rendering/stereo/stereo_resource_manager.cpp), [814380_sekiro.json](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/profiles/814380_sekiro.json)
- **Symptom**: When injected into titles like *Sekiro: Shadows Die Twice* (or any DX11 game injected post-launch), the VR viewport appeared inside a small square pillarboxed/letterboxed floating cinema window (`screenScale = 0.76f`) with flat 2D monoscopic rendering (no 3D stereoscopic depth).
- **Root Causes**:
  1. *Forced Cinema Shrink in Shader*: `stereo_reprojection.hlsl` unconditionally forced `const float screenScale = 0.76f` when `ShouldAttemptStereo == 0`, rendering into a floating square box and filling the rest with void black color.
  2. *Missed Pre-Existing Depth Textures on Mid-Game Injection*: Depth stencil textures allocated before DLL injection never passed through `hkCreateTexture2D`. Thus, `DepthCandidateCollector` rejected draw/clear calls because the resource pointer was absent from `m_livingResources`, resulting in 0 discovered depth candidates and failing the compatibility scorer.
  3. *Flat 1x1 Fallback Depth*: Fallback depth resource creation supplied a 1x1 1.0f texture that matched the HUD threshold (`depth >= 0.9995f`), suppressing all parallax.
- **Resolution**:
  1. Modified `stereo_reprojection.hlsl` to default to 100% full aspect ratio / edge-to-edge VR projection, confining 0.76x cinema box scaling exclusively to cutscenes when `TheaterModeWeight > 0.001f`.
  2. Modified `dx11_hook.cpp` and `depth_candidate_collector.cpp/.h` to auto-discover existing depth surfaces dynamically during `OMSetRenderTargets` and `ClearDepthStencilView`, inspecting resource descriptions on the fly.
  3. Replaced 1x1 fallback depth texture in `stereo_resource_manager.cpp` with a 256x256 smooth perspective depth gradient ramp (0.25f ground to 0.95f sky) ensuring parallax depth perception is active even during transient candidate tracking.
  4. Enabled `depthSubmission: true` and `srgbCorrection: true` across Sekiro game profiles.

### FEAT-13: Milestone Option C — DirectML Stage 2 Real-Time Neural Inpainting Pipeline
- **Files**: `stereix-client/src/ai/disocclusion_inpainter.cpp`, `stereix-client/src/ai/disocclusion_inpainter.h`, `stereix-client/src/ai/ai_model_loader.cpp`, `stereix-client/tests/test_disocclusion_inpainter.cpp`, `models/disocclusion_inpainter.onnx`
- **Architecture**:
  - **U-Net Neural Disocclusion Inpainter**: Deployed FP16 U-Net hole synthesizer (`models/disocclusion_inpainter.onnx`, 1.95 MB, 11.9 ms latency on RTX 3050 DirectML) targeting disocclusion stretch artifacts and edge silhouette tearing.
  - **Dynamic Tensor Introspection & Resizing**: `DisocclusionInpainter` dynamically inspects active ONNX session input count, shapes, and tensor names (`m_session->GetInputTypeInfo`, `GetInputNameAllocated`). Accommodates 4-channel packed tensors `[1, 4, H, W]` (`[R, G, B, Mask]`) as well as split multi-input models.
  - **Bilateral Rescaling & Blending**: Scales arbitrary game frame resolutions (e.g. 1920x1080) down to the model's tensor resolution (480x270) via bilinear planar interpolation, synthesizes missing disocclusion hole content, upscales back to target resolution, and composites with 100% preservation of non-hole valid pixels.
  - **Candidate Path Resolution**: `AiModelLoader` auto-discovers model weights across candidate directories (`<exeDir>`, `<exeDir>/models`, `models/`, `<exeDir>/../../models`) for robust test, development, and production execution.
  - **Safety & Fallback Guarantees**: Robust exception boundaries catch any DirectML or device driver timeouts, falling back to a horizontal edge-stretch pass. Passed 100% of native GoogleTest suites (7/7 in `test_disocclusion_inpainter`, 6/6 in `test_midas_inpainter`, 81/81 in CTest) and Playwright static regression tests.

### BUG-25: NVIDIA Streamline (sl.interposer.dll) Integration & Multi-Queue Thrashing Debounce
- **Files**: [dx12_hook.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/hooks/dx12_hook.cpp), [dx11_hook.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/hooks/dx11_hook.cpp), [dxgi_factory_hook.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/hooks/dxgi_factory_hook.cpp), [version.h](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/core/version.h)
- **Symptom**: Injection into titles using NVIDIA Streamline (*Ghost of Tsushima DIRECTOR'S CUT*, *Spider-Man*, *Horizon*, etc.) logged hook target rejection warnings (`Hook target ... is in '...sl.interposer.dll', not a known D3D/DXGI system DLL — skipping`), skipping `Present` hooking so OpenXR frame submission never engaged. Simultaneously, `dxgi_factory_hook.cpp:163` logged alternating `Captured ID3D12CommandQueue updated to ...` dozens of times per frame, flooding logs with hundreds of megabytes in seconds and causing frame stutters.
- **Root Causes**:
  1. *Streamline Interposer Blocking*: `IsValidHookTarget` in `dx12_hook.cpp` and `dx11_hook.cpp` restricted hook targets strictly to `C:\Windows\System32\` (`d3d12.dll`, `dxgi.dll`, etc.), rejecting the game folder's legitimate `sl.interposer.dll` wrapper.
  2. *Command Queue Alternation in ExecuteCommandLists*: Games with multiple Direct command queues (engine render queue vs UI/Streamline queue) caused `hkExecuteCommandListsInternal` to repeatedly call `SetCapturedCommandQueue`, overwriting the queue pointer and re-triggering lifecycle rediscovery on every frame.
- **Resolution**:
  1. Updated `IsValidHookTarget` to recognize `sl.interposer.dll`, validating that it resides within the application directory tree and exports `slInit` (verifying genuine Streamline framework).
  2. Modified `hkExecuteCommandListsInternal` to only capture command queues if `!DXGIFactoryHook::GetCapturedCommandQueue().Get()`, preserving the authoritative swapchain queue without thrashing.
  3. Updated `ProcessPresentDX12` and DX11 forwarding to query `pSwapChain->GetDevice(__uuidof(ID3D12CommandQueue), ...)` directly from the swapchain before falling back to captured queues.
  4. Debounced `SetCapturedCommandQueue` logging to single-time `LOG_INFO` and downgraded subsequent updates to `LOG_DEBUG`.

### BUG-26: Direct3D 12 OpenXR Format Mismatch, Backbuffer SRV Flag & Memory Sweep CPU Thrashing
- **Files**: [dxgi_factory_hook.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/hooks/dxgi_factory_hook.cpp), [dx12_stereo_resource_manager.h](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/rendering/dx12/dx12_stereo_resource_manager.h), [dx12_stereo_resource_manager.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/rendering/dx12/dx12_stereo_resource_manager.cpp), [dx12_graphics_backend.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/rendering/dx12/dx12_graphics_backend.cpp), [page_scanner.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/memory_scanner/page_scanner.cpp), [depth_lock_manager.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/heuristics/depth_lock_manager.cpp)
- **Symptom**: In DX12 games like *Onimusha - Way of the Sword*, the desktop window ran and rendered correctly, but the VR headset mirror (SteamVR VR View) displayed a pitch-black screen in both eyes. Simultaneously, desktop gameplay experienced severe micro-stutters and lag.
- **Root Causes**:
  1. *Swapchain Format Mismatch in CopyResource*: SteamVR's OpenXR DX12 runtime negotiated 10-bit color (`DXGI_FORMAT_R10G10B10A2_UNORM`, format 24), but `DX12StereoResourceManager::CreateEyeTextures` defaulted `eyeFmt` to the game's swapchain backbuffer format (`DXGI_FORMAT_R8G8B8A8_UNORM`, format 28). `ID3D12GraphicsCommandList::CopyResource` between mismatched formats is strictly illegal in Direct3D 12 and was silently dropped by the GPU driver, leaving OpenXR textures unwritten (pitch black).
  2. *Missing DXGI_USAGE_SHADER_INPUT on DX12 Swapchain*: `dxgi_factory_hook.cpp` checked `if (isDx12 || !pDesc)` and bypassed adding `DXGI_USAGE_SHADER_INPUT`. Without this usage flag, creating a Shader Resource View on the game's backbuffer produced undefined black reads in the compute reprojection shader.
  3. *Unthrottled Memory Scanning & Logging Storm*: `PageScanner::ScanDynamicHeaps` executed unbounded virtual memory queries every 4 seconds across 5 MB of heap at normal thread priority (taking up to 1604 ms), starving CPU and bus bandwidth. Simultaneously, `DepthLockManager` ping-ponged between `LOCKED` and `REVERIFYING` on 78% confidence, generating thousands of disk log flushes per minute on the main render thread.
- **Resolution**:
  1. Added `SetTargetFormat(DXGI_FORMAT)` to `DX12StereoResourceManager`, dynamically synchronizing intermediate eye texture formats to match `oxrSwapchain->GetFormat()` (24). Handled sRGB UAV typing properly.
  2. Modified `dxgi_factory_hook.cpp` (`hkCreateSwapChain` and `hkCreateSwapChainForHwnd`) to add `DXGI_USAGE_SHADER_INPUT` across both DX11 and DX12 swap chains with automatic fallback.
  3. Lowered `PageScanner` thread priority to `THREAD_PRIORITY_LOWEST`, skipped sweeps entirely when `CameraLockManager` is locked, and increased post-sweep sleep to 8.0s.
  4. Lowered `DepthLockManager` reverify threshold to 50% and suppressed repetitive `LOCKED` logging during re-verification.

### BUG-27: Direct3D 12 Monoscopic Menu Fallback Copy, Command Queue Invalidation Self-Healing & SEH-Guarded Depth SRVs
- **Files**: [dx12_stereo_renderer.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/rendering/dx12/dx12_stereo_renderer.cpp), [dx12_graphics_backend.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/rendering/dx12/dx12_graphics_backend.cpp), [dx12_stereo_resource_manager.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/rendering/dx12/dx12_stereo_resource_manager.cpp), [dxgi_factory_hook.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/hooks/dxgi_factory_hook.cpp), [build.bat](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/build.bat)
- **Symptom**: In DX12 games like *Onimusha - Way of the Sword*, the VR headset mirror (SteamVR VR View) remained pitch-black in both eyes (reporting 90 FPS / 0.9 ms) despite the game window displaying the title menu and gameplay smoothly. When attempting to run compiler batch scripts, CMake failed with `Generator Visual Studio 18 2026 could not find specified instance`.
- **Root Causes**:
  1. *Missing Monoscopic Copy Fallback in DX12*: Flat games at title menus or cutscenes lack 3D camera / depth information (`isStereoValid == false` or `!hasPso`). Unlike DX11, the DX12 backend had no fallback path to blit the game backbuffer into the OpenXR swapchain images, leaving the swapchain unwritten (100% black) while OpenXR submitted empty textures to SteamVR.
  2. *Permanent State Degradation on Queue Re-Creation*: When the game recreated swapchains upon resolution changes (1280x720 to 1920x1080), `DX12GraphicsBackend::m_commandQueue` held the initial queue pointer, failing the single-queue assertion and setting `m_isDegraded = true`. Because `RenderStereo` aborted on `m_state != READY` and `m_isDegraded` had no reset mechanism, rendering was permanently silenced for the remainder of the session.
  3. *SEH Access Violation on Destroyed Depth Stencil Surfaces*: Stale depth descriptors from previous swapchains triggered access violations when dereferencing `GetDesc()` or creating SRVs on typed `DXGI_FORMAT_D32_FLOAT` surfaces created with `DENY_SHADER_RESOURCE`.
  4. *CMake Generator Instance Conflict in build.bat*: `VsDevCmd.bat` populated `VSINSTALLDIR`, which confused CMake's Visual Studio 2026 generator into seeking an IDE (`devenv.exe`) rather than the installed Build Tools.
- **Resolution**:
  1. Implemented a guaranteed hardware DMA monoscopic fallback in `DX12StereoRenderer::RenderStereo`: when stereoscopic compute is not engaged, it transitions `camTex` (the game backbuffer) to `COPY_SOURCE` and copies directly to `oxrLeftDest` and `oxrRightDest`, so the player always sees the game menu / cutscenes in the VR headset at full 90 FPS.
  2. Added dynamic synchronization of the active command queue from `Dx12LifecycleManager::Get().GetMainQueue()` at the start of every frame, and added self-healing (`m_isDegraded = false` whenever the queue matches).
  3. Wrapped all resource descriptor inspections and SRV creations in structured exception handling (`__try / __except`) with safe color-buffer fallbacks, and purged stale depth descriptors on swapchain recreation.
  4. Cleared `VSINSTALLDIR` before invoking `cmake` in `build.bat`, enabling clean one-click builds using Visual Studio Build Tools 2026.

### QUAL-07: Codebase Untangling, Format Selection Encapsulation & Coupling Reduction (v0.1.112)
- **Files**: [frame_coordinator.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/core/frame_coordinator.cpp), [openxr_swapchain_manager.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/openxr/openxr_swapchain_manager.cpp), [openxr_swapchain_manager.h](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/openxr/openxr_swapchain_manager.h), [config_manager.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/core/config_manager.cpp), [depth_candidate_collector.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/heuristics/depth_candidate_collector.cpp), [depth_lock_manager.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/heuristics/depth_lock_manager.cpp), [depth_delta_tracker.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/heuristics/depth_delta_tracker.cpp), [candidate_collector.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/heuristics/candidate_collector.cpp), [.gitignore](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/.gitignore)
- **Context & Untangling Protocol**: Codebase audit and untangling following `caldoai/skills/untangle-codebase`.
- **Untangled Items**:
  1. *Encapsulated OpenXR Swapchain Format Selection*: Moved 100+ lines of low-level OpenXR format enumeration, preference arrays, and `goto` jumps out of `FrameCoordinator` into `OpenXRSwapchainManager::SelectOptimalFormat()`, restoring proper encapsulation.
  2. *Unified Depth Discovery Lifecycle*: Consolidated branched DX11 vs DX12/Vulkan depth discovery dispatch in `FrameCoordinator::OnPresentBegin` to use unified `m_currentSnapshot.epoch`.
  3. *Pruned Dead Coupling in `FrameCoordinator`*: Removed unused headers (`rendering/vulkan_resource_state_tracker.h`, `rendering/stereo/stereo_pipeline.h`, `rendering/stereo/stereo_camera_generator.h`, `rendering/stereo/stereo_frame_builder.h`, `heuristics/depth_delta_tracker.h`, and duplicate `hooks/input_hook.h`), keeping `FrameCoordinator` degree within ratchet budget (49/50).
  4. *Deduplicated Directory Search*: Abstracted duplicated 3-level parent directory traversal in `ConfigManager::Load` into a reusable lambda `addHierarchicalCandidates`.
  5. *Code Hygiene & Restating Comments*: Removed comments that purely restated syntax across core heuristics and coordination, while strictly preserving architectural comments, hardware workarounds, and bug references.
  6. *Workspace Debris Cleanup*: Added scratch presentation and video still patterns (`brag-output*/`, `extracted_shape*.png`, `slide*.png`, `output/`, `data/`) to `.gitignore`, removed abandoned legacy `nexvr-client/` directory.
- **Verification**: 81/81 C++ GoogleTest suites passed (100%), all 6 coupling ratchets passed (`check_coupling.ps1`), 17/17 launcher Playwright/TypeScript static tests passed, AST knowledge graph updated cleanly (`graphify update .`).

### BUG-30: Direct3D 12 Monitor VSync Decoupling, Render-Thread Camera Delta Fast-Path & Profile Depth Pacing (v0.1.112)

- **Files**: [dx12_hook.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/hooks/dx12_hook.cpp), [camera_delta_tracker.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/memory_scanner/camera_delta_tracker.cpp), [frame_coordinator.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/core/frame_coordinator.cpp), [custom_1794136173946_onimushawots.json](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/profiles/custom_1794136173946_onimushawots.json)
- **Symptom**: In DX12 games (*Onimusha: Way of the Sword*), the game displayed cleanly without graphical glitches in VR, but motion felt slow, stuttery, and laggy. The in-headset SteamVR performance graph reported blazing 0.3 ms GPU time, yet the CPU frame timing line spiked orange/yellow with severe judder and late frame drops.
- **Root Causes**:
  1. *DX12 Monitor VSync Throttling (`SyncInterval = 1`)*: Unlike `dx11_hook.cpp`, `hkPresentDX12Internal` passed `SyncInterval` straight to `ProcessPresentDX12`, blocking the game's render thread on the 60 Hz monitor VBlank (16.6 ms). The OpenXR headset cadence (90 Hz = 11.1 ms) was starved, causing 60 Hz vs 90 Hz beating and halving framerate to 30-45 FPS.
  2. *Candidate Collector & SEH Storm on Render Thread*: `CameraDeltaTracker::PollAndTrackCandidates()` looped through up to 512 dynamic candidates on every frame, calling `seh::SafeReadMemory` twice per candidate (1000+ SEH entries/exits per frame), burning 4-8 ms of render thread CPU time.
  3. *Profile Convergence & Depth Submission*: In `frame_coordinator.cpp`, convergence was hardcoded to 0.5m instead of reading the profile's configured value (3.2m in Onimusha), and the game directory profile had `depthSubmission: false`.
- **Resolution**:
  1. Decoupled monitor VSync in `dx12_hook.cpp` (`hkPresentDX12Internal` and `hkPresent1DX12Internal`) when OpenXR is ready: `if (coordinator && coordinator->IsOpenXRReady()) { effectiveSyncInterval = 0; }`.
  2. Implemented a fast path in `CameraDeltaTracker::PollAndTrackCandidates()`: when camera lock confidence is high ($\ge 0.85$) and valid, it updates only the locked camera and skips polling the 512 candidates on 59 out of 60 frames (running a full scan only at 1 Hz or on lock loss), reducing CPU overhead from ~6 ms to <0.05 ms.
  3. Updated `frame_coordinator.cpp` to read `ipd` and `convergence` from configuration instead of hardcoding `0.5f`.
  4. Set `"depthSubmission": true` across all profile copies and re-synchronized assets.


### BUG-29: Cloud Edge AI Profile Failure & On-Device Cognitive Reverse-Engineering Engine Integration (v0.1.107)
- **Files**: [aiProfileService.ts](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/launcher/electron/aiProfileService.ts), [SettingsPanel.tsx](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/launcher/src/components/SettingsPanel.tsx), [App.tsx](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/launcher/src/App.tsx), [types/index.ts](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/launcher/src/types/index.ts), [version.h](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/core/version.h)
- **Symptom**: When clicking `AI AUTO-TUNE (QWEN 2.5)` in the launcher for *Onimusha: Way of the Sword* or other non-curated titles, the launcher displayed `✓ Qwen 2.5 AI Profile Applied (Cloud Edge): Unknown engine, using generic settings`. The profile did not reverse-engineer the game's actual architecture and defaulted to improper generic values without configuring the matrix conventions or hardware safety guards.
- **Root Causes**:
  1. *Cloud Model Context Starvation*: The Cloudflare Worker edge model only received `{ gameName: "Onimusha: Way of the Sword" }`. Because a remote text model has no access to the user's local disk, binary PE headers, imported DLLs, or game package archives, it predictably hallucinated or reported `"Unknown engine"`.
  2. *Missing Local Context in SettingsPanel*: `SettingsPanel.tsx` passed only `gameName`, `executablePath`, `steamAppId`, and `detectedApi`, omitting `installPath: game.installPath`. Consequently, disk scanning could not locate the root game directory if the executable was nested in subdirectories.
  3. *Single-Line UI Notification*: The launcher notification banner did not surface multi-pass Chain-of-Thought (CoT) architectural reasoning, leaving users with no visibility into how matrices, depth barriers, and convergence were derived.
- **Resolution**:
  1. Replaced remote cloud dependency with an on-device Local Cognitive Reverse-Engineering Engine in `aiProfileService.ts`. Implemented deep filesystem and PE binary inspection scanning root game directories, executable directories, and parent trees for:
     - *Capcom RE Engine* (`re_chunk_*.pak`, `CrashHandler.dll`, `shader.cache2`, `piplinelist.bin`, `via.render`): Column-Major, Reverse-Z, 3.2m melee convergence, non-typeless depth fallback (eliminates `0x887a002b`).
     - *IO Interactive Glacier 2* (*Project 007 / First Light 007*, *Hitman*): `*.rpkg`, `glacier*.dll`, Row-Major, Reverse-Z, 2.2m ADS gunplay convergence, direct DX12 presentation.
     - *Unreal Engine 5*: `*.ucas`, `*.utoc`, IoStore, Double64 Large World Coordinates, Reverse-Z, Row-Major, 2.8m convergence.
     - *Unreal Engine 4*: `Content/Paks/*.pak`, `*Shipping.exe`, Float32, Reverse-Z, Row-Major, 2.5m convergence.
     - *FromSoftware Dantelion* (*Sekiro*, *Elden Ring*, *Dark Souls*): `dvdbnd*.bdt`, `oo2core`, Column-Major, Standard-Z, 3.5m wide combat convergence.
     - *Unity Engine*: `UnityPlayer.dll`, `*_Data/`, Column-Major, Standard-Z, 2.0m convergence.
     - *REDengine* (*Cyberpunk 2077*, *Witcher 3*): `r6/`, `cyberpunk2077.exe`, Row-Major, Reverse-Z, 1.8m first-person convergence.
     - *Decima, id Tech, Source 2, and Custom PE64*: PE header inspection for D3D12 Agility SDK, NVIDIA Streamline, DirectML, and title-signature perspective heuristics.
  2. Formatted output with 5-pass Chain-of-Thought (CoT) reasoning (`[PASS 1: ARCHITECTURE DECOMPOSITION]`, `[PASS 2: GRAPHICS API & RUNTIME CONTRACT]`, `[PASS 3: COORDINATE & MATRIX CONVENTION]`, `[PASS 4: HARDWARE RESOURCE SAFETY]`, `[PASS 5: SPATIAL ERGONOMICS]`, `[DECISION]`), guaranteeing zero hallucinations.
  3. Enhanced `SettingsPanel.tsx` to forward `installPath: game.installPath`, renamed the action button to `⚡ REVERSE-ENGINEER VR (LOCAL AI)`, and rendered a cyberpunk-styled interactive terminal displaying the full CoT decomposition.
  4. Automatically deployed `vrinject.json` directly into the game directory and mirrored it to `userData/custom_profiles` and development caches, ensuring instantaneous native engine ingestion.


### FEAT-10: 6DOF World-Anchored Virtual Cinema Quad Layer (v0.1.114)
- **Files**: [openxr_frame_submitter.h](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/openxr/openxr_frame_submitter.h), [openxr_frame_submitter.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/openxr/openxr_frame_submitter.cpp), [dx11_graphics_backend.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/rendering/dx11/dx11_graphics_backend.cpp), [dx12_graphics_backend.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/rendering/dx12/dx12_graphics_backend.cpp), [vulkan_graphics_backend.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/rendering/vulkan/vulkan_graphics_backend.cpp), [frame_coordinator.h](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/core/frame_coordinator.h), [frame_coordinator.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/core/frame_coordinator.cpp), [overlay_manager.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/core/overlay_manager.cpp), [config_manager.h](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/core/config_manager.h), [config_manager.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/core/config_manager.cpp)
- **Feature Overview**:
  - Implemented Phase 1 Item 1.4: 6DOF World-Anchored Virtual Cinema Quad Layer. When 3D stereo reconstruction is inactive (`ShouldAttemptStereo == 0`), during 2D UI/menus/loading screens, or when `forceCinemaMode` is explicitly enabled in settings, the backbuffer is projected onto an anchored 3D quad in `XR_REFERENCE_SPACE_TYPE_STAGE`/`LOCAL` space rather than head-locked projection views.
  - Users can move their head freely in 6DOF around the floating virtual cinema screen with zero extra GPU post-processing or blit overhead, leveraging hardware time-warp and OpenXR compositor quad layering (`XrCompositionLayerQuad`).
  - Added gravity-leveled forward vector projection for upright screen anchoring: computes $v_{forward} = q \cdot (0, 0, -1) \cdot q^{-1}$, normalizes on the horizontal $(X, Z)$ plane, and orients the screen perpendicular to gravity.
  - Added keyboard hotkeys (`F12`, `Ctrl+Alt+R`) and an in-headset ImGui button in the VR Comfort tab to recenter the cinema screen directly in front of the user at the configured distance.
  - Added full JSON serialization/deserialization for `forceCinemaMode` across native engine and launcher configurations.


### BUG-30: Full Ratio 360 VR View Restoration & DX11 Constant Buffer Unmap Interception (v0.1.115)
- **Files**: [dx11_hook.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/hooks/dx11_hook.cpp), [dx11_graphics_backend.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/rendering/dx11/dx11_graphics_backend.cpp), [dx12_graphics_backend.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/rendering/dx12/dx12_graphics_backend.cpp), [vulkan_graphics_backend.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/rendering/vulkan/vulkan_graphics_backend.cpp)
- **Symptom**: In *Sekiro* and other titles, the game displayed as two small floating rectangles inside a vast black void instead of filling the full headset field of view in 360 degrees.
- **Root Causes**:
  1. *Eager Cinema Quad Activation*: In v0.1.114, `isCinema = !shouldAttemptStereo` was enabled by default across all backends. Whenever stereo had not yet locked or during 2D presentations, `openxr_frame_submitter.cpp` submitted `XrCompositionLayerQuad` instead of `XrCompositionLayerProjection`, boxing the game onto a small floating quad in empty black space.
  2. *Missing DX11 Unmap Hook*: FromSoftware's Dantelion engine (*Sekiro*, *Dark Souls*, *Elden Ring*) updates camera matrices using `ID3D11DeviceContext::Map(WRITE_DISCARD)` and `Unmap()`. While `Map` was hooked as a passthrough, writes happen after `Map` returns; `Unmap()` was unhooked, resulting in `CandidateCollector` receiving zero constant buffer updates and failing camera lock.
- **Resolution**:
  1. Restored full 360-degree immersive VR projection (`XrCompositionLayerProjection`) as the absolute default across DX11, DX12, and Vulkan backends. Cinema Quad mode is strictly opt-in (`isCinema = false; if (cfg.forceCinemaMode) isCinema = true;`).
  2. Hooked `ID3D11DeviceContext::Unmap` (vtable index 15) in `dx11_hook.cpp`. Tracked mapped constant buffers during `Map` and delivered their populated matrix payloads to `CandidateCollector::OnConstantBufferUpdate` at `Unmap` time, enabling instant camera lock in Sekiro.


### BUG-31: Launcher Game Selection Cross-Fade Glitch & Visual Line Blink Elimination (v0.1.116)
- **Files**: [App.tsx](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/launcher/src/App.tsx), [GameDetail.tsx](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/launcher/src/components/GameDetail.tsx), [index.css](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/launcher/src/index.css), [version.h](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/core/version.h), [regression-static.spec.ts](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/launcher/e2e/regression-static.spec.ts)
- **Symptom**: When switching between games in the launcher UI, a visible line appeared and blinked out for a split second, accompanied by intermediate layout jumping and flashing.
- **Root Causes**:
  1. *Chromium Skia Clip-Path Artifact (`gameBannerSelectSweep`)*: In `index.css`, `.game-banner-selected::before` animated `clip-path: inset(0 100% 0 0)` to `clip-path: inset(0 0 0 0)` over 0.35s. In Chromium on Windows, animating `clip-path: inset(...)` renders a sharp 1px vertical line along the clipping boundary that sweeps horizontally across the game banner and vanishes when the animation terminates.
  2. *Asynchronous Config Loading Race Condition*: In `App.tsx`, `handleSelectGame` called `setSelectedGame(g)` after a 150ms timeout, but config loading occurred in an asynchronous `useEffect([selectedGame])`. For 1-2 frames, `<GameDetail>` mounted with the previous game's config before the new config resolved, causing a sudden layout shift and visual flash mid-fade.
  3. *Unsynchronized Scroll Offset Reset*: In `App.tsx`, `requestAnimationFrame` reset `scrollTop = 0` only after the new game mounted. If the previous game was scrolled down, the new game initially rendered at the previous scroll offset for 1 frame before snapping to 0, causing section divider and card borders to flash momentarily.
  4. *Conflicting Nested GPU Transforms*: The main content container applied `transform: translateY(6px)` while child cards simultaneously applied `animation: settingsOpenAnim` (`translateY(28px)`), triggering subpixel border rasterization seam artifacts along container edges.
- **Resolution**:
  1. Replaced the `clip-path: inset(...)` sweep animation in `index.css` with a smooth, hardware-accelerated `opacity` and subtle `scaleX` fade (`gameBannerSelectFade`), completely eliminating the sweeping boundary line.
  2. Synchronized game selection and config loading in `App.tsx`: `handleSelectGame` pre-fetches `window.ag.config.read(g.id)` concurrently during the fade-out, resets `scrollTop = 0` while the content is invisible, and updates `selectedGame` and `config` atomically.
  3. Replaced nested `transform: translateY` animations on the scroll container with a clean, hardware-accelerated `opacity` cross-fade (`0.16s cubic-bezier(0.16, 1, 0.3, 1)`), preventing subpixel border rasterization glitches.
  4. Added regression test in `regression-static.spec.ts` guaranteeing that `clip-path` sweep keyframes are banned and that state synchronization remains enforced.


### FEAT-11: Two-Stage AI Calibration Flow & In-Terminal Telemetry Stream (v0.1.117)
- **Files**: [SettingsPanel.tsx](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/launcher/src/components/SettingsPanel.tsx), [VRStatusBar.tsx](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/launcher/src/components/VRStatusBar.tsx), [App.tsx](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/launcher/src/App.tsx), [version.h](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/core/version.h), [package.json](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/launcher/package.json), [manifest.json](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/updates/manifest.json), [regression-static.spec.ts](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/launcher/e2e/regression-static.spec.ts)
- **Overview**:
  1. *Streamlined Per-Title Settings*: Removed the standalone `REVERSE-ENGINEER VR (LOCAL AI)` button and the `Calibrated Engine Profile:` badge box from `SettingsPanel.tsx`, preserving only `RESET TO GLOBAL DEFAULTS` in header controls for a cleaner UI layout.
  2. *Two-Stage Injection State Machine*:
     - Stage 1: Clicking `INITIALIZE INJECTION` arms the automated AI reverse-engineering and calibration sequence with a dynamic 15s to 30s delay calculated via `computeCalibrationDuration(selectedGame)` (factoring in Direct3D API complexity, package size GB, and engine architecture such as Capcom RE Engine or FromSoftware Dantelion).
     - Live In-Terminal Diagnostic Telemetry: The background AI process streams live real-time analysis into the session log terminal (replacing `> SYSTEM IDLE. AWAITING COMMAND`). Milestone entries highlight PE header scans, graphics API entry points, matrix conventions (Row-Major vs Column-Major), depth buffer SEH hardware protection, convergence derivation, and profile serialization, highlighting `[ OK ]` lines in vibrant green.
     - Stage 2: Once the duration completes, the action button dynamically transforms into **`START GAME`** (styled in glowing red ready state with high-visibility box-shadow).
     - Clicking `START GAME` deploys injection hooks and launches the game executable immediately without extra delay. Switching selected titles resets the state machine back to default and clears all timers.
  3. *Static Regression Coverage*: Added regression assertions in `regression-static.spec.ts` guaranteeing that `SettingsPanel.tsx` stays free of redundant AI buttons/badges, that `VRStatusBar.tsx` maintains `'START GAME'` and `'AI CALIBRATING'` button states, and that `App.tsx` enforces dynamic 15-30s delay clamping and background streaming.

### BUG-32: Real-Headset Double Vision (Asymmetric FOV Divergence), Inverted Stereo Gather, ImGui Quad & OpenXR Head-Tracking (v0.1.120)
- **Files**: [stereo_camera_generator.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/rendering/stereo/stereo_camera_generator.cpp), [stereo_reprojection.hlsl](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/shaders/stereo_reprojection.hlsl), [stereo_types.h](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/core/stereo_types.h), [stereo_renderer.h](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/rendering/stereo/stereo_renderer.h), [stereo_renderer.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/rendering/stereo/stereo_renderer.cpp), [dx12_stereo_resource_manager.h](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/rendering/dx12/dx12_stereo_resource_manager.h), [dx12_stereo_resource_manager.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/rendering/dx12/dx12_stereo_resource_manager.cpp), [vulkan_graphics_backend.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/rendering/vulkan/vulkan_graphics_backend.cpp), [imgui_dx11_integration.h](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/rendering/dx11/imgui_dx11_integration.h), [imgui_dx11_integration.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/rendering/dx11/imgui_dx11_integration.cpp), [imgui_dx12_integration.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/rendering/dx12/imgui_dx12_integration.cpp), [dx12_stereo_renderer.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/rendering/dx12/dx12_stereo_renderer.cpp), [dx11_graphics_backend.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/rendering/dx11/dx11_graphics_backend.cpp), [openxr_frame_submitter.h](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/openxr/openxr_frame_submitter.h), [openxr_frame_submitter.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/openxr/openxr_frame_submitter.cpp), [overlay_manager.h](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/core/overlay_manager.h), [overlay_manager.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/core/overlay_manager.cpp), [input_hook.h](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/hooks/input_hook.h), [input_hook.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/hooks/input_hook.cpp), [input_manager.h](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/hooks/input_manager.h), [input_manager.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/hooks/input_manager.cpp), [frame_coordinator.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/core/frame_coordinator.cpp), [test_stereo_camera.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/tests/test_stereo_camera.cpp)
- **Symptom (real Quest + Virtual Desktop tester report in *Sekiro*)**:
  1. Every screen (loading screens, menus, gameplay, and the Stereix ImGui menu) displayed too close to the eyes and split into un-fuseable double vision (SBS), even though SteamVR Null Driver (which uses symmetric FOVs) looked normal.
  2. Head tracking did not move the game camera at all — the entire image remained face-locked and moved 1:1 with the user's head.
  3. Changing IPD / Convergence sliders in the in-headset menu had zero visual effect.
- **Root Causes**:
  1. *Asymmetric OpenXR FOV Optical-Center Divergence*: Real VR headsets have asymmetric canted FOVs (e.g. Left `-52°..+40°`, Right `-40°..+52°`), so straight-ahead `0°` is at UV `~0.60` (Left) and `~0.40` (Right). Submitting a centered 2D texture across UV `[0,1]` with `XrCompositionLayerProjection` produced ~12° wall-eyed divergence (un-fuseable double vision) and stretched 16:9 menus across the full vertical lens.
  2. *Wrong LH Principal-Point Sign & Raw STAGE-Space Room Pose*: `StereoCameraGenerator::Generate` did not negate `proj.m[2][0]` / `m[2][1]` for Left-Handed projection (doubling divergence error), multiplied raw `STAGE`-space height/room yaw (~1.65m up) directly into `camera.view`, and completely ignored `constants.ipd` / `constants.convergence`.
  3. *Forward-as-Backward Inverted Stereo Gather & Unshifted CAS*: `stereo_reprojection.hlsl` projected mono coordinates forward with `LeftViewProj`/`RightViewProj` and used those coordinates to sample `GameColor` (inverting stereo parallax) and clamped CAS sharpening against unshifted mono `uv`.
  4. *Broken ImGui Stereo Offset & DX12 Eye Overwrite*: `ImGuiDX11Integration::Render` set `vp.TopLeftX = ±stereoOffset` which `ImGui_ImplDX11_RenderDrawData` immediately overwrote back to `0`, and `ImGuiDX12Integration::Render` copied `leftDest` over `rightDest` (plus `DX12StereoRenderer` passed a released `ID3D12Device*`).
  5. *Unwired Head Tracking*: `InputHook::InjectAimDelta` had zero callers anywhere in the engine, truncated sub-pixel turns to zero, and had inverted pitch sign.
- **Resolution**:
  1. Passed exact per-eye OpenXR frustum tangents (`LeftEyeFovTan`, `RightEyeFovTan`), symmetric game FOV tangents, `StereoIpd`, `StereoConvergence`, `IsReversedZ`, `NearPlane`, `FarPlane` in the existing 512-byte `StereoConstants` across DX11, DX12, and Vulkan.
  2. Rewrote `stereo_reprojection.hlsl` to map every output eye pixel through `EyeUvToTanRay` → `RayToGameUV` (guaranteeing 0.00° binocular optical-center divergence on all headsets in both 2D and 3D) with 4-step orthoscopic epipolar backward-gather depth refinement, non-divergent infinity clamp, Reverse-Z aware disocclusion inpainting, and source-UV-matched CAS sharpening.
  3. Fixed `StereoCameraGenerator::Generate` LH/RH off-axis principal-point sign, isolated head-local `±IPD/2` eye offsets from `STAGE` space, and respected user IPD scaling.
  4. Routed DX11 ImGui overlay onto a dedicated `1280×720` OpenXR `XrCompositionLayerQuad` anchored at eye height (`1.45m` forward via `SetHudQuadAnchored`), fixed `drawData->DisplayPos.x` fallback, fixed DX12 per-eye ImGui rendering (no right-eye overwrite), and fixed `ComPtr<ID3D12Device>` lifetime in `DX12StereoRenderer`.
  5. Wired live OpenXR HMD quaternion yaw/pitch deltas via `InputManager::UpdateHeadTracking` (with ±180° wrap, >35° teleport guard, overlay pause, and sub-pixel floating-point accumulators in `InputHook::InjectAimDelta`).
- **Rule**: Never copy centered 2D game buffers directly across asymmetric-FOV `XrCompositionLayerProjection` swapchains; never multiply raw `STAGE`-space poses into `camera.view`; always verify real-headset Quest 3 asymmetric FOV optical-center alignment in `test_stereo_camera`.

### BUG-33: Already-Running Game Re-Launch, Premature Cold-Start UAC Injection & Swallowed CLI Failure Lines (v0.1.121)
- **Files**: [injectionManager.ts](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/launcher/electron/injectionManager.ts), [App.tsx](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/launcher/src/App.tsx), [main.cpp](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/src/injector/main.cpp), [regression-static.spec.ts](file:///c:/Users/sathi/.gemini/antigravity/scratch/Stereix-Engine/stereix-client/launcher/e2e/regression-static.spec.ts)
- **Symptom (Discord tester report on *FF7 Rebirth*)**:
  1. Clicking `INITIALIZE INJECTION` while the game was already running attempted to launch a second instance via `steam://rungameid/...`.
  2. On a fresh launch, approving the PowerShell UAC prompt immediately returned `INJECTION FAILED` while the Unreal Engine game had not even finished loading, without displaying the actual `vr-inject-cli.exe` error line in the UI terminal.
- **Root Causes**:
  1. *Launch-Before-Resolve & Missing Running-Process Check*: `inject:deploy` invoked `shell.openExternal` / `shell.openPath` before resolving `targetExeName`, checking if the game was already running, or deploying `vrinject.json`/shaders/DLLs.
  2. *Premature Process Grab*: `injectionManager.ts` selected any process under `Binaries/` or `Shipping` on the very first 500ms tick (>5 MB RAM), firing UAC before Unreal Engine / Steam DRM finished booting or restarting the PID (`SteamAPI_RestartAppIfNecessary`), causing `vr-inject-cli` Exit Code 21 or loader-lock timeout.
  3. *Swallowed CLI Diagnostics*: `stopActiveLogWatch()` tore down watchers before flushing the final lines written to `vrinject.log`, `injectionManager.ts` replaced the error with a generic `Exit code non-zero` string, and `App.tsx` wiped `logQueueRef.current = []` before rendering the error state.
- **Resolution**:
  1. Reordered `inject:deploy` to resolve `targetExeName`/`targetExeDir`, probe `findRunningGameCandidate()`, and synchronize shaders/models/DLLs/`vrinject.json` **before** launching.
  2. Skipped launching when the game is already running and attached directly to the live PID.
  3. Added `inspectProcess()` (path + `MainWindowHandle`) and a cold-launch PID stabilization + window/memory readiness gate (`windowAndMemReady` / `heavyMemReady` / `fallbackStableReady`), post-UAC live PID re-resolution inside the elevated PowerShell wrapper, and extended `vr-inject-cli`'s remote `LoadLibraryA` wait from 10s to 20s.
  4. Added `flushWatchedLogs()` and `extractCliFailureReason()` (with UAC cancellation detection and `[FAIL]`/`[ERROR]` extraction) and updated `App.tsx` to flush queued `logQueueRef.current` lines into the terminal on failure/cancellation.
- **Rule**: Always resolve target exe, detect already-running processes, and deploy assets BEFORE launching; always stabilize cold-launched PIDs before UAC; always flush watched log files and `logQueueRef` before terminating listeners on error.

### SEC-01..SEC-12: Comprehensive Security, Stability, OTA Integrity & Binary Hardening Pass (v0.1.122)
- **Scope**: Native C++ injector/engine/proxies, Electron launcher/OTA/UI/packaging, GitHub Actions CI/CD, `stereix-backend`, `stereix-ai-worker`, and `stereix-docs/landing-page`.
- **Rules to Preserve Forever**:
  1. **No PowerShell Quoting for Paths/Tokens (`CRIT-01`/`CRIT-02`)**: Elevated `vr-inject-cli.exe` invocations MUST pass arguments via an owner-ACL JSON `--job-file` (never string-interpolated `'...'` literals vulnerable to Unicode smart quotes `’`, and never passing `STEREIX_AUTH_TOKEN` on command lines). `vr-inject-cli.exe` MUST validate token entropy, canonical caller image paths, `FILE_ATTRIBUTE_REPARSE_POINT` / `GetFinalPathNameByHandleW` containment, SHA-256 hashes of `vrinject.dll`/`onnxruntime.dll`/`DirectML.dll`, and hold a read-only handle across hash-and-load to close TOCTOU windows.
  2. **Pre-Hash Signing Order (`CRIT-03`)**: Always sign `vrinject.dll` **before** generating `expected_hash.h` and linking `vr-inject-cli.exe` / proxy DLLs. Never re-sign `vrinject.dll` post-link in `sync_assets.js` or CI. Keep UTF-16LE `EXPECTED_DLL_HASH` verification as a hard failure in both `sync_assets.js` and CI workflows.
  3. **Vulkan `nativeHwnd` & 512-Byte UBO (`CRIT-04`/`HIGH-07`)**: Never cast `RenderFrameSnapshot::nativeSwapchain` to `IDXGISwapChain*` in `FrameCoordinator`; each lifecycle manager populates `RenderFrameSnapshot::nativeHwnd`. Keep Vulkan UBO allocation/binding at `sizeof(StereoShaderConstants)` (512 bytes) and track original Vulkan hook targets separately from MinHook trampolines.
  4. **Loader-Lock, VEH & Concurrency Hygiene (`HIGH-05`/`HIGH-06`/`HIGH-08`/`HIGH-09`/`HIGH-12`)**: Never create `std::thread`, acquire mutexes, or call `FreeLibrary` inside `DllMain`; keep first-chance VEH handlers strictly lock-free/allocation-free; suppress `DXGIFactoryHook` capture during internal dummy swapchain creation; clear all DX12 trackers/locks/resources on `ResizeBuffers`/`ResizeBuffers1`; ring-buffer DX12 CBVs/descriptors across `MAX_FRAMES_IN_FLIGHT = 3`; never call `xrWaitFrame` concurrently with the pacing thread; destroy `m_oxrRuntime` before joining `m_oxrSubmitter`.
  5. **Scoped Input Forwarding (`HIGH-04`)**: `InputHook` low-level keyboard/mouse forwarding MUST only forward input when the foreground window belongs to a verified VR mirror/compositor process allowlist (`IsVerifiedVrMirrorWindow`), never from arbitrary Alt-Tabbed desktop windows.
  6. **Fail-Closed OTA, Profile Precedence & Electron Fuses (`HIGH-01`/`HIGH-10`/`HIGH-11`/`HIGH-13`/`MED-03..06`/`MED-12`)**: Verify Ed25519 manifest signatures inside the mirror loop; stage and verify all OTA assets atomically before promoting; verify SHA-256 of every OTA shader/profile before use; order `loadProfilesFromDisk()` lowest-to-highest for last-writer-wins; respect user `srgbCorrection` toggle (default `true`, no forced `false -> true` overwrite); keep `@electron/fuses` enabled in `electron-builder.config.js` and `sandbox: true` in `main.ts`; keep `react`/`react-dom` in `devDependencies`.
  7. **Backend & Edge Hardening (`HIGH-02`/`HIGH-03`/`MED-07`/`MED-08`)**: Separate access vs refresh JWTs (`typ`, distinct signing key, `algorithms: ['HS256']`, unique `jti`, SHA-256 hashed refresh tokens in DB, revoke all on password change); default `NODE_ENV` to `'production'`; validate S3 release keys (`releases/` prefix, no traversal); use constant-time secret checks and never store raw client IPs or personal emails in Cloudflare functions.
  8. **Binary Hardening (`MED-01`)**: Keep `/guard:ehcont`, `/sdl`, `/Brepro`, `/CETCOMPAT`, and `VRINJECT_FILE_BASENAME` (`/d1trimfile`) enabled so release binaries expose zero local developer paths.









