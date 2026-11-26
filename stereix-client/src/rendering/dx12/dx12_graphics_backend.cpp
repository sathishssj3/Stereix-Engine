#include "rendering/dx12/dx12_graphics_backend.h"
#include "heuristics/camera_lock_manager.h"
#include "heuristics/depth_lock_manager.h"
#include "core/performance_profiler.h"
#include "core/subsystem_context.h"
#include "core/gpu_profiler.h"
#include "core/runtime_state_monitor.h"
#include "rendering/stereo/stereo_pipeline.h"
#include "openxr/openxr_runtime_manager.h"
#include "openxr/openxr_swapchain_manager.h"
#include "openxr/openxr_frame_submitter.h"
#include "core/logger.h"
#include "rendering/dx12/dx12_lifecycle_manager.h"
#include "rendering/stereo/stereo_camera_generator.h"
#include "rendering/stereo/stereo_pipeline.h"
#include "rendering/dx12/imgui_dx12_integration.h"
#include "core/overlay_manager.h"
#include <iostream>

namespace vrinject {

DX12GraphicsBackend::DX12GraphicsBackend() {
    m_resourceManager = std::make_unique<DX12StereoResourceManager>();
    m_psoCache = std::make_unique<DX12PipelineStateCache>();
    m_stateTracker = std::make_unique<DX12ResourceStateTracker>();
    m_fenceManager = std::make_unique<DX12FenceManager>();
    m_renderer = std::make_unique<DX12StereoRenderer>();
}

DX12GraphicsBackend::~DX12GraphicsBackend() {
    Shutdown();
}

bool DX12GraphicsBackend::Initialize(void* nativeDevice, void* nativeContext) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!nativeDevice) {
        m_state = StereoRendererState::UNINITIALIZED;
        return false;
    }

    m_device = reinterpret_cast<ID3D12Device*>(nativeDevice);
    
    // Acquire command queue from nativeContext or lifecycle manager
    if (nativeContext) {
        m_commandQueue = reinterpret_cast<ID3D12CommandQueue*>(nativeContext);
    } else {
        m_commandQueue = reinterpret_cast<ID3D12CommandQueue*>(Dx12LifecycleManager::Get().GetMainQueue());
    }
    if (!m_commandQueue) {
        LOG_WARN("DX12GraphicsBackend: No CommandQueue available yet. Postponing initialization.");
        m_state = StereoRendererState::UNINITIALIZED;
        return false;
    }

    if (!m_fenceManager->Initialize(m_device.Get())) {
        LOG_ERROR("DX12GraphicsBackend: FenceManager initialization failed.");
        m_state = StereoRendererState::FAILED;
        return false;
    }

    if (!m_psoCache->Initialize(m_device.Get())) {
        LOG_ERROR("DX12GraphicsBackend: PSOCache initialization failed.");
        m_state = StereoRendererState::FAILED;
        return false;
    }

    if (!m_resourceManager->Initialize(m_device.Get(), m_fenceManager.get(), m_psoCache.get())) {
        LOG_ERROR("DX12GraphicsBackend: ResourceManager initialization failed.");
        m_state = StereoRendererState::FAILED;
        return false;
    }

    if (!m_renderer->Initialize(m_device.Get())) {
        LOG_ERROR("DX12GraphicsBackend: StereoRenderer initialization failed.");
        m_state = StereoRendererState::FAILED;
        return false;
    }

    m_state = StereoRendererState::READY;
    LOG_INFO("DX12GraphicsBackend: Initialization completed successfully (device=%p, queue=%p).",
             m_device.Get(), m_commandQueue.Get());
    return true;
}

void DX12GraphicsBackend::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_renderer) m_renderer->Shutdown();
    if (m_resourceManager) m_resourceManager->Shutdown();
    if (m_psoCache) m_psoCache->Shutdown();
    if (m_fenceManager) m_fenceManager->Shutdown();
    if (m_stateTracker) m_stateTracker->Clear();

    m_oxrLeftDest = nullptr;
    m_oxrRightDest = nullptr;
    m_oxrHudDest = nullptr;
    m_commandQueue.Reset();
    m_device.Reset();
    m_state = StereoRendererState::UNINITIALIZED;
}

void DX12GraphicsBackend::ReleaseResources() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fenceManager && m_commandQueue) {
        uint64_t val = m_fenceManager->Signal(m_commandQueue.Get());
        m_fenceManager->WaitForFenceValue(val);
        m_fenceManager->ProcessDeferredReleases();
    }
    if (m_stateTracker) {
        m_stateTracker->Clear();
    }
    m_oxrLeftDest = nullptr;
    m_oxrRightDest = nullptr;
    m_oxrHudDest = nullptr;
}

void DX12GraphicsBackend::SetState(StereoRendererState state) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state = state;
}

StereoRendererState DX12GraphicsBackend::GetState() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

bool DX12GraphicsBackend::Healthy() const {
    StereoRendererState s = GetState();
    return (s == StereoRendererState::READY || s == StereoRendererState::DEGRADED) && m_device != nullptr;
}

