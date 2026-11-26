#pragma once

#include "openxr/openxr_types.h"
#include "openxr/openxr_health_monitor.h"
#include "openxr/openxr_swapchain_manager.h"
#include <d3d11.h>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace vrinject {
namespace openxr {

class OpenXRFrameSubmitter {
public:
    OpenXRFrameSubmitter(OpenXRHealthMonitor* healthMonitor);
    ~OpenXRFrameSubmitter();

    // DX11 Phased Submission
    bool BeginAndAcquireDX11(
        XrSession session,
        XrSpace referenceSpace,
        OpenXRSwapchainManager* swapchainManager,
        XrPosef& outLeftPose, XrFovf& outLeftFov,
        XrPosef& outRightPose, XrFovf& outRightFov
    );

    bool ReleaseAndEndDX11(
        XrSession session,
        XrSpace referenceSpace,
        OpenXRSwapchainManager* swapchainManager,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* leftEyeTex,
        ID3D11Texture2D* rightEyeTex
    );

    // DX12 Phased Submission
    bool BeginAndAcquireDX12(
        XrSession session,
        XrSpace referenceSpace,
        OpenXRSwapchainManager* swapchainManager,
        ID3D12Resource*& outLeftDest,
        ID3D12Resource*& outRightDest,
        XrPosef& outLeftPose, XrFovf& outLeftFov,
        XrPosef& outRightPose, XrFovf& outRightFov
    );

    bool ReleaseAndEndDX12(
        XrSession session,
        XrSpace referenceSpace,
        OpenXRSwapchainManager* swapchainManager,
        uint32_t leftWidth, uint32_t leftHeight,
        uint32_t rightWidth, uint32_t rightHeight
    );

    // Vulkan Phased Submission
    bool BeginAndAcquireVulkan(
        XrSession session,
        XrSpace referenceSpace,
        OpenXRSwapchainManager* swapchainManager,
        VkImage& outLeftDest,
        VkImage& outRightDest,
        XrPosef& outLeftPose, XrFovf& outLeftFov,
        XrPosef& outRightPose, XrFovf& outRightFov
    );

    bool ReleaseAndEndVulkan(
        XrSession session,
        XrSpace referenceSpace,
        OpenXRSwapchainManager* swapchainManager,
        uint32_t leftWidth, uint32_t leftHeight,
        uint32_t rightWidth, uint32_t rightHeight
    );

    SubmitterState GetState() const { return state_; }
    void ResetState() { state_ = SubmitterState::WAIT_FRAME; }

    // Non-blocking shutdown signal so FrameCoordinator can destroy the OpenXR session
    // (unblocking any active xrWaitFrame) before joining the pacing thread (HIGH-09).
    void BeginShutdown();

    struct HudQuadConfig {
        bool enabled = false;
        XrSwapchain swapchain = XR_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        XrPosef pose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -0.95f}}; // 0.95m directly in front
        XrExtent2Df size = {1.2f, 0.675f};                                 // 1.2m width, ~16:9 aspect
    };

    struct CinemaQuadConfig {
        bool enabled = false;
        float distance = 2.5f;          // Screen distance in meters
        float sizeX = 3.2f;             // Screen width in meters
        float sizeY = 1.8f;             // Screen height in meters
        XrPosef pose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -2.5f}};
        bool needsRecenter = true;
    };

    void SetHudQuad(XrSwapchain swapchain, uint32_t width, uint32_t height, float distance = 0.95f, float sizeX = 1.2f, float sizeY = 0.675f);
    void SetHudQuadAnchored(XrSwapchain swapchain, uint32_t width, uint32_t height, const XrPosef& headPose, bool recenter, float distance = 1.45f, float sizeX = 1.35f, float sizeY = 0.76f);
    void ClearHudQuad();
    const HudQuadConfig& GetHudQuadConfig() const { return hudQuadConfig_; }

    void SetCinemaQuad(bool enabled, float distance = 2.5f, float width = 1920.0f, float height = 1080.0f);
    void RequestCinemaRecenter();
    void RecenterCinemaQuad(const XrPosef& headPose);
    const CinemaQuadConfig& GetCinemaQuadConfig() const { return cinemaQuadConfig_; }

private:
    bool LocateViews(XrSession session, XrSpace referenceSpace, 
                     XrPosef& outLeftPose, XrFovf& outLeftFov,
                     XrPosef& outRightPose, XrFovf& outRightFov);

    // Asynchronous Frame Pacing Engine
    void EnsurePacingThreadRunning(XrSession session);
    void StopPacingThread();
    void PacingThreadLoop();
    bool WaitAndBeginFrame(XrSession session);
    bool ConsumePacedFrameState(XrSession session, XrFrameState& outFrameState, bool& outPacingBusyOrFailed);
    void TriggerNextPacingWait(XrSession session);

    OpenXRHealthMonitor* healthMonitor_ = nullptr;
    SubmitterState state_ = SubmitterState::WAIT_FRAME;
    HudQuadConfig hudQuadConfig_;
    CinemaQuadConfig cinemaQuadConfig_;

    XrFrameState currentFrameState_{XR_TYPE_FRAME_STATE};
    
    // Cached fallback poses in case of tracking loss
    XrPosef lastLeftPose_ = {{0, 0, 0, 1}, {0, 0, 0}};
    XrPosef lastRightPose_ = {{0, 0, 0, 1}, {0, 0, 0}};
    XrFovf lastLeftFov_ = {-0.8f, 0.8f, 0.8f, -0.8f};
    XrFovf lastRightFov_ = {-0.8f, 0.8f, 0.8f, -0.8f};

    uint32_t lastAcquiredLeftIndex_ = 0;
    uint32_t lastAcquiredRightIndex_ = 0;

    // Pacing thread synchronization state
    std::thread pacingThread_;
    std::mutex pacingMutex_;
    std::condition_variable cvPacingRequest_;
    std::condition_variable cvPacingReady_;
    XrSession pacingSession_ = XR_NULL_HANDLE;
    XrFrameState pacingFrameState_{XR_TYPE_FRAME_STATE};
    std::atomic<bool> pacingRunning_{false};
    bool pacingWaitRequested_ = false;
    bool pacingInFlight_ = false;
    bool pacingFrameReady_ = false;
    bool pacingFailed_ = false;
};

} // namespace openxr
} // namespace vrinject
