#include "core/frame_coordinator.h"
#include <system_error>
#include <vector>
#include "rendering/dx11/dx11_graphics_backend.h"
#include "rendering/dx12/dx12_graphics_backend.h"
#include "rendering/vulkan/vulkan_graphics_backend.h"
#include "rendering/dx11/dx11_resource_validator.h"
#include "core/logger.h"

#include "memory_scanner/camera_delta_tracker.h"
#include "memory_scanner/page_scanner.h"
#include "heuristics/camera_lock_manager.h"
#include "heuristics/camera_classifier.h"
#include "heuristics/camera_ranking_engine.h"
#include "heuristics/camera_validator.h"
#include "heuristics/candidate_collector.h"
#include "heuristics/depth_candidate_collector.h"
#include "heuristics/depth_lock_manager.h"
#include "core/diagnostic_context.h"
#include "core/subsystem_context.h"
#include "core/engine_detector.h"
#include "hooks/input_hook.h"
#include "core/overlay_manager.h"
#include <windows.h>

#include "ai/backend/dx11_ai_backend.h"
#include "ai/backend/dx12_ai_backend.h"
#include "ai/backend/vulkan_ai_backend.h"

namespace vrinject {

void FrameCoordinator::OnPresentBegin(const RenderFrameSnapshot &snapshot) {
  if (m_frameActive) {
    LOG_WARN("FrameCoordinator: Re-entrant OnPresentBegin detected! Forcing "
             "EndFrame.");
    OnPresentEnd();
  }

  ScopedCpuTimer frameTimer(&m_cpuProfiler, CpuSegment::EntireFrame);

  m_currentSnapshot = snapshot;
  m_frameActive = true;
  m_globalFrameCounter++;

  m_stateMonitor.BeginFrame(m_globalFrameCounter);

  if (m_currentSnapshot.backend == GraphicsBackend::DX11) {
    m_gpuProfiler.BeginFrame(
        static_cast<ID3D11DeviceContext *>(m_currentSnapshot.nativeContext),
        m_globalFrameCounter);

    auto validation = Dx11ResourceValidator::Validate(m_currentSnapshot,
                                                      m_currentSnapshot.epoch);
    m_stateMonitor.UpdateDx11Health(validation.status ==
                                    Dx11ValidationStatus::VALID);
    if (validation.status != Dx11ValidationStatus::VALID) {
      SubsystemContext::Get().GetCameraLockManager()->Reset();
      SubsystemContext::Get().GetDepthLockManager()->OnDeviceLost();
      return;
    }
  } else if (m_currentSnapshot.backend == GraphicsBackend::DX12) {
    m_stateMonitor.UpdateDx11Health(true);
  } else if (m_currentSnapshot.backend == GraphicsBackend::Vulkan) {
    m_stateMonitor.UpdateDx11Health(true);
  }

  if (!m_engineDetected) {
    SubsystemContext::Get().GetEngineDetector()->Detect();
    m_engineDetected = true;
    
    // Phase 6: Initialize dynamic memory scanner
    if (SubsystemContext::Get().GetPageScanner()->Initialize()) {
        SubsystemContext::Get().GetPageScanner()->StartDynamicScan(90.0f); // Default 90 FOV
    }
  }

  if (m_globalFrameCounter == 1) {
    LOG_INFO("[OK] Graphics Pipeline Hooked: %ux%u (API: %s)",
             m_currentSnapshot.width, m_currentSnapshot.height,
             m_currentSnapshot.backend == GraphicsBackend::DX11 ? "DirectX 11" :
             m_currentSnapshot.backend == GraphicsBackend::DX12 ? "DirectX 12" : "Vulkan");
  } else if (m_globalFrameCounter % 600 == 0) {
    LOG_INFO("[STATUS] VR Session Active — Frame %llu (1920x1080 @ 90 FPS)", m_globalFrameCounter);
  }

  // Initialize OverlayManager on first frame with window HWND
  static bool s_overlayInitialized = false;
  if (!s_overlayInitialized) {
      HWND hwnd = nullptr;
      if (m_currentSnapshot.nativeSwapchain) {
          IDXGISwapChain* sc = reinterpret_cast<IDXGISwapChain*>(m_currentSnapshot.nativeSwapchain);
          DXGI_SWAP_CHAIN_DESC desc = {};
          if (SUCCEEDED(sc->GetDesc(&desc))) {
              hwnd = desc.OutputWindow;
          }
      }
      OverlayManager::GetInstance().Initialize(hwnd);
      if (hwnd) {
          InputHook::GetInstance().SetTargetHwnd(hwnd);
      }
      s_overlayInitialized = true;
  }

  // Keyboard shortcut toggle for In-Headset Dashboard (HOME, INSERT, F11)
  static bool s_lastKeyboardToggle = false;
  bool keyboardPressed = ((GetAsyncKeyState(VK_HOME) & 0x8000) != 0) ||
                         ((GetAsyncKeyState(VK_INSERT) & 0x8000) != 0) ||
                         ((GetAsyncKeyState(VK_F11) & 0x8000) != 0);
  if (keyboardPressed && !s_lastKeyboardToggle) {
      OverlayManager::GetInstance().ToggleOverlay();
      LOG_INFO("OverlayManager: In-headset menu toggled via keyboard shortcut (Visible: %s)",
               OverlayManager::GetInstance().IsOverlayVisible() ? "YES" : "NO");
  }
  s_lastKeyboardToggle = keyboardPressed;

  // Keyboard shortcut for Virtual Cinema Screen Recentering (F12 or Ctrl+Alt+R)
  static bool s_lastRecenterToggle = false;
  bool recenterPressed = ((GetAsyncKeyState(VK_F12) & 0x8000) != 0) ||
                         (((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) &&
                          ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) &&
                          ((GetAsyncKeyState('R') & 0x8000) != 0));
  if (recenterPressed && !s_lastRecenterToggle) {
      RequestCinemaRecenter();
      LOG_INFO("FrameCoordinator: Virtual Cinema Screen recentered in front of headset gaze.");
  }
  s_lastRecenterToggle = recenterPressed;

  // Camera and depth discovery - wrapped in try-catch because these subsystems
  // may crash on DX12 backends (they were designed for DX11). A crash here must
  // NOT prevent OpenXR initialization.
  CameraSnapshot camSnapshot;
  DepthSnapshot depthSnapshot;
  try {
    std::vector<CameraCandidate> candidates;
    {
      ScopedCpuTimer camTimer(&m_cpuProfiler, CpuSegment::CameraDiscovery);
      candidates = SubsystemContext::Get().GetCandidateCollector()->GetAndClearCandidates();

      for (auto &c : candidates) {
        c.temporalScore = 0.5f;
        if (!CameraValidator::ValidateCandidate(c)) {
          c.valid = false;
        }
      }

      // Phase 9: Feed headset tracking pose into the camera tracker
      m_deltaTracker.UpdateHeadsetPose(m_cachedHeadsetPose);

      // Phase 6: Poll dynamic candidates and track them
      m_deltaTracker.PollAndTrackCandidates();
      
      CameraCandidate bestCandidate = {};
      bool hasBest = false;
      
      Matrix4x4 dynamicViewMatrix = {};
      Matrix4x4 dynamicProjMatrix = {};
      if (m_deltaTracker.GetLockedCamera(dynamicViewMatrix, dynamicProjMatrix)) {
          // Dynamic scanner found a locked camera matrix!
          bestCandidate.id = 0xC0CA3E01; // Non-zero unique ID so CameraLockManager accepts it
          bestCandidate.view = dynamicViewMatrix;

          // If projection matrix is missing, square (shadow/cube), or invalid, synthesize a mathematically
          // clean perspective projection matching the actual viewport aspect ratio (e.g. 1920x1080 -> 16:9)
          if (!PageScanner::IsValidProjectionMatrixFloat(&dynamicProjMatrix.m[0][0])) {
              float width = static_cast<float>(m_currentSnapshot.width > 0 ? m_currentSnapshot.width : 1920);
              float height = static_cast<float>(m_currentSnapshot.height > 0 ? m_currentSnapshot.height : 1080);
              float aspect = width / height;
              float fovHRad = 90.0f * (3.1415926535f / 180.0f);
              float tanHalfFovH = std::tan(fovHRad * 0.5f);
              float tanHalfFovV = tanHalfFovH / aspect;

              dynamicProjMatrix = {};
              dynamicProjMatrix.m[0][0] = 1.0f / tanHalfFovH;
              dynamicProjMatrix.m[1][1] = 1.0f / tanHalfFovV;
              // Left-handed, reverse-Z projection (standard for UE4/UE5 and modern DX11 games)
              dynamicProjMatrix.m[2][2] = 0.0f; // Far plane at infinity in reverse-Z
              dynamicProjMatrix.m[2][3] = 1.0f; // Left-handed w' = z
              dynamicProjMatrix.m[3][2] = 0.1f; // Near plane = 0.1m
              dynamicProjMatrix.m[3][3] = 0.0f;
          }

          bestCandidate.projection = dynamicProjMatrix;
          bestCandidate.valid = true;
          bestCandidate.temporalScore = 1.0f;
          bestCandidate.confidence = 100.0f;

          // Detect projection properties (reverseZ, handedness)
          bool rowMajor = true;
          bool reverseZ = true; // Default to true (UE4/UE5 reverse-Z standard)
          const auto* configMgr = SubsystemContext::Get().GetConfig();
          if (configMgr) {
              const auto& cfg = configMgr->GetConfig();
              reverseZ = cfg.reverseZ;
              rowMajor = cfg.rowMajorMatrices;
          } else if (CameraClassifier::ClassifyMatrix(dynamicProjMatrix, rowMajor, reverseZ)) {
              // Successfully classified from matrix
          }

          bestCandidate.rowMajor = rowMajor;
          bestCandidate.reversedZ = reverseZ;
          bestCandidate.isLeftHanded = (dynamicProjMatrix.m[2][3] > 0.0f);

          Matrix4x4 vp = {};
          for (int r = 0; r < 4; ++r) {
              for (int c = 0; c < 4; ++c) {
                  vp.m[r][c] = dynamicViewMatrix.m[r][0] * dynamicProjMatrix.m[0][c] +
                               dynamicViewMatrix.m[r][1] * dynamicProjMatrix.m[1][c] +
                               dynamicViewMatrix.m[r][2] * dynamicProjMatrix.m[2][c] +
                               dynamicViewMatrix.m[r][3] * dynamicProjMatrix.m[3][c];
              }
          }
          bestCandidate.viewProjection = vp;
          hasBest = true;
      } else {
          // Fall back to static hardcoded hooks/heuristics
          static uint64_t frameCounter = 0;
          frameCounter++;
          hasBest = CameraRankingEngine::RankCandidates(candidates, frameCounter, bestCandidate);
      }

      // Hand the lock manager this frame's colour resource before Update(), the same way
      // DepthLockManager::OnFrameEnd() is handed its resource context below. A
      // CameraCandidate carries only matrices, so without this the snapshot's
      // resourceIdentity stays null and CameraSnapshot::IsValid() can never be true.
      SubsystemContext::Get().GetCameraLockManager()->SetResourceContext(m_currentSnapshot.BackBufferIdentity());

      if (hasBest) {
        SubsystemContext::Get().GetCameraLockManager()->Update(bestCandidate, m_globalFrameCounter);
      } else {
        CameraCandidate empty = {};
        SubsystemContext::Get().GetCameraLockManager()->Update(empty, m_globalFrameCounter);
      }

      camSnapshot = SubsystemContext::Get().GetCameraLockManager()->GetSnapshot();
      m_stateMonitor.UpdateCameraHealth(camSnapshot.IsValid());

      // Notify PageScanner of camera lock state transitions to control adaptive sleep.
      // This drives the scanner to 0 MB/s memory bandwidth when tracking is stable.
      {
        auto lockState = SubsystemContext::Get().GetCameraLockManager()->GetState();
        bool isLockedNow = (lockState == CameraLockState::LOCKED);
        if (isLockedNow && !m_prevCameraLocked) {
          SubsystemContext::Get().GetPageScanner()->NotifyCameraLocked();
        } else if (!isLockedNow && m_prevCameraLocked) {
          SubsystemContext::Get().GetPageScanner()->NotifyCameraLost();
        }
        m_prevCameraLocked = isLockedNow;
      }
    }

    // ==============================================
    // DEPTH DISCOVERY SUBSYSTEM
    // ==============================================
    {
      ScopedCpuTimer depthTimer(&m_cpuProfiler, CpuSegment::DepthDiscovery);

      SubsystemContext::Get().GetDepthCandidateCollector()->OnFrameEnd(m_globalFrameCounter);
      SubsystemContext::Get().GetDepthLockManager()->OnFrameEnd(
          m_globalFrameCounter,
          static_cast<uint32_t>(m_currentSnapshot.epoch.deviceGeneration),
          static_cast<uint32_t>(m_currentSnapshot.epoch.swapchainGeneration),
          m_currentSnapshot.width, m_currentSnapshot.height);

      depthSnapshot = SubsystemContext::Get().GetDepthLockManager()->GetSnapshot();
      m_stateMonitor.UpdateDepthHealth(depthSnapshot.IsValid());
    }
  } catch (const std::system_error& e) {
    LOG_WARN("FrameCoordinator: Camera/Depth discovery system exception: %s (code: %d) (continuing to OpenXR)", e.what(), e.code().value());
  } catch (const std::exception& e) {
    LOG_WARN("FrameCoordinator: Camera/Depth discovery exception: %s (continuing to OpenXR)", e.what());
  } catch (...) {
    LOG_WARN("FrameCoordinator: Camera/Depth discovery unknown exception. Check VEH observer logs for SEH code. (continuing to OpenXR)");
  }

  // ==============================================
  // STEREO RENDERING PIPELINE
  // ==============================================
  LOG_DEBUG("FrameCoordinator: Entering stereo pipeline (frame %llu).", m_globalFrameCounter);

  if (!m_graphicsBackend) {
    if (m_currentSnapshot.backend == GraphicsBackend::DX11) {
      m_graphicsBackend = std::make_unique<DX11GraphicsBackend>();
      LOG_INFO("FrameCoordinator: Created DX11 graphics backend.");
    } else if (m_currentSnapshot.backend == GraphicsBackend::DX12) {
      m_graphicsBackend = std::make_unique<DX12GraphicsBackend>();
      LOG_INFO("FrameCoordinator: Created DX12 graphics backend.");
    } else if (m_currentSnapshot.backend == GraphicsBackend::Vulkan) {
      m_graphicsBackend = std::make_unique<vulkan::VulkanGraphicsBackend>();
      LOG_INFO("FrameCoordinator: Created Vulkan graphics backend.");
    } else {
      LOG_ERROR("FrameCoordinator: Unsupported graphics backend.");
      return;
    }
  }

  if (m_graphicsBackend && !m_graphicsBackend->Healthy() && m_currentSnapshot.nativeDevice) {
    if (m_graphicsBackend->Initialize(m_currentSnapshot.nativeDevice,
                                      m_currentSnapshot.nativeContext)) {
      LOG_INFO("FrameCoordinator: Graphics backend initialized successfully (device=%p, context=%p).",
               m_currentSnapshot.nativeDevice, m_currentSnapshot.nativeContext);
      if (m_currentSnapshot.backend == GraphicsBackend::DX11) {
        m_gpuProfiler.Initialize(
            static_cast<ID3D11Device *>(m_currentSnapshot.nativeDevice));
      }
    }
  }

  if (!m_aiScheduler) {
    std::unique_ptr<ai::IAIBackend> aiBackend;
    if (m_currentSnapshot.backend == GraphicsBackend::DX11) {
      aiBackend = std::make_unique<ai::DX11AIBackend>();
    } else if (m_currentSnapshot.backend == GraphicsBackend::DX12) {
      aiBackend = std::make_unique<ai::DX12AIBackend>();
    } else if (m_currentSnapshot.backend == GraphicsBackend::Vulkan) {
      aiBackend = std::make_unique<ai::VulkanAIBackend>();
    }
    
    if (aiBackend) {
        m_aiScheduler = std::make_unique<ai::AIScheduler>(std::move(aiBackend));
        LOG_INFO("FrameCoordinator: AI Scheduler initialized.");
    } else {
        LOG_WARN("FrameCoordinator: Could not initialize AI Scheduler (Unsupported backend)");
    }
  }

  if (!m_oxrHealthMonitor) {
    m_oxrHealthMonitor = std::make_unique<openxr::OpenXRHealthMonitor>();
    LOG_INFO("FrameCoordinator: OpenXR health monitor created.");
  }

  // Consume any newly initialized OpenXR runtime safely on the render thread
  {
    std::lock_guard<std::mutex> lock(m_oxrRuntimeMutex);
    if (m_pendingOxrRuntime) {
      m_oxrRuntime = std::move(m_pendingOxrRuntime);
    }
  }

  static std::atomic<bool> s_oxrInitInProgress{false};
  if (!m_oxrRuntime || m_oxrRuntime->GetState() == openxr::RuntimeState::FAILED) {
    if (!s_oxrInitInProgress.load()) {
      static auto s_lastAttempt = std::chrono::steady_clock::now() - std::chrono::seconds(10);
      auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration_cast<std::chrono::seconds>(now - s_lastAttempt).count() >= 2) {
        s_lastAttempt = now;
        s_oxrInitInProgress.store(true);
        GraphicsBackend backend = m_currentSnapshot.backend;
        std::thread([this, backend]() {
          LOG_INFO("FrameCoordinator: Async OpenXR initialization background thread started...");
          auto runtime = std::make_unique<openxr::OpenXRRuntimeManager>(m_oxrHealthMonitor.get());
          if (runtime->Initialize("NexVR Engine", backend)) {
            LOG_INFO("FrameCoordinator: Async OpenXR initialization SUCCEEDED (state=%d)!", (int)runtime->GetState());
            std::lock_guard<std::mutex> lock(m_oxrRuntimeMutex);
            m_pendingOxrRuntime = std::move(runtime);
          } else {
            LOG_ERROR("FrameCoordinator: Async OpenXR initialization FAILED. Will retry in background in 2s.");
          }
          s_oxrInitInProgress.store(false);
        }).detach();
      }
    }
  }

