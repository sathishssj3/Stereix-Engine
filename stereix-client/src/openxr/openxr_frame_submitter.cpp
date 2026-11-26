#include "openxr/openxr_frame_submitter.h"
#include "core/logger.h"
#include "core/seh_shield.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>

namespace vrinject {
namespace openxr {

OpenXRFrameSubmitter::OpenXRFrameSubmitter(OpenXRHealthMonitor* healthMonitor)
    : healthMonitor_(healthMonitor) {}

OpenXRFrameSubmitter::~OpenXRFrameSubmitter() {
    StopPacingThread();
}

void OpenXRFrameSubmitter::BeginShutdown() {
    {
        std::lock_guard<std::mutex> lock(pacingMutex_);
        pacingRunning_.store(false, std::memory_order_release);
        pacingWaitRequested_ = false;
        pacingSession_ = XR_NULL_HANDLE;
    }
    cvPacingRequest_.notify_all();
    cvPacingReady_.notify_all();
}

void OpenXRFrameSubmitter::EnsurePacingThreadRunning(XrSession session) {
    if (!session || session == XR_NULL_HANDLE) return;

    bool needRestart = false;
    {
        std::lock_guard<std::mutex> lock(pacingMutex_);
        needRestart = (pacingSession_ != session || !pacingRunning_.load(std::memory_order_acquire));
    }

    if (needRestart) {
        StopPacingThread();

        std::lock_guard<std::mutex> lock(pacingMutex_);
        pacingSession_ = session;
        pacingRunning_.store(true, std::memory_order_release);
        pacingWaitRequested_ = true; // Request immediate wait for first frame
        pacingInFlight_ = false;
        pacingFrameReady_ = false;
        pacingFailed_ = false;
        pacingThread_ = std::thread(&OpenXRFrameSubmitter::PacingThreadLoop, this);
    }
}

void OpenXRFrameSubmitter::StopPacingThread() {
    BeginShutdown();
    if (pacingThread_.joinable()) {
        pacingThread_.join();
    }
}

void OpenXRFrameSubmitter::PacingThreadLoop() {
    while (pacingRunning_.load(std::memory_order_acquire)) {
        XrSession session = XR_NULL_HANDLE;
        {
            std::unique_lock<std::mutex> lock(pacingMutex_);
            cvPacingRequest_.wait(lock, [this]() {
                return !pacingRunning_.load(std::memory_order_acquire) || pacingWaitRequested_;
            });
            if (!pacingRunning_.load(std::memory_order_acquire)) break;
            session = pacingSession_;
            pacingWaitRequested_ = false;
            pacingInFlight_ = (session != XR_NULL_HANDLE);
            pacingFailed_ = false;
        }

        if (session != XR_NULL_HANDLE) {
            XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
            XrFrameState frameState{XR_TYPE_FRAME_STATE};
            XrResult res = xrWaitFrame(session, &waitInfo, &frameState);

            {
                std::lock_guard<std::mutex> lock(pacingMutex_);
                pacingInFlight_ = false;
                if (XR_SUCCEEDED(res)) {
                    pacingFrameState_ = frameState;
                    pacingFrameReady_ = true;
                    pacingFailed_ = false;
                } else {
                    pacingFrameReady_ = false;
                    pacingFailed_ = true;
                }
            }
            cvPacingReady_.notify_all();
        }
    }
}

bool OpenXRFrameSubmitter::ConsumePacedFrameState(XrSession session, XrFrameState& outFrameState, bool& outPacingBusyOrFailed) {
    std::unique_lock<std::mutex> lock(pacingMutex_);
    outPacingBusyOrFailed = false;
    if (!pacingRunning_.load(std::memory_order_acquire) || pacingSession_ != session) return false;

    if (!pacingFrameReady_ && !pacingFailed_ && (pacingInFlight_ || pacingWaitRequested_)) {
        // Wait up to 16ms ONLY when a pacing wait is actually requested or in flight (avoid 16ms dead-stall after failed xrBeginFrame)
        cvPacingReady_.wait_for(lock, std::chrono::milliseconds(16), [this]() {
            return pacingFrameReady_ || pacingFailed_ || !pacingRunning_.load(std::memory_order_acquire);
        });
    }

    if (pacingFrameReady_) {
        outFrameState = pacingFrameState_;
        pacingFrameReady_ = false;
        pacingFailed_ = false;
        return true;
    }

    if (pacingInFlight_ || pacingWaitRequested_) {
        // Never invoke xrWaitFrame synchronously while pacing thread still owns an in-flight/pending wait (HIGH-09)
        outPacingBusyOrFailed = true;
        return false;
    }

    if (pacingFailed_) {
        // Previous xrWaitFrame failed; schedule a retry for next Present without double-calling synchronously
        pacingFailed_ = false;
        pacingWaitRequested_ = true;
        outPacingBusyOrFailed = true;
        lock.unlock();
        cvPacingRequest_.notify_one();
        return false;
    }

    return false;
}

void OpenXRFrameSubmitter::TriggerNextPacingWait(XrSession session) {
    {
        std::lock_guard<std::mutex> lock(pacingMutex_);
        if (!pacingRunning_.load(std::memory_order_acquire)) return;
        pacingSession_ = session;
        pacingWaitRequested_ = true;
    }
    cvPacingRequest_.notify_one();
}

bool OpenXRFrameSubmitter::WaitAndBeginFrame(XrSession session) {
    EnsurePacingThreadRunning(session);

    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    bool pacingBusyOrFailed = false;
    bool gotPacedState = ConsumePacedFrameState(session, frameState, pacingBusyOrFailed);
    if (!gotPacedState) {
        if (pacingBusyOrFailed) {
            if (healthMonitor_) healthMonitor_->RecordFrameDropped();
            return false;
        }
        // Synchronous fallback ONLY when pacing thread is idle/stopped (no concurrent xrWaitFrame possible)
        state_ = SubmitterState::WAIT_FRAME;
        XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        XrResult res = xrWaitFrame(session, &waitInfo, &frameState);
        if (XR_FAILED(res)) {
            if (healthMonitor_) healthMonitor_->RecordFrameDropped();
            return false;
        }
    }

    currentFrameState_ = frameState;

    state_ = SubmitterState::BEGIN_FRAME;
    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    XrResult res = xrBeginFrame(session, &beginInfo);
    if (XR_FAILED(res)) {
        if (healthMonitor_) healthMonitor_->RecordFrameDropped();
        return false;
    }

    // Immediately trigger background pacing for the NEXT frame now that xrBeginFrame has been called
    TriggerNextPacingWait(session);

    return true;
}

bool OpenXRFrameSubmitter::LocateViews(XrSession session, XrSpace referenceSpace, 
                                       XrPosef& outLeftPose, XrFovf& outLeftFov,
                                       XrPosef& outRightPose, XrFovf& outRightFov) {
    XrViewLocateInfo viewLocateInfo{XR_TYPE_VIEW_LOCATE_INFO};
    viewLocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    viewLocateInfo.displayTime = currentFrameState_.predictedDisplayTime;
    viewLocateInfo.space = referenceSpace;

    XrViewState viewState{XR_TYPE_VIEW_STATE};
    uint32_t viewCountOutput;
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    XrResult res = xrLocateViews(session, &viewLocateInfo, &viewState, 2, &viewCountOutput, views);

    if (XR_SUCCEEDED(res) && viewCountOutput == 2 &&
        (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        
        outLeftPose = views[0].pose;
        outLeftFov = views[0].fov;
        outRightPose = views[1].pose;
        outRightFov = views[1].fov;

        lastLeftPose_ = outLeftPose;
        lastLeftFov_ = outLeftFov;
        lastRightPose_ = outRightPose;
        lastRightFov_ = outRightFov;
        return true;
    } else {
        // Fallback to last known good pose
        outLeftPose = lastLeftPose_;
        outLeftFov = lastLeftFov_;
        outRightPose = lastRightPose_;
        outRightFov = lastRightFov_;
        return false;
    }
}

bool OpenXRFrameSubmitter::BeginAndAcquireDX11(
    XrSession session,
    XrSpace referenceSpace,
    OpenXRSwapchainManager* swapchainManager,
    XrPosef& outLeftPose, XrFovf& outLeftFov,
    XrPosef& outRightPose, XrFovf& outRightFov) 
{
    outLeftPose = lastLeftPose_;
    outLeftFov = lastLeftFov_;
    outRightPose = lastRightPose_;
    outRightFov = lastRightFov_;

    if (!WaitAndBeginFrame(session)) return false;

    auto endEmptyFrame = [&]() {
        state_ = SubmitterState::END_FRAME;
        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = currentFrameState_.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = 0;
        xrEndFrame(session, &endInfo);
    };

    if (!currentFrameState_.shouldRender || !swapchainManager) {
        // App is not in focus/visible (or swapchain unavailable): pair xrEndFrame cleanly and skip render (HIGH-09)
        endEmptyFrame();
        return false;
    }

    // Locate views for head tracking
    LocateViews(session, referenceSpace, outLeftPose, outLeftFov, outRightPose, outRightFov);

    // 3. Acquire Images
    state_ = SubmitterState::ACQUIRE_IMAGES;
    uint32_t leftIndex = 0;
    uint32_t rightIndex = 0;
    if (!swapchainManager->AcquireImages(leftIndex, rightIndex)) {
        endEmptyFrame();
        return false;
    }
    lastAcquiredLeftIndex_ = leftIndex;
    lastAcquiredRightIndex_ = rightIndex;

    // 4. Wait Images
    state_ = SubmitterState::WAIT_IMAGES;
    if (!swapchainManager->WaitImages()) {
        swapchainManager->ReleaseImages();
        endEmptyFrame();
        return false;
    }

    return true;
}

static bool AreD3D11FormatsCompatible(DXGI_FORMAT f1, DXGI_FORMAT f2) {
    if (f1 == f2) return true;
    auto getFamily = [](DXGI_FORMAT f) -> int {
        switch (f) {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_R8G8B8A8_UINT:
            case DXGI_FORMAT_R8G8B8A8_SNORM:
            case DXGI_FORMAT_R8G8B8A8_SINT:
                return 1;
            case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                return 2;
            case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            case DXGI_FORMAT_R10G10B10A2_UNORM:
            case DXGI_FORMAT_R10G10B10A2_UINT:
                return 3;
            case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R16G16B16A16_UNORM:
                return 4;
            default:
                return 0;
        }
    };
    int fam1 = getFamily(f1);
    int fam2 = getFamily(f2);
    return (fam1 != 0 && fam1 == fam2);
}

bool OpenXRFrameSubmitter::ReleaseAndEndDX11(
    XrSession session,
    XrSpace referenceSpace,
    OpenXRSwapchainManager* swapchainManager,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* leftEyeTex,
    ID3D11Texture2D* rightEyeTex) 
{
    auto startTime = std::chrono::high_resolution_clock::now();

    if (!currentFrameState_.shouldRender) {
        state_ = SubmitterState::END_FRAME;
        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = currentFrameState_.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = 0;
        xrEndFrame(session, &endInfo);
        return true;
    }

    // 5. Copy Textures
    state_ = SubmitterState::COPY;
    ID3D11Texture2D* leftDest = swapchainManager->GetLeftSwapchainImage(lastAcquiredLeftIndex_);
    ID3D11Texture2D* rightDest = swapchainManager->GetRightSwapchainImage(lastAcquiredRightIndex_);

    auto copyEye = [&](ID3D11Texture2D* src, ID3D11Texture2D* dst, const char* eyeName) {
        if (!src || !dst) return;
        if (!vrinject::seh::IsValidMemoryPointer(src) || !vrinject::seh::IsValidMemoryPointer(dst)) return;

        D3D11_TEXTURE2D_DESC srcDesc = {};
        D3D11_TEXTURE2D_DESC dstDesc = {};
        __try {
            src->GetDesc(&srcDesc);
            dst->GetDesc(&dstDesc);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG_WARN("OpenXRFrameSubmitter: SEH intercepted GetDesc on %s", eyeName);
            return;
        }

        bool compatible = (srcDesc.Format == dstDesc.Format) || AreD3D11FormatsCompatible(srcDesc.Format, dstDesc.Format);
        __try {
            if (compatible && srcDesc.Width == dstDesc.Width && srcDesc.Height == dstDesc.Height) {
                if (srcDesc.Format == dstDesc.Format) {
                    context->CopyResource(dst, src);
                } else {
                    D3D11_BOX box;
                    box.left = 0;
                    box.top = 0;
                    box.front = 0;
                    box.right = srcDesc.Width;
                    box.bottom = srcDesc.Height;
                    box.back = 1;
                    context->CopySubresourceRegion(dst, 0, 0, 0, 0, src, 0, &box);
                }
            } else if (compatible) {
                D3D11_BOX box;
                box.left = 0;
                box.top = 0;
                box.front = 0;
                box.right = (std::min)(srcDesc.Width, dstDesc.Width);
                box.bottom = (std::min)(srcDesc.Height, dstDesc.Height);
                box.back = 1;
                context->CopySubresourceRegion(dst, 0, 0, 0, 0, src, 0, &box);
            } else {
                static bool s_loggedFormatMismatch = false;
                if (!s_loggedFormatMismatch) {
                    LOG_ERROR("OpenXRFrameSubmitter: D3D11 copy failed on %s! Incompatible format family (src format %u != dst format %u)",
                              eyeName, srcDesc.Format, dstDesc.Format);
                    s_loggedFormatMismatch = true;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG_WARN("OpenXRFrameSubmitter: SEH intercepted CopyResource/CopySubresourceRegion on %s", eyeName);
        }
    };

    copyEye(leftEyeTex, leftDest, "LeftEye");
    copyEye(rightEyeTex, rightDest, "RightEye");

    // 6. Release Images
    state_ = SubmitterState::RELEASE;
    swapchainManager->ReleaseImages();

    // 7. End Frame
    state_ = SubmitterState::END_FRAME;
    
    // Create projection layer
    XrCompositionLayerProjectionView projectionViews[2] = {
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}
    };

    auto extentOf = [](ID3D11Texture2D* image) -> XrExtent2Di {
        if (!image) return XrExtent2Di{0, 0};
        D3D11_TEXTURE2D_DESC desc = {};
        image->GetDesc(&desc);
        return XrExtent2Di{static_cast<int32_t>(desc.Width), static_cast<int32_t>(desc.Height)};
    };

    // Left Eye - Native full surrounding headset FOV
    projectionViews[0].pose = lastLeftPose_;
    projectionViews[0].fov = lastLeftFov_;
    projectionViews[0].subImage.swapchain = swapchainManager->GetLeftSwapchain();
    projectionViews[0].subImage.imageArrayIndex = 0;
    projectionViews[0].subImage.imageRect.offset = {0, 0};
    projectionViews[0].subImage.imageRect.extent = extentOf(leftDest);

    // Right Eye - Native full surrounding headset FOV
    projectionViews[1].pose = lastRightPose_;
    projectionViews[1].fov = lastRightFov_;
    projectionViews[1].subImage.swapchain = swapchainManager->GetRightSwapchain();
    projectionViews[1].subImage.imageArrayIndex = 0;
    projectionViews[1].subImage.imageRect.offset = {0, 0};
    projectionViews[1].subImage.imageRect.extent = extentOf(rightDest);

    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.layerFlags = 0; // Opaque layer
    layer.space = referenceSpace;
    layer.viewCount = 2;
    layer.views = projectionViews;

    XrCompositionLayerQuad quadLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad cinemaLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[2];
    uint32_t layerCount = 1;

    if (cinemaQuadConfig_.enabled) {
        if (cinemaQuadConfig_.needsRecenter) {
            RecenterCinemaQuad(lastLeftPose_);
            cinemaQuadConfig_.needsRecenter = false;
        }

        cinemaLayer.layerFlags = 0; // Opaque layer
        cinemaLayer.space = referenceSpace;
        cinemaLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        cinemaLayer.subImage.swapchain = swapchainManager->GetLeftSwapchain();
        cinemaLayer.subImage.imageArrayIndex = 0;
        cinemaLayer.subImage.imageRect.offset = {0, 0};
        cinemaLayer.subImage.imageRect.extent = extentOf(leftDest);
        cinemaLayer.pose = cinemaQuadConfig_.pose;
        cinemaLayer.size = {cinemaQuadConfig_.sizeX, cinemaQuadConfig_.sizeY};

        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cinemaLayer);
    } else {
        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
    }

    if (hudQuadConfig_.enabled && hudQuadConfig_.swapchain != XR_NULL_HANDLE) {
        quadLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        quadLayer.space = referenceSpace;
        quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quadLayer.subImage.swapchain = hudQuadConfig_.swapchain;
        quadLayer.subImage.imageArrayIndex = 0;
        quadLayer.subImage.imageRect.offset = {0, 0};
        quadLayer.subImage.imageRect.extent.width = hudQuadConfig_.width;
        quadLayer.subImage.imageRect.extent.height = hudQuadConfig_.height;
        quadLayer.pose = hudQuadConfig_.pose;
        quadLayer.size = hudQuadConfig_.size;

        layers[1] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quadLayer);
        layerCount = 2;
    }

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = currentFrameState_.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = layerCount;
    endInfo.layers = layers;

    XrResult res = xrEndFrame(session, &endInfo);
    if (XR_FAILED(res)) {
        if (healthMonitor_) healthMonitor_->RecordFrameDropped();
        return false;
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    float latencyMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

    if (healthMonitor_) {
        healthMonitor_->RecordFrameSubmitted(latencyMs); 
    }

    return true;
}

bool OpenXRFrameSubmitter::BeginAndAcquireDX12(
    XrSession session,
    XrSpace referenceSpace,
    OpenXRSwapchainManager* swapchainManager,
    ID3D12Resource*& outLeftDest,
    ID3D12Resource*& outRightDest,
    XrPosef& outLeftPose, XrFovf& outLeftFov,
    XrPosef& outRightPose, XrFovf& outRightFov) 
{
    outLeftDest = nullptr;
    outRightDest = nullptr;
    outLeftPose = lastLeftPose_;
    outLeftFov = lastLeftFov_;
    outRightPose = lastRightPose_;
    outRightFov = lastRightFov_;

    if (!WaitAndBeginFrame(session)) return false;

    if (!currentFrameState_.shouldRender || !swapchainManager) {
        state_ = SubmitterState::END_FRAME;
        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = currentFrameState_.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = 0;
        xrEndFrame(session, &endInfo);
        return false;
    }

    // Locate views for head tracking
    LocateViews(session, referenceSpace, outLeftPose, outLeftFov, outRightPose, outRightFov);

    // 3. Acquire Images
    state_ = SubmitterState::ACQUIRE_IMAGES;
    uint32_t leftIndex = 0;
    uint32_t rightIndex = 0;
    if (!swapchainManager->AcquireImages(leftIndex, rightIndex)) {
        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = currentFrameState_.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = 0;
        xrEndFrame(session, &endInfo);
        return false;
    }

    // 4. Wait Images
    state_ = SubmitterState::WAIT_IMAGES;
    if (!swapchainManager->WaitImages()) {
        swapchainManager->ReleaseImages();
        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = currentFrameState_.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = 0;
        xrEndFrame(session, &endInfo);
        return false;
    }

    outLeftDest = swapchainManager->GetLeftSwapchainImageDX12(leftIndex);
    outRightDest = swapchainManager->GetRightSwapchainImageDX12(rightIndex);
    return true;
}

bool OpenXRFrameSubmitter::ReleaseAndEndDX12(
    XrSession session,
    XrSpace referenceSpace,
    OpenXRSwapchainManager* swapchainManager,
    uint32_t leftWidth, uint32_t leftHeight,
    uint32_t rightWidth, uint32_t rightHeight)
{
    // 6. Release Images
    state_ = SubmitterState::RELEASE;
    swapchainManager->ReleaseImages();

    // 7. End Frame
    state_ = SubmitterState::END_FRAME;
    
    XrCompositionLayerProjectionView projectionViews[2] = {
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}
    };

    // Left Eye - Native full surrounding headset FOV
    projectionViews[0].pose = lastLeftPose_;
    projectionViews[0].fov = lastLeftFov_;
    projectionViews[0].subImage.swapchain = swapchainManager->GetLeftSwapchain();
    projectionViews[0].subImage.imageArrayIndex = 0;
    projectionViews[0].subImage.imageRect.offset = {0, 0};
    projectionViews[0].subImage.imageRect.extent.width = leftWidth;
    projectionViews[0].subImage.imageRect.extent.height = leftHeight;
    
    // Right Eye - Native full surrounding headset FOV
    projectionViews[1].pose = lastRightPose_;
    projectionViews[1].fov = lastRightFov_;
    projectionViews[1].subImage.swapchain = swapchainManager->GetRightSwapchain();
    projectionViews[1].subImage.imageArrayIndex = 0;
    projectionViews[1].subImage.imageRect.offset = {0, 0};
    projectionViews[1].subImage.imageRect.extent.width = rightWidth;
    projectionViews[1].subImage.imageRect.extent.height = rightHeight;

    XrCompositionLayerProjection projectionLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projectionLayer.layerFlags = 0; // Opaque layer - do not blend with SteamVR void background
    projectionLayer.space = referenceSpace;
    projectionLayer.viewCount = 2;
    projectionLayer.views = projectionViews;

    XrCompositionLayerQuad quadLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad cinemaLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[2];
    uint32_t layerCount = 1;

    if (cinemaQuadConfig_.enabled) {
        if (cinemaQuadConfig_.needsRecenter) {
            RecenterCinemaQuad(lastLeftPose_);
            cinemaQuadConfig_.needsRecenter = false;
        }

        cinemaLayer.layerFlags = 0; // Opaque layer
        cinemaLayer.space = referenceSpace;
        cinemaLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        cinemaLayer.subImage.swapchain = swapchainManager->GetLeftSwapchain();
        cinemaLayer.subImage.imageArrayIndex = 0;
        cinemaLayer.subImage.imageRect.offset = {0, 0};
        cinemaLayer.subImage.imageRect.extent.width = leftWidth;
        cinemaLayer.subImage.imageRect.extent.height = leftHeight;
        cinemaLayer.pose = cinemaQuadConfig_.pose;
        cinemaLayer.size = {cinemaQuadConfig_.sizeX, cinemaQuadConfig_.sizeY};

        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cinemaLayer);
    } else {
        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projectionLayer);
    }