CameraSnapshot DX12GraphicsBackend::GetCamera() {
    // Rely on core logic, which has DX12 hooked versions updating it
    return SubsystemContext::Get().GetCameraLockManager()->GetSnapshot();
}

DepthSnapshot DX12GraphicsBackend::GetDepth() {
    return SubsystemContext::Get().GetDepthLockManager()->GetSnapshot();
}

void DX12GraphicsBackend::RenderStereo(
    const RenderFrameSnapshot& frameSnapshot,
    const CameraSnapshot& camSnapshot, 
    const DepthSnapshot& depthSnapshot, 
    const StereoParams& params,
    bool shouldAttemptStereo,
    void* uiMaskHandle) 
{
    if (m_state == StereoRendererState::UNINITIALIZED || m_state == StereoRendererState::FAILED) return;

    // Dynamically synchronize active main queue from Dx12LifecycleManager across swapchains
    ID3D12CommandQueue* activeQueue = Dx12LifecycleManager::Get().GetMainQueue();
    if (activeQueue) {
        m_commandQueue = activeQueue;
    }
    if (!m_commandQueue || !m_device) return;

    StereoConstants constants;
    constants.ipd = params.ipd;
    constants.nearPlane = params.nearPlane;
    constants.farPlane = params.farPlane;
    constants.convergence = params.convergence;
    constants.enableASW = params.enableASW;
    constants.aswTargetFps = params.aswTargetFps;

    EyeView leftEye, rightEye;
    StereoCameraGenerator::Generate(frameSnapshot, camSnapshot, constants, leftEye, rightEye);

    std::lock_guard<std::mutex> lock(m_mutex);

    // Free resources that completed execution on GPU
    m_fenceManager->ProcessDeferredReleases();

    // Wait for the next ring-buffer slot's GPU fence BEFORE updating CBV/SRV descriptors (HIGH-08)
    const uint32_t frameSlot = m_renderer->PrepareNextFrameSlot(m_fenceManager.get());

    // Update resources based on new snapshots (like handling resizes) and write stereo matrices
    m_resourceManager->UpdateFrameResources(m_device.Get(), leftEye, rightEye, camSnapshot, depthSnapshot, shouldAttemptStereo, frameSlot);

    // Perform rendering dispatch and synchronize
    bool success = m_renderer->RenderStereo(
        m_commandQueue.Get(), 
        m_resourceManager.get(), 
        m_psoCache.get(), 
        m_stateTracker.get(), 
        m_fenceManager.get(), 
        camSnapshot, 
        depthSnapshot,
        m_oxrLeftDest,
        m_oxrRightDest,
        shouldAttemptStereo,
        uiMaskHandle ? (ID3D12Resource*)uiMaskHandle : nullptr,
        m_oxrHudDest);
        
    if (success) {
        m_state = StereoRendererState::READY;
    } else {
        m_state = StereoRendererState::DEGRADED;
    }
}

void* DX12GraphicsBackend::GetLeftEyeTexture() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_resourceManager ? m_resourceManager->GetLeftEyeTexture() : nullptr;
}

void* DX12GraphicsBackend::GetRightEyeTexture() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_resourceManager ? m_resourceManager->GetRightEyeTexture() : nullptr;
}

void DX12GraphicsBackend::SetOpenXRSwapchainImages(ID3D12Resource* left, ID3D12Resource* right) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_oxrLeftDest = left;
    m_oxrRightDest = right;
}

bool DX12GraphicsBackend::CreateOpenXRSession(openxr::OpenXRRuntimeManager* xrRuntime, const RenderFrameSnapshot& snapshot) {
    if (!xrRuntime) return false;

    // OpenXR REQUIRES xrGetD3D12GraphicsRequirementsKHR before xrCreateSession
    LUID adapterLuid{};
    if (!xrRuntime->CheckDX12GraphicsRequirements(&adapterLuid)) {
        LOG_ERROR("DX12GraphicsBackend: CheckDX12GraphicsRequirements failed!");
    } else {
        LOG_INFO("DX12GraphicsBackend: DX12 graphics requirements satisfied (LUID: %08x:%08x).",
                 adapterLuid.HighPart, adapterLuid.LowPart);
    }
    bool sessionOk = xrRuntime->CreateSessionDX12(
        static_cast<ID3D12Device *>(snapshot.nativeDevice),
        static_cast<ID3D12CommandQueue *>(snapshot.nativeContext));
    if (sessionOk) {
        LOG_INFO("DX12GraphicsBackend: DX12 OpenXR session created successfully!");
    } else {
        LOG_ERROR("DX12GraphicsBackend: DX12 OpenXR session creation FAILED!");
    }
    return sessionOk;
}