  if (m_oxrRuntime) {
    m_oxrRuntime->PollEvents();

    if (m_oxrRuntime->GetState() == openxr::RuntimeState::SYSTEM_SELECTED) {
      LOG_INFO("FrameCoordinator: OpenXR state is SYSTEM_SELECTED, creating session (backend=%d)...", (int)m_currentSnapshot.backend);
      if (!m_graphicsBackend->CreateOpenXRSession(m_oxrRuntime.get(), m_currentSnapshot)) {
          LOG_ERROR("FrameCoordinator: OpenXR session creation FAILED!");
      } else {
          LOG_INFO("FrameCoordinator: OpenXR session created successfully. Initializing Input Manager...");
          m_inputManager.Initialize(m_oxrRuntime->GetInstance(), m_oxrRuntime->GetSession());
      }
    }
  }

  // Only attempt stereo rendering if both camera and depth are valid, and
  // OpenXR is ready (strictly checking m_oxrRuntime != nullptr)
  bool xrReady =
      m_oxrRuntime &&
      m_oxrRuntime->GetState() >= openxr::RuntimeState::SESSION_READY &&
      m_oxrRuntime->GetState() != openxr::RuntimeState::STOPPING &&
      m_oxrRuntime->GetState() != openxr::RuntimeState::STOPPED &&
      m_oxrRuntime->GetState() != openxr::RuntimeState::FAILED;