    if (hudQuadConfig_.enabled && hudQuadConfig_.swapchain != XR_NULL_HANDLE) {
        quadLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        quadLayer.space = referenceSpace;
        quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quadLayer.subImage.swapchain = hudQuadConfig_.swapchain;
        quadLayer.subImage.imageArrayIndex = 0;
        quadLayer.subImage.imageRect.offset = {0, 0};
        quadLayer.subImage.imageRect.extent.width = hudQuadConfig_.width;
        quadLayer.subImage.imageRect.extent.height = hudQuadConfig_.height;
        quadLayer.pose = hudQuadConfig_.pose;
        quadLayer.size = hudQuadConfig_.size;

        layers[1] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quadLayer);
        layerCount = 2;
    }

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = currentFrameState_.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = layerCount;
    endInfo.layers = layers;

    XrResult res = xrEndFrame(session, &endInfo);
    if (XR_FAILED(res)) {
        if (healthMonitor_) healthMonitor_->RecordFrameDropped();
        return false;
    }

    if (healthMonitor_) {
        // We'll track it from this point for now
        healthMonitor_->RecordFrameSubmitted(1.0f); // Default to 1.0f for DX12 for now unless we pass startTime
        
        if (healthMonitor_->GetFramesSubmitted() % 1000 == 0) {
            std::cout << "[OpenXR Telemetry] DX12 Frames Submitted: " << healthMonitor_->GetFramesSubmitted() 
                      << " | Predicted Display Time: " << currentFrameState_.predictedDisplayTime << "\n";
        }
    }

    return true;
}

