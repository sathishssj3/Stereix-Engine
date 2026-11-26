#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

namespace vrinject {
namespace DXGIFactoryHook {

    bool Initialize();
    void Shutdown();

    // Returns the captured command queue if it matches the swapchain's device
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> GetCapturedCommandQueue();
    void SetCapturedCommandQueue(ID3D12CommandQueue* queue);
    void ClearCapturedCommandQueue();

    // Suppresses swapchain/queue capture while Stereix creates internal dummy devices/swapchains (HIGH-12)
    void BeginInternalInit();
    void EndInternalInit();
    bool IsInternalInitSuppressed();

    class ScopedCaptureSuppression {
    public:
        ScopedCaptureSuppression() { BeginInternalInit(); }
        ~ScopedCaptureSuppression() { EndInternalInit(); }
        ScopedCaptureSuppression(const ScopedCaptureSuppression&) = delete;
        ScopedCaptureSuppression& operator=(const ScopedCaptureSuppression&) = delete;
    };

}
}
