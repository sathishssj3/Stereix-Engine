#include "rendering/dx12/dx12_stereo_renderer.h"
#include "rendering/dx12/imgui_dx12_integration.h"
#include "core/overlay_manager.h"
#include <iostream>
#include <cassert>

namespace vrinject {

static bool SafeGetResourceDesc(ID3D12Resource* res, D3D12_RESOURCE_DESC& outDesc) {
    if (!res) return false;
    __try {
        outDesc = res->GetDesc();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

DX12StereoRenderer::~DX12StereoRenderer() {
    Shutdown();
}

bool DX12StereoRenderer::Initialize(ID3D12Device* device) {
    if (!device) return false;

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[i])))) {
            return false;
        }
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocators[i].Get(), nullptr, IID_PPV_ARGS(&m_commandLists[i])))) {
            return false;
        }
        // Command list starts in recording state, close it immediately since we reset it per frame
        m_commandLists[i]->Close();
    }

    return true;
}

void DX12StereoRenderer::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        m_commandLists[i].Reset();
        m_commandAllocators[i].Reset();
    }
}

bool DX12StereoRenderer::RenderStereo(
    ID3D12CommandQueue* gameQueue,
    DX12StereoResourceManager* resourceManager,
    DX12PipelineStateCache* psoCache,
    DX12ResourceStateTracker* stateTracker,
    DX12FenceManager* fenceManager,
    const CameraSnapshot& cam,
    const DepthSnapshot& depth,
    ID3D12Resource* oxrLeftDest,
    ID3D12Resource* oxrRightDest,
    bool shouldAttemptStereo,
    ID3D12Resource* uiMask) 
{
    if (!gameQueue || !resourceManager || !psoCache || !stateTracker || !fenceManager) return false;

    // Synchronization Contract (Single Queue Assertion)
    // The dispatch MUST submit to the game's queue to guarantee FIFO ordering
    ID3D12CommandQueue* mainQueue = Dx12LifecycleManager::Get().GetMainQueue();
    if (mainQueue && gameQueue == mainQueue) {
        m_isDegraded = false;
    } else if (mainQueue && gameQueue != mainQueue) {
        std::cerr << "CRITICAL ERROR: StereoRenderer dispatch queue does not match LifecycleManager MainQueue." << std::endl;
        m_isDegraded = true;
    }
    
    std::lock_guard<std::mutex> lock(m_mutex);

    // Frame Sequence: advance ring buffer monotonically across frames
    uint32_t frameIndex = (m_currentFrameIndex++) % MAX_FRAMES_IN_FLIGHT;

    // Ensure the GPU has finished with this allocator before resetting it
    if (m_frameFences[frameIndex] != 0) {
        fenceManager->WaitForFenceValue(m_frameFences[frameIndex]);
    }
    
    m_commandAllocators[frameIndex]->Reset();
    ID3D12GraphicsCommandList* cmdList = m_commandLists[frameIndex].Get();
    cmdList->Reset(m_commandAllocators[frameIndex].Get(), nullptr);

    ID3D12Resource* camTex = (cam.IsValid() && cam.resourceIdentity.nativeHandle) ? 
        reinterpret_cast<ID3D12Resource*>(cam.resourceIdentity.nativeHandle) : 
        Dx12LifecycleManager::Get().GetBackBuffer();
    if (!camTex) {
        camTex = Dx12LifecycleManager::Get().GetBackBuffer();
    }

    ID3D12Resource* depthTex = (depth.IsValid() && depth.identity.nativeHandle) ? 
        reinterpret_cast<ID3D12Resource*>(depth.identity.nativeHandle) : 
        camTex;
    
    if (!camTex) {
        cmdList->Close();
        return true;
    }

    bool isBackBuffer = (camTex == Dx12LifecycleManager::Get().GetBackBuffer());
    D3D12_RESOURCE_STATES camInitialState = isBackBuffer ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_RENDER_TARGET;

    // Check PSO availability once, before recording any commands
    ID3D12Device* device = nullptr;
    gameQueue->GetDevice(IID_PPV_ARGS(&device));
    bool hasPso = false;
    ID3D12RootSignature* rootSig = nullptr;
    ID3D12PipelineState* pso = nullptr;
    if (device) {
        rootSig = psoCache->GetStereoRootSignature(device);
        pso = psoCache->GetStereoPSO(device);
        hasPso = (rootSig != nullptr && pso != nullptr);
        device->Release();
    }

    ID3D12Resource* leftEye = resourceManager->GetLeftEyeTexture();
    ID3D12Resource* rightEye = resourceManager->GetRightEyeTexture();

    // Verify depth texture is legally accessible as a Shader Resource View in compute shaders
    bool isDepthShaderReadable = false;
    if (depthTex && depthTex != camTex) {
        D3D12_RESOURCE_DESC dDesc = {};
        if (SafeGetResourceDesc(depthTex, dDesc)) {
            bool denyShader = (dDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0;
            bool isTypelessOrFloat = (
                dDesc.Format == DXGI_FORMAT_R32G8X24_TYPELESS ||
                dDesc.Format == DXGI_FORMAT_R32_TYPELESS ||
                dDesc.Format == DXGI_FORMAT_R24G8_TYPELESS ||
                dDesc.Format == DXGI_FORMAT_R16_TYPELESS ||
                dDesc.Format == DXGI_FORMAT_R32_FLOAT ||
                dDesc.Format == DXGI_FORMAT_R16_UNORM
            );
            isDepthShaderReadable = (!denyShader && isTypelessOrFloat);
        }
    }

    bool isStereoValid = !m_isDegraded && shouldAttemptStereo && hasPso && leftEye && rightEye &&
                         (cam.IsValid() && depth.IsValid() && cam.resourceIdentity.nativeHandle && depth.identity.nativeHandle) &&
                         isDepthShaderReadable;

    bool canRunShader = (hasPso && leftEye && rightEye && camTex);
    if (!canRunShader) {
        if (camTex && oxrLeftDest && oxrRightDest) {
            stateTracker->ForceResourceState(camTex, camInitialState);
            stateTracker->TransitionResource(camTex, D3D12_RESOURCE_STATE_COPY_SOURCE);
            stateTracker->ForceResourceState(oxrLeftDest, D3D12_RESOURCE_STATE_COMMON);
            stateTracker->TransitionResource(oxrLeftDest, D3D12_RESOURCE_STATE_COPY_DEST);
            stateTracker->ForceResourceState(oxrRightDest, D3D12_RESOURCE_STATE_COMMON);
            stateTracker->TransitionResource(oxrRightDest, D3D12_RESOURCE_STATE_COPY_DEST);
            stateTracker->FlushBarriers(cmdList);

            cmdList->CopyResource(oxrLeftDest, camTex);
            cmdList->CopyResource(oxrRightDest, camTex);

            stateTracker->TransitionResource(camTex, camInitialState);
            stateTracker->TransitionResource(oxrLeftDest, D3D12_RESOURCE_STATE_COMMON);
            stateTracker->TransitionResource(oxrRightDest, D3D12_RESOURCE_STATE_COMMON);
            stateTracker->FlushBarriers(cmdList);
        }
        cmdList->Close();
        ID3D12CommandList* ppCommandLists[] = { cmdList };
        gameQueue->ExecuteCommandLists(1, ppCommandLists);
        m_frameFences[frameIndex] = fenceManager->Signal(gameQueue);
        return false;
    }

    stateTracker->ForceResourceState(camTex, camInitialState);
    stateTracker->TransitionResource(camTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if (isStereoValid && depthTex && depthTex != camTex) {
        stateTracker->ForceResourceState(depthTex, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        stateTracker->TransitionResource(depthTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    stateTracker->TransitionResource(leftEye, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    stateTracker->TransitionResource(rightEye, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    stateTracker->FlushBarriers(cmdList);

    // Bind Descriptor Heaps
    resourceManager->BindDescriptorHeaps(cmdList);

    // Bind Root Signature & Pipeline State
    cmdList->SetComputeRootSignature(rootSig);
    cmdList->SetPipelineState(pso);

    // Bind Constants & Resources
    cmdList->SetComputeRootDescriptorTable(0, resourceManager->GetCbvGpuHandle());
    cmdList->SetComputeRootDescriptorTable(1, resourceManager->GetSrvGpuHandle());
    cmdList->SetComputeRootDescriptorTable(2, resourceManager->GetUavGpuHandle());

    // Dispatch Compute (8x8 thread groups over eye dimensions)
    UINT eyeW = resourceManager->GetWidth();
    UINT eyeH = resourceManager->GetHeight();
    if (eyeW == 0) eyeW = 1920;
    if (eyeH == 0) eyeH = 1080;
    UINT dispatchX = (eyeW + 7) / 8;
    UINT dispatchY = (eyeH + 7) / 8;
    cmdList->Dispatch(dispatchX, dispatchY, 1);

    // UAV Barrier to ensure compute writes are visible before copy to swapchain
    D3D12_RESOURCE_BARRIER uavBarriers[2] = {};
    uavBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[0].UAV.pResource = leftEye;
    uavBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[1].UAV.pResource = rightEye;
    cmdList->ResourceBarrier(2, uavBarriers);

    // If in-headset menu is active, draw it directly onto eye buffers in the same command list
    if (OverlayManager::GetInstance().IsOverlayVisible()) {
        DXGI_FORMAT rtvFmt = leftEye->GetDesc().Format;
        ImGuiDX12Integration::GetInstance().Initialize(device, rtvFmt);
        ImGuiDX12Integration::GetInstance().Render(device, cmdList, leftEye, rightEye);
    }

    // Transition Outputs for OpenXR Copy Source
    stateTracker->TransitionResource(leftEye, D3D12_RESOURCE_STATE_COPY_SOURCE);
    stateTracker->TransitionResource(rightEye, D3D12_RESOURCE_STATE_COPY_SOURCE);
    
    // Transition OpenXR swapchain images to COPY_DEST
    if (oxrLeftDest) {
        stateTracker->ForceResourceState(oxrLeftDest, D3D12_RESOURCE_STATE_COMMON);
        stateTracker->TransitionResource(oxrLeftDest, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (oxrRightDest) {
        stateTracker->ForceResourceState(oxrRightDest, D3D12_RESOURCE_STATE_COMMON);
        stateTracker->TransitionResource(oxrRightDest, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    stateTracker->FlushBarriers(cmdList);

    // Copy to OpenXR Swapchain (exact dimensions match eye textures)
    if (oxrLeftDest) {
        cmdList->CopyResource(oxrLeftDest, leftEye);
    }
    if (oxrRightDest) {
        cmdList->CopyResource(oxrRightDest, rightEye);
    }

    // Transition OpenXR swapchain images back to COMMON for OpenXR presentation
    if (oxrLeftDest) {
        stateTracker->TransitionResource(oxrLeftDest, D3D12_RESOURCE_STATE_COMMON);
    }
    if (oxrRightDest) {
        stateTracker->TransitionResource(oxrRightDest, D3D12_RESOURCE_STATE_COMMON);
    }
    
    // Transition game inputs back to standard states so the engine's next frame does not encounter barrier conflicts.
    // The game's depth buffer must be strictly restored to DEPTH_WRITE so that subsequent frame depth writes
    // (such as terrain Z-prepass, G-buffer, and cascaded shadow maps) do not fail or corrupt depth metadata.
    stateTracker->TransitionResource(camTex, camInitialState);
    if (isStereoValid && depthTex && depthTex != camTex) {
        stateTracker->TransitionResource(depthTex, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }
    stateTracker->FlushBarriers(cmdList);

    // Execute Command List
    cmdList->Close();
    ID3D12CommandList* ppCommandLists[] = { cmdList };
    gameQueue->ExecuteCommandLists(1, ppCommandLists);

    // Signal Fence (GPU Lifetime tracking)
    m_frameFences[frameIndex] = fenceManager->Signal(gameQueue);
    
    return true;
}

} // namespace vrinject