bool OpenXRFrameSubmitter::BeginAndAcquireVulkan(
    XrSession session,
    XrSpace referenceSpace,
    OpenXRSwapchainManager* swapchainManager,
    VkImage& outLeftDest,
    VkImage& outRightDest,
    XrPosef& outLeftPose, XrFovf& outLeftFov,
    XrPosef& outRightPose, XrFovf& outRightFov) 
{
    outLeftDest = VK_NULL_HANDLE;
    outRightDest = VK_NULL_HANDLE;
    outLeftPose = lastLeftPose_;
    outLeftFov = lastLeftFov_;
    outRightPose = lastRightPose_;
    outRightFov = lastRightFov_;

    if (!WaitAndBeginFrame(session)) return false;

    if (!currentFrameState_.shouldRender || !swapchainManager) {
        state_ = SubmitterState::END_FRAME;
        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = currentFrameState_.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = 0;
        xrEndFrame(session, &endInfo);
        return false;
    }

    // Locate views for head tracking
    LocateViews(session, referenceSpace, outLeftPose, outLeftFov, outRightPose, outRightFov);

    // 3. Acquire Images
    state_ = SubmitterState::ACQUIRE_IMAGES;
    uint32_t leftIndex = 0;
    uint32_t rightIndex = 0;
    if (!swapchainManager->AcquireImages(leftIndex, rightIndex)) {
        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = currentFrameState_.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = 0;
        xrEndFrame(session, &endInfo);
        return false;
    }

    // 4. Wait Images
    state_ = SubmitterState::WAIT_IMAGES;
    if (!swapchainManager->WaitImages()) {
        swapchainManager->ReleaseImages();
        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = currentFrameState_.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = 0;
        xrEndFrame(session, &endInfo);
        return false;
    }

    outLeftDest = swapchainManager->GetLeftSwapchainImageVulkan(leftIndex);
    outRightDest = swapchainManager->GetRightSwapchainImageVulkan(rightIndex);
    return true;
}