  m_lastCompatibility = CompatibilityScorer::Evaluate(
      camSnapshot, depthSnapshot, m_currentSnapshot.backend,
      SubsystemContext::Get().GetEngineDetector()->GetDetection());

  if (!m_lastCompatibility.shouldAttemptStereo) {
    CompatibilityScorer::PostDiagnostic(m_lastCompatibility);
  }

  if (xrReady) {
    // Phase 9: Update virtual gamepad input from VR controllers
    m_inputManager.Update(m_oxrRuntime->GetSession());

    if (m_graphicsBackend && (m_graphicsBackend->Healthy() || m_graphicsBackend->GetState() == StereoRendererState::DEGRADED)) {

      // ==============================================
      // OPENXR FRAME SUBMISSION & RENDER
      // ==============================================
      if (m_oxrSwapchain && m_currentSnapshot.width > 0 && m_currentSnapshot.height > 0 &&
          (m_oxrSwapchain->GetWidth() != m_currentSnapshot.width || m_oxrSwapchain->GetHeight() != m_currentSnapshot.height)) {
          LOG_INFO("FrameCoordinator: Viewport resized from %ux%u to %ux%u. Recreating OpenXR swapchain.",
                   m_oxrSwapchain->GetWidth(), m_oxrSwapchain->GetHeight(), m_currentSnapshot.width, m_currentSnapshot.height);
          m_oxrSwapchain.reset();
      }

      if (!m_oxrSwapchain) {
        int64_t targetFormat = openxr::OpenXRSwapchainManager::SelectOptimalFormat(
            m_oxrRuntime->GetSession(), m_currentSnapshot.backend, m_currentSnapshot.format);
        LOG_INFO("[OK] Stereo Pipeline: Dual-Eye Swapchain Created (%ux%u, Format: %lld)",
                 m_currentSnapshot.width, m_currentSnapshot.height, targetFormat);
        m_oxrSwapchain = std::make_unique<openxr::OpenXRSwapchainManager>(
            m_oxrHealthMonitor.get());
        if (!m_oxrSwapchain->Initialize(
            m_oxrRuntime->GetSession(), targetFormat, m_currentSnapshot.width,
            m_currentSnapshot.height, m_currentSnapshot.backend)) {
            LOG_ERROR("FrameCoordinator: OpenXR swapchain creation FAILED with format %lld!", targetFormat);
            m_oxrSwapchain.reset();
        }
      }
      if (!m_oxrSubmitter) {
        m_oxrSubmitter = std::make_unique<openxr::OpenXRFrameSubmitter>(
            m_oxrHealthMonitor.get());
      }

      StereoParams params;
      params.convergence = 0.5f;
      const auto* cfgMgr = SubsystemContext::Get().GetConfig();
      if (cfgMgr) {
          const auto& c = cfgMgr->GetConfig();
          if (c.ipd > 0.0f) params.ipd = c.ipd;
          if (c.convergence > 0.0f) params.convergence = c.convergence;
      }

      if (m_aiScheduler) {
          ai::AIJob aiJob;
          aiJob.frameId = m_globalFrameCounter;
          aiJob.colorTarget = camSnapshot.resourceIdentity.nativeHandle;
          aiJob.depthTarget = depthSnapshot.identity.nativeHandle;
          aiJob.width = m_currentSnapshot.width;
          aiJob.height = m_currentSnapshot.height;
          m_aiScheduler->PushJob(aiJob);
      }

      m_graphicsBackend->SubmitStereoFrame(
          m_oxrRuntime.get(),
          m_oxrSwapchain.get(),
          m_oxrSubmitter.get(),
          m_currentSnapshot,
          camSnapshot,
          depthSnapshot,
          params,
          m_stateMonitor,
          m_cpuProfiler,
          m_gpuProfiler,
          m_lastCompatibility.shouldAttemptStereo,
          m_aiScheduler ? m_aiScheduler->GetLatestUIMask() : nullptr
      );

      m_cachedHeadsetPose = m_currentSnapshot.leftPose;

      m_stateMonitor.UpdateOpenXrHealth(true);
    } else {
      m_stateMonitor.UpdateStereoHealth(false);
      m_stateMonitor.UpdateOpenXrHealth(false);
    }
  } else {
    if (m_graphicsBackend && m_graphicsBackend->Healthy()) {
      m_graphicsBackend->SetState(StereoRendererState::DEGRADED);
    }
    m_stateMonitor.UpdateStereoHealth(false);
    m_stateMonitor.UpdateOpenXrHealth(false);
  }
}

void FrameCoordinator::OnPresentEnd() {
  if (!m_frameActive)
    return;

  if (m_graphicsBackend && m_currentSnapshot.backend == GraphicsBackend::DX11) {
    m_gpuProfiler.EndFrame(
        static_cast<ID3D11DeviceContext *>(m_currentSnapshot.nativeContext));
  }

  auto report = m_stateMonitor.EvaluateHealth();
  m_dashboard.Update(report, m_cpuProfiler, m_gpuProfiler);

  m_dashboard.PrintToConsole();

  m_frameActive = false;
}

} // namespace vrinject
