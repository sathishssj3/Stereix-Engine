#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <mutex>
#include "heuristics/camera_snapshot.h"
#include "heuristics/depth_snapshot.h"
#include "rendering/dx12/dx12_fence_manager.h"
#include "rendering/dx12/dx12_pipeline_state_cache.h"
#include "rendering/dx12/dx12_resource_state_tracker.h"
#include "rendering/dx12/dx12_stereo_resource_manager.h"
#include "rendering/dx12/dx12_lifecycle_manager.h"

namespace vrinject {

class DX12StereoRenderer {
public:
    DX12StereoRenderer() = default;
    ~DX12StereoRenderer();

    bool Initialize(ID3D12Device* device);
    void Shutdown();

    // Waits on the next ring-buffer slot's fence and returns that slot index BEFORE
    // DX12StereoResourceManager updates CBV/SRV descriptors for the frame (HIGH-08).
    uint32_t PrepareNextFrameSlot(DX12FenceManager* fenceManager);

    // Renders the stereo reprojection. Returns true on success, false if degraded/skipped.
    // Enforces the single-queue constraint.
    bool RenderStereo(
        ID3D12CommandQueue* gameQueue,
        DX12StereoResourceManager* resourceManager,
        DX12PipelineStateCache* psoCache,
        DX12ResourceStateTracker* stateTracker,
        DX12FenceManager* fenceManager,
        const CameraSnapshot& cam,
        const DepthSnapshot& depth,
        ID3D12Resource* oxrLeftDest = nullptr,
        ID3D12Resource* oxrRightDest = nullptr,
        bool shouldAttemptStereo = true,
        ID3D12Resource* uiMask = nullptr,
        ID3D12Resource* oxrHudDest = nullptr);

private:
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_commandAllocators[MAX_FRAMES_IN_FLIGHT];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandLists[MAX_FRAMES_IN_FLIGHT];
    uint64_t m_frameFences[MAX_FRAMES_IN_FLIGHT] = {0, 0, 0};
    uint32_t m_currentFrameIndex = 0;
    bool m_hasPreparedSlot = false;
    uint32_t m_preparedSlotIndex = 0;
    
    mutable std::mutex m_mutex;
    
    bool m_isDegraded = false;
};

} // namespace vrinject