bool OpenXRFrameSubmitter::ReleaseAndEndVulkan(
    XrSession session,
    XrSpace referenceSpace,
    OpenXRSwapchainManager* swapchainManager,
    uint32_t leftWidth, uint32_t leftHeight,
    uint32_t rightWidth, uint32_t rightHeight)
{
    // 6. Release Images
    state_ = SubmitterState::RELEASE;
    swapchainManager->ReleaseImages();

    // 7. End Frame
    state_ = SubmitterState::END_FRAME;
    
    XrCompositionLayerProjectionView projectionViews[2] = {
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}
    };

    // Left Eye - Native full surrounding headset FOV
    projectionViews[0].pose = lastLeftPose_;
    projectionViews[0].fov = lastLeftFov_;
    projectionViews[0].subImage.swapchain = swapchainManager->GetLeftSwapchain();
    projectionViews[0].subImage.imageArrayIndex = 0;
    projectionViews[0].subImage.imageRect.offset = {0, 0};
    projectionViews[0].subImage.imageRect.extent.width = leftWidth;
    projectionViews[0].subImage.imageRect.extent.height = leftHeight;
    
    // Right Eye - Native full surrounding headset FOV
    projectionViews[1].pose = lastRightPose_;
    projectionViews[1].fov = lastRightFov_;
    projectionViews[1].subImage.swapchain = swapchainManager->GetRightSwapchain();
    projectionViews[1].subImage.imageArrayIndex = 0;
    projectionViews[1].subImage.imageRect.offset = {0, 0};
    projectionViews[1].subImage.imageRect.extent.width = rightWidth;
    projectionViews[1].subImage.imageRect.extent.height = rightHeight;

    XrCompositionLayerProjection projectionLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projectionLayer.layerFlags = 0; // Opaque layer
    projectionLayer.space = referenceSpace;
    projectionLayer.viewCount = 2;
    projectionLayer.views = projectionViews;

    XrCompositionLayerQuad quadLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad cinemaLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[2];
    uint32_t layerCount = 1;

    if (cinemaQuadConfig_.enabled) {
        if (cinemaQuadConfig_.needsRecenter) {
            RecenterCinemaQuad(lastLeftPose_);
            cinemaQuadConfig_.needsRecenter = false;
        }

        cinemaLayer.layerFlags = 0; // Opaque layer
        cinemaLayer.space = referenceSpace;
        cinemaLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        cinemaLayer.subImage.swapchain = swapchainManager->GetLeftSwapchain();
        cinemaLayer.subImage.imageArrayIndex = 0;
        cinemaLayer.subImage.imageRect.offset = {0, 0};
        cinemaLayer.subImage.imageRect.extent.width = leftWidth;
        cinemaLayer.subImage.imageRect.extent.height = leftHeight;
        cinemaLayer.pose = cinemaQuadConfig_.pose;
        cinemaLayer.size = {cinemaQuadConfig_.sizeX, cinemaQuadConfig_.sizeY};

        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cinemaLayer);
    } else {
        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projectionLayer);
    }

    if (hudQuadConfig_.enabled && hudQuadConfig_.swapchain != XR_NULL_HANDLE) {
        quadLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        quadLayer.space = referenceSpace;
        quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quadLayer.subImage.swapchain = hudQuadConfig_.swapchain;
        quadLayer.subImage.imageArrayIndex = 0;
        quadLayer.subImage.imageRect.offset = {0, 0};
        quadLayer.subImage.imageRect.extent.width = hudQuadConfig_.width;
        quadLayer.subImage.imageRect.extent.height = hudQuadConfig_.height;
        quadLayer.pose = hudQuadConfig_.pose;
        quadLayer.size = hudQuadConfig_.size;

        layers[1] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quadLayer);
        layerCount = 2;
    }

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = currentFrameState_.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = layerCount;
    endInfo.layers = layers;

    XrResult res = xrEndFrame(session, &endInfo);
    if (XR_FAILED(res)) {
        if (healthMonitor_) healthMonitor_->RecordFrameDropped();
        return false;
    }

    if (healthMonitor_) {
        healthMonitor_->RecordFrameSubmitted(1.0f);
        
        if (healthMonitor_->GetFramesSubmitted() % 1000 == 0) {
            std::cout << "[OpenXR Telemetry] Vulkan Frames Submitted: " << healthMonitor_->GetFramesSubmitted() 
                      << " | Predicted Display Time: " << currentFrameState_.predictedDisplayTime << "\n";
        }
    }

    return true;
}

