#include "rendering/dx12/imgui_dx12_integration.h"
#include "core/logger.h"
#include "core/overlay_manager.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "backends/imgui_impl_dx12.h"

namespace vrinject {

bool ImGuiDX12Integration::Initialize(ID3D12Device* device, DXGI_FORMAT rtvFormat) {
    if (m_initialized) return true;
    if (!device) return false;
    if (!ImGui::GetCurrentContext()) return false;

    // Create SRV Descriptor Heap for ImGui (needs 1 descriptor for font texture)
    D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = 1;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_srvDescHeap)))) {
        LOG_ERROR("ImGuiDX12: Failed to create SRV descriptor heap");
        return false;
    }

    // Create RTV Descriptor Heap for ImGui (2 descriptors for left and right eyes)
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = 2;
    rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvDescHeap)))) {
        LOG_ERROR("ImGuiDX12: Failed to create RTV descriptor heap");
        return false;
    }
    m_rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    if (!ImGui_ImplDX12_Init(device, 3,
        rtvFormat,
        m_srvDescHeap.Get(),
        m_srvDescHeap->GetCPUDescriptorHandleForHeapStart(),
        m_srvDescHeap->GetGPUDescriptorHandleForHeapStart())) {
        LOG_ERROR("ImGuiDX12: ImGui_ImplDX12_Init failed!");
        return false;
    }

    m_initialized = true;
    LOG_INFO("ImGuiDX12: Initialized successfully with unified command list integration.");
    return true;
}

void ImGuiDX12Integration::Render(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* leftDest, ID3D12Resource* rightDest) {
    if (!m_initialized || !device || !cmdList || !leftDest) return;

    // Set Descriptor Heaps
    ID3D12DescriptorHeap* heaps[] = { m_srvDescHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);

    // Generate ImGui DrawData ONCE per frame
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    OverlayManager::GetInstance().Render();
    ImDrawData* drawData = ImGui::GetDrawData();

    if (drawData && drawData->TotalVtxCount > 0) {
        const ImVec2 origDisplayPos = drawData->DisplayPos;
        const float stereoOffset = static_cast<float>(leftDest->GetDesc().Width) * 0.022f;

        // 1. Draw into Left Eye (slot 0, shifted nasally right)
        D3D12_CPU_DESCRIPTOR_HANDLE rtvLeft = m_rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = leftDest->GetDesc().Format;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(leftDest, &rtvDesc, rtvLeft);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = leftDest;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cmdList->ResourceBarrier(1, &barrier);

        drawData->DisplayPos.x = origDisplayPos.x - stereoOffset;
        cmdList->OMSetRenderTargets(1, &rtvLeft, FALSE, nullptr);
        ImGui_ImplDX12_RenderDrawData(drawData, cmdList);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        cmdList->ResourceBarrier(1, &barrier);

        // 2. Draw directly into Right Eye (slot 1, shifted nasally left) without overwriting Right Eye stereo image
        if (rightDest && rightDest != leftDest) {
            D3D12_CPU_DESCRIPTOR_HANDLE rtvRight = m_rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
            rtvRight.ptr += m_rtvDescriptorSize;
            D3D12_RENDER_TARGET_VIEW_DESC rtvDescRight = {};
            rtvDescRight.Format = rightDest->GetDesc().Format;
            rtvDescRight.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device->CreateRenderTargetView(rightDest, &rtvDescRight, rtvRight);

            D3D12_RESOURCE_BARRIER rightBarrier = {};
            rightBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            rightBarrier.Transition.pResource = rightDest;
            rightBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            rightBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            rightBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            cmdList->ResourceBarrier(1, &rightBarrier);

            drawData->DisplayPos.x = origDisplayPos.x + stereoOffset;
            cmdList->OMSetRenderTargets(1, &rtvRight, FALSE, nullptr);
            ImGui_ImplDX12_RenderDrawData(drawData, cmdList);

            rightBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            rightBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            cmdList->ResourceBarrier(1, &rightBarrier);
        }

        drawData->DisplayPos = origDisplayPos;
    }
}

void ImGuiDX12Integration::Shutdown() {
    if (m_initialized) {
        ImGui_ImplDX12_Shutdown();
        m_srvDescHeap.Reset();
        m_rtvDescHeap.Reset();
        m_initialized = false;
    }
}

} // namespace vrinject
