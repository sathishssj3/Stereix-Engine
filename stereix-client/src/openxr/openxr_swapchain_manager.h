#pragma once

#include "openxr/openxr_types.h"
#include "openxr/openxr_health_monitor.h"
#include <d3d11.h>
#include <d3d12.h>
#include <vector>
#include "core/graphics_types.h"

namespace vrinject {
namespace openxr {

class OpenXRSwapchainManager {
public:
    OpenXRSwapchainManager(OpenXRHealthMonitor* healthMonitor);
    ~OpenXRSwapchainManager();

    static int64_t SelectOptimalFormat(XrSession session, GraphicsBackend backendAPI, int64_t gameFormat);

    bool Initialize(XrSession session, int64_t format, uint32_t width, uint32_t height, GraphicsBackend backendAPI);
    void Shutdown();

    bool AcquireImages(uint32_t& outLeftIndex, uint32_t& outRightIndex);
    bool WaitImages();
    bool ReleaseImages();

    // Floating HUD Quad Swapchain
    bool CreateHudSwapchain(XrSession session, int64_t format, uint32_t width, uint32_t height, GraphicsBackend backendAPI);
    XrSwapchain GetHudSwapchain() const { return hudSwapchain_; }
    uint32_t GetHudWidth() const { return hudWidth_; }
    uint32_t GetHudHeight() const { return hudHeight_; }
    bool AcquireHudImage(uint32_t& outIndex);
    bool WaitHudImage();
    bool ReleaseHudImage();

    ID3D11Texture2D* GetLeftSwapchainImage(uint32_t index) const;
    ID3D11Texture2D* GetRightSwapchainImage(uint32_t index) const;
    ID3D11Texture2D* GetHudSwapchainImageD3D11(uint32_t index) const;

    ID3D12Resource* GetLeftSwapchainImageDX12(uint32_t index) const;
    ID3D12Resource* GetRightSwapchainImageDX12(uint32_t index) const;
    ID3D12Resource* GetHudSwapchainImageDX12(uint32_t index) const;

    VkImage GetLeftSwapchainImageVulkan(uint32_t index) const;
    VkImage GetRightSwapchainImageVulkan(uint32_t index) const;
    VkImage GetHudSwapchainImageVulkan(uint32_t index) const;

    XrSwapchain GetLeftSwapchain() const { return leftSwapchain_; }
    XrSwapchain GetRightSwapchain() const { return rightSwapchain_; }

    uint32_t GetWidth() const { return width_; }
    uint32_t GetHeight() const { return height_; }

    int64_t GetFormat() const { return format_; }
    bool IsSrgb() const { return format_ == 29 || format_ == 91 || format_ == 43 || format_ == 50; }

private:
    bool CreateSwapchain(XrSession session, int64_t format, uint32_t width, uint32_t height, XrSwapchain& outSwapchain, GraphicsBackend backendAPI, bool isLeft);

    OpenXRHealthMonitor* healthMonitor_ = nullptr;
    int64_t format_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    
    XrSwapchain leftSwapchain_ = XR_NULL_HANDLE;
    XrSwapchain rightSwapchain_ = XR_NULL_HANDLE;

    // HUD Quad Swapchain handles
    XrSwapchain hudSwapchain_ = XR_NULL_HANDLE;
    uint32_t hudWidth_ = 0;
    uint32_t hudHeight_ = 0;
    std::vector<XrSwapchainImageD3D11KHR> hudImagesD3D11_;
    std::vector<XrSwapchainImageD3D12KHR> hudImagesD3D12_;
    std::vector<XrSwapchainImageVulkanKHR> hudImagesVulkan_;

    std::vector<XrSwapchainImageD3D11KHR> leftImagesD3D11_;
    std::vector<XrSwapchainImageD3D11KHR> rightImagesD3D11_;
    std::vector<XrSwapchainImageD3D12KHR> leftImagesD3D12_;
    std::vector<XrSwapchainImageD3D12KHR> rightImagesD3D12_;
    std::vector<XrSwapchainImageVulkanKHR> leftImagesVulkan_;
    std::vector<XrSwapchainImageVulkanKHR> rightImagesVulkan_;
};

} // namespace openxr
} // namespace vrinject