void OpenXRFrameSubmitter::SetHudQuad(XrSwapchain swapchain, uint32_t width, uint32_t height, float distance, float sizeX, float sizeY) {
    hudQuadConfig_.enabled = (swapchain != XR_NULL_HANDLE && width > 0 && height > 0);
    hudQuadConfig_.swapchain = swapchain;
    hudQuadConfig_.width = width;
    hudQuadConfig_.height = height;
    hudQuadConfig_.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    hudQuadConfig_.pose.position = {0.0f, 0.0f, -distance};
    hudQuadConfig_.size = {sizeX, sizeY};
}

void OpenXRFrameSubmitter::SetHudQuadAnchored(XrSwapchain swapchain, uint32_t width, uint32_t height, const XrPosef& headPose, bool recenter, float distance, float sizeX, float sizeY) {
    bool wasEnabled = hudQuadConfig_.enabled;
    hudQuadConfig_.enabled = (swapchain != XR_NULL_HANDLE && width > 0 && height > 0);
    hudQuadConfig_.swapchain = swapchain;
    hudQuadConfig_.width = width;
    hudQuadConfig_.height = height;
    hudQuadConfig_.size = {sizeX, sizeY};

    if (hudQuadConfig_.enabled && (recenter || !wasEnabled)) {
        float x = headPose.orientation.x;
        float y = headPose.orientation.y;
        float z = headPose.orientation.z;
        float w = headPose.orientation.w;

        // Forward vector (-Z rotated by head orientation) leveled onto horizontal X-Z plane
        float vx = -2.0f * (x * z + y * w);
        float vz = -(1.0f - 2.0f * (x * x + y * y));
        float len = std::sqrt(vx * vx + vz * vz);
        if (len < 0.001f) {
            vx = 0.0f;
            vz = -1.0f;
        } else {
            vx /= len;
            vz /= len;
        }

        float yaw = std::atan2(-vx, -vz);
        float halfYaw = yaw * 0.5f;
        hudQuadConfig_.pose.orientation = {0.0f, std::sin(halfYaw), 0.0f, std::cos(halfYaw)};
        hudQuadConfig_.pose.position = {
            headPose.position.x + vx * distance,
            headPose.position.y,
            headPose.position.z + vz * distance
        };
    }
}