void DX12GraphicsBackend::SubmitStereoFrame(
    openxr::OpenXRRuntimeManager* xrRuntime,
    openxr::OpenXRSwapchainManager* oxrSwapchain,
    openxr::OpenXRFrameSubmitter* oxrSubmitter,
    RenderFrameSnapshot& currentSnapshot,
    const CameraSnapshot& camSnapshot,
    const DepthSnapshot& depthSnapshot,
    const StereoParams& params,
    RuntimeStateMonitor& stateMonitor,
    PerformanceProfiler& cpuProfiler,
    GpuProfiler& gpuProfiler,
    bool shouldAttemptStereo,
    void* uiMaskHandle
) {
    ID3D12Resource *leftDest = nullptr;
    ID3D12Resource *rightDest = nullptr;
    XrPosef leftPose, rightPose;
    XrFovf leftFov, rightFov;

    {
        ScopedCpuTimer oxrTimer(&cpuProfiler, CpuSegment::OpenXrSubmission);
        if (oxrSubmitter->BeginAndAcquireDX12(xrRuntime->GetSession(),
                                              xrRuntime->GetReferenceSpace(),
                                              oxrSwapchain,
                                              leftDest, rightDest,
                                              leftPose, leftFov, rightPose, rightFov)) {
            currentSnapshot.leftPose = leftPose;
            currentSnapshot.leftFov = leftFov;
            currentSnapshot.rightPose = rightPose;
            currentSnapshot.rightFov = rightFov;

            if (oxrSwapchain && m_resourceManager) {
                if (oxrSwapchain->GetFormat() > 0) {
                    m_resourceManager->SetTargetFormat(static_cast<DXGI_FORMAT>(oxrSwapchain->GetFormat()));
                }
                uint32_t oxrW = oxrSwapchain->GetWidth();
                uint32_t oxrH = oxrSwapchain->GetHeight();
                if (oxrW > 0 && oxrH > 0) {
                    m_resourceManager->SetTargetDimensions(oxrW, oxrH);
                }
            }

            SetOpenXRSwapchainImages(leftDest, rightDest);

            // Acquire floating HUD quad image BEFORE RenderStereo so DX12StereoRenderer copies uiMaskHandle into it (MED-11, HIGH-09)
            bool hudAcquired = false;
            bool hudCopied = false;
            {
                std::lock_guard<std::mutex> hudLock(m_mutex);
                m_oxrHudDest = nullptr;
            }
            if (uiMaskHandle && oxrSwapchain && oxrSwapchain->GetHudSwapchain() != XR_NULL_HANDLE) {
                uint32_t hudImageIndex = 0;
                if (oxrSwapchain->AcquireHudImage(hudImageIndex)) {
                    hudAcquired = true;
                    if (oxrSwapchain->WaitHudImage()) {
                        ID3D12Resource* hudDest = oxrSwapchain->GetHudSwapchainImageDX12(hudImageIndex);
                        if (hudDest) {
                            std::lock_guard<std::mutex> hudLock(m_mutex);
                            m_oxrHudDest = hudDest;
                            hudCopied = true;
                        }
                    }
                }
            }

            RenderStereo(currentSnapshot, camSnapshot, depthSnapshot, params, shouldAttemptStereo, uiMaskHandle);
            bool stereoOk = (m_state == StereoRendererState::READY);

            {
                std::lock_guard<std::mutex> hudLock(m_mutex);
                m_oxrHudDest = nullptr;
            }
            if (hudAcquired && oxrSwapchain) {
                oxrSwapchain->ReleaseHudImage();
            }
            if (stereoOk && hudCopied && oxrSwapchain) {
                const uint32_t hudW = oxrSwapchain->GetHudWidth() > 0 ? oxrSwapchain->GetHudWidth() : currentSnapshot.width;
                const uint32_t hudH = oxrSwapchain->GetHudHeight() > 0 ? oxrSwapchain->GetHudHeight() : currentSnapshot.height;
                oxrSubmitter->SetHudQuad(oxrSwapchain->GetHudSwapchain(), hudW, hudH);
            } else {
                oxrSubmitter->ClearHudQuad();
            }

            uint32_t submitW = (oxrSwapchain && oxrSwapchain->GetWidth() > 0) ? oxrSwapchain->GetWidth() : currentSnapshot.width;
            uint32_t submitH = (oxrSwapchain && oxrSwapchain->GetHeight() > 0) ? oxrSwapchain->GetHeight() : currentSnapshot.height;

            bool isCinema = false;
            float cinemaDist = 2.5f;
            const auto* cfgMgr = SubsystemContext::Get().GetConfig();
            if (cfgMgr) {
                const auto& cfg = cfgMgr->GetConfig();
                if (cfg.forceCinemaMode) isCinema = true;
                if (cfg.theaterDistance >= 1.0f) cinemaDist = cfg.theaterDistance;
            }
            oxrSubmitter->SetCinemaQuad(isCinema, cinemaDist, static_cast<float>(submitW), static_cast<float>(submitH));

            oxrSubmitter->ReleaseAndEndDX12(
                xrRuntime->GetSession(), xrRuntime->GetReferenceSpace(),
                oxrSwapchain, submitW, submitH, submitW, submitH);

            stateMonitor.UpdateStereoHealth(stereoOk || (currentSnapshot.backBuffer != nullptr));
            stateMonitor.UpdateOpenXrHealth(true);
        } else {
            stateMonitor.UpdateOpenXrHealth(false);
        }
    }
}

} // namespace vrinject