void OpenXRFrameSubmitter::ClearHudQuad() {
    hudQuadConfig_.enabled = false;
    hudQuadConfig_.swapchain = XR_NULL_HANDLE;
    hudQuadConfig_.width = 0;
    hudQuadConfig_.height = 0;
}

void OpenXRFrameSubmitter::SetCinemaQuad(bool enabled, float distance, float width, float height) {
    cinemaQuadConfig_.enabled = enabled;
    if (distance >= 0.5f) {
        cinemaQuadConfig_.distance = distance;
    }
    float aspect = (height > 0.0f) ? (width / height) : (16.0f / 9.0f);
    if (aspect < 0.5f || aspect > 4.0f) aspect = 16.0f / 9.0f;

    cinemaQuadConfig_.sizeX = 3.2f;
    cinemaQuadConfig_.sizeY = cinemaQuadConfig_.sizeX / aspect;
}

void OpenXRFrameSubmitter::RequestCinemaRecenter() {
    cinemaQuadConfig_.needsRecenter = true;
}

void OpenXRFrameSubmitter::RecenterCinemaQuad(const XrPosef& headPose) {
    float x = headPose.orientation.x;
    float y = headPose.orientation.y;
    float z = headPose.orientation.z;
    float w = headPose.orientation.w;

    // Forward vector in OpenXR reference space (-Z rotated by head orientation)
    float vx = -2.0f * (x * z + y * w);
    float vy = -2.0f * (y * z - x * w);
    float vz = -(1.0f - 2.0f * (x * x + y * y));

    // Project onto horizontal X-Z plane to keep the virtual cinema screen level with gravity
    float len = std::sqrt(vx * vx + vz * vz);
    if (len < 0.001f) {
        vx = 0.0f;
        vz = -1.0f;
    } else {
        vx /= len;
        vz /= len;
    }

    // Yaw angle around Y axis
    float yaw = std::atan2(-vx, -vz);
    float halfYaw = yaw * 0.5f;

    cinemaQuadConfig_.pose.orientation.x = 0.0f;
    cinemaQuadConfig_.pose.orientation.y = std::sin(halfYaw);
    cinemaQuadConfig_.pose.orientation.z = 0.0f;
    cinemaQuadConfig_.pose.orientation.w = std::cos(halfYaw);

    // Anchor screen at cinema distance in front of current head position, at eye level
    cinemaQuadConfig_.pose.position.x = headPose.position.x + vx * cinemaQuadConfig_.distance;
    cinemaQuadConfig_.pose.position.y = headPose.position.y;
    cinemaQuadConfig_.pose.position.z = headPose.position.z + vz * cinemaQuadConfig_.distance;
}

} // namespace openxr
} // namespace vrinject

