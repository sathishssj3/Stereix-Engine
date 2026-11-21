#include "openxr/openxr_swapchain_manager.h"
#include "core/logger.h"
#include <iostream>

namespace vrinject {
namespace openxr {

OpenXRSwapchainManager::OpenXRSwapchainManager(OpenXRHealthMonitor* healthMonitor)
    : healthMonitor_(healthMonitor) {}

OpenXRSwapchainManager::~OpenXRSwapchainManager() {
    Shutdown();
}

bool OpenXRSwapchainManager::Initialize(XrSession session, int64_t format, uint32_t width, uint32_t height, GraphicsBackend backendAPI) {
    format_ = format;
    width_ = width;
    height_ = height;
    if (!CreateSwapchain(session, format, width, height, leftSwapchain_, backendAPI, true)) {
        return false;
    }
    if (!CreateSwapchain(session, format, width, height, rightSwapchain_, backendAPI, false)) {
        return false;
    }
    return true;
}

bool OpenXRSwapchainManager::CreateSwapchain(XrSession session, int64_t format, uint32_t width, uint32_t height, XrSwapchain& outSwapchain, GraphicsBackend backendAPI, bool isLeft) {
    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.arraySize = 1;
    createInfo.format = format;
    createInfo.width = width;
    createInfo.height = height;
    createInfo.mipCount = 1;
    createInfo.faceCount = 1;
    createInfo.sampleCount = 1;
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

    XrResult res = xrCreateSwapchain(session, &createInfo, &outSwapchain);
    if (XR_FAILED(res)) {
        std::cerr << "Failed to create OpenXR swapchain.\n";
        return false;
    }

    uint32_t imageCount = 0;
    xrEnumerateSwapchainImages(outSwapchain, 0, &imageCount, nullptr);
    
    if (backendAPI == GraphicsBackend::DX12) {
        std::vector<XrSwapchainImageD3D12KHR>& outImages = isLeft ? leftImagesD3D12_ : rightImagesD3D12_;
        outImages.clear();
        outImages.resize(imageCount);
        for (uint32_t i = 0; i < imageCount; ++i) {
            outImages[i] = {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
            outImages[i].next = nullptr;
        }
        res = xrEnumerateSwapchainImages(outSwapchain, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(outImages.data()));
    } else if (backendAPI == GraphicsBackend::Vulkan) {
        std::vector<XrSwapchainImageVulkanKHR>& outImages = isLeft ? leftImagesVulkan_ : rightImagesVulkan_;
        outImages.clear();
        outImages.resize(imageCount);
        for (uint32_t i = 0; i < imageCount; ++i) {
            outImages[i] = {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR};
            outImages[i].next = nullptr;
        }
        res = xrEnumerateSwapchainImages(outSwapchain, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(outImages.data()));
    } else {
        std::vector<XrSwapchainImageD3D11KHR>& outImages = isLeft ? leftImagesD3D11_ : rightImagesD3D11_;
        outImages.clear();
        outImages.resize(imageCount);
        for (uint32_t i = 0; i < imageCount; ++i) {
            outImages[i] = {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
            outImages[i].next = nullptr;
        }
        res = xrEnumerateSwapchainImages(outSwapchain, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(outImages.data()));
    }
    
    if (XR_FAILED(res)) {
        std::cerr << "Failed to enumerate OpenXR swapchain images.\n";
        return false;
    }

    return true;
}

void OpenXRSwapchainManager::Shutdown() {
    if (leftSwapchain_ != XR_NULL_HANDLE) {
        xrDestroySwapchain(leftSwapchain_);
        leftSwapchain_ = XR_NULL_HANDLE;
    }
    if (rightSwapchain_ != XR_NULL_HANDLE) {
        xrDestroySwapchain(rightSwapchain_);
        rightSwapchain_ = XR_NULL_HANDLE;
    }
    if (hudSwapchain_ != XR_NULL_HANDLE) {
        xrDestroySwapchain(hudSwapchain_);
        hudSwapchain_ = XR_NULL_HANDLE;
    }
    leftImagesD3D11_.clear();
    rightImagesD3D11_.clear();
    leftImagesD3D12_.clear();
    rightImagesD3D12_.clear();
    leftImagesVulkan_.clear();
    rightImagesVulkan_.clear();
    hudImagesD3D11_.clear();
    hudImagesD3D12_.clear();
    hudImagesVulkan_.clear();
    hudWidth_ = 0;
    hudHeight_ = 0;
}

bool OpenXRSwapchainManager::CreateHudSwapchain(XrSession session, int64_t format, uint32_t width, uint32_t height, GraphicsBackend backendAPI) {
    if (hudSwapchain_ != XR_NULL_HANDLE) {
        xrDestroySwapchain(hudSwapchain_);
        hudSwapchain_ = XR_NULL_HANDLE;
        hudImagesD3D11_.clear();
        hudImagesD3D12_.clear();
        hudImagesVulkan_.clear();
    }

    hudWidth_ = width;
    hudHeight_ = height;

    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.arraySize = 1;
    createInfo.format = format;
    createInfo.width = width;
    createInfo.height = height;
    createInfo.mipCount = 1;
    createInfo.faceCount = 1;
    createInfo.sampleCount = 1;
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;

    XrResult res = xrCreateSwapchain(session, &createInfo, &hudSwapchain_);
    if (XR_FAILED(res)) {
        return false;
    }

    uint32_t imageCount = 0;
    xrEnumerateSwapchainImages(hudSwapchain_, 0, &imageCount, nullptr);

    if (backendAPI == GraphicsBackend::DX12) {
        hudImagesD3D12_.resize(imageCount);
        for (uint32_t i = 0; i < imageCount; ++i) {
            hudImagesD3D12_[i] = {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
        }
        res = xrEnumerateSwapchainImages(hudSwapchain_, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(hudImagesD3D12_.data()));
    } else if (backendAPI == GraphicsBackend::Vulkan) {
        hudImagesVulkan_.resize(imageCount);
        for (uint32_t i = 0; i < imageCount; ++i) {
            hudImagesVulkan_[i] = {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR};
        }
        res = xrEnumerateSwapchainImages(hudSwapchain_, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(hudImagesVulkan_.data()));
    } else {
        hudImagesD3D11_.resize(imageCount);
        for (uint32_t i = 0; i < imageCount; ++i) {
            hudImagesD3D11_[i] = {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
        }
        res = xrEnumerateSwapchainImages(hudSwapchain_, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(hudImagesD3D11_.data()));
    }

    return XR_SUCCEEDED(res);
}

bool OpenXRSwapchainManager::AcquireHudImage(uint32_t& outIndex) {
    if (hudSwapchain_ == XR_NULL_HANDLE) return false;
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    return XR_SUCCEEDED(xrAcquireSwapchainImage(hudSwapchain_, &acquireInfo, &outIndex));
}

bool OpenXRSwapchainManager::WaitHudImage() {
    if (hudSwapchain_ == XR_NULL_HANDLE) return false;
    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = 50000000; // 50ms in nanoseconds
    return XR_SUCCEEDED(xrWaitSwapchainImage(hudSwapchain_, &waitInfo));
}

bool OpenXRSwapchainManager::ReleaseHudImage() {
    if (hudSwapchain_ == XR_NULL_HANDLE) return false;
    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    return XR_SUCCEEDED(xrReleaseSwapchainImage(hudSwapchain_, &releaseInfo));
}

bool OpenXRSwapchainManager::AcquireImages(uint32_t& outLeftIndex, uint32_t& outRightIndex) {
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    
    XrResult resL = xrAcquireSwapchainImage(leftSwapchain_, &acquireInfo, &outLeftIndex);
    if (XR_FAILED(resL)) {
        if (healthMonitor_) healthMonitor_->RecordAcquireTimeout();
        return false;
    }

    XrResult resR = xrAcquireSwapchainImage(rightSwapchain_, &acquireInfo, &outRightIndex);
    if (XR_FAILED(resR)) {
        if (healthMonitor_) healthMonitor_->RecordAcquireTimeout();
        return false;
    }
    return true;
}

bool OpenXRSwapchainManager::WaitImages() {
    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    // 50ms bounded timeout prevents freezing/hanging the game loop if runtime stalls
    waitInfo.timeout = 50000000; // 50ms in nanoseconds

    XrResult resL = xrWaitSwapchainImage(leftSwapchain_, &waitInfo);
    if (XR_FAILED(resL)) return false;

    XrResult resR = xrWaitSwapchainImage(rightSwapchain_, &waitInfo);
    if (XR_FAILED(resR)) return false;

    return true;
}

bool OpenXRSwapchainManager::ReleaseImages() {
    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};

    XrResult resL = xrReleaseSwapchainImage(leftSwapchain_, &releaseInfo);
    if (XR_FAILED(resL)) return false;

    XrResult resR = xrReleaseSwapchainImage(rightSwapchain_, &releaseInfo);
    if (XR_FAILED(resR)) return false;

    return true;
}

ID3D11Texture2D* OpenXRSwapchainManager::GetLeftSwapchainImage(uint32_t index) const {
    if (index >= leftImagesD3D11_.size()) return nullptr;
    return leftImagesD3D11_[index].texture;
}

ID3D11Texture2D* OpenXRSwapchainManager::GetRightSwapchainImage(uint32_t index) const {
    if (index >= rightImagesD3D11_.size()) return nullptr;
    return rightImagesD3D11_[index].texture;
}

ID3D11Texture2D* OpenXRSwapchainManager::GetHudSwapchainImageD3D11(uint32_t index) const {
    if (index >= hudImagesD3D11_.size()) return nullptr;
    return hudImagesD3D11_[index].texture;
}

ID3D12Resource* OpenXRSwapchainManager::GetLeftSwapchainImageDX12(uint32_t index) const {
    if (index >= leftImagesD3D12_.size()) return nullptr;
    return leftImagesD3D12_[index].texture;
}

ID3D12Resource* OpenXRSwapchainManager::GetRightSwapchainImageDX12(uint32_t index) const {
    if (index >= rightImagesD3D12_.size()) return nullptr;
    return rightImagesD3D12_[index].texture;
}

ID3D12Resource* OpenXRSwapchainManager::GetHudSwapchainImageDX12(uint32_t index) const {
    if (index >= hudImagesD3D12_.size()) return nullptr;
    return hudImagesD3D12_[index].texture;
}

VkImage OpenXRSwapchainManager::GetLeftSwapchainImageVulkan(uint32_t index) const {
    if (index >= leftImagesVulkan_.size()) return VK_NULL_HANDLE;
    return leftImagesVulkan_[index].image;
}

VkImage OpenXRSwapchainManager::GetRightSwapchainImageVulkan(uint32_t index) const {
    if (index >= rightImagesVulkan_.size()) return VK_NULL_HANDLE;
    return rightImagesVulkan_[index].image;
}

VkImage OpenXRSwapchainManager::GetHudSwapchainImageVulkan(uint32_t index) const {
    if (index >= hudImagesVulkan_.size()) return VK_NULL_HANDLE;
    return hudImagesVulkan_[index].image;
}

int64_t OpenXRSwapchainManager::SelectOptimalFormat(XrSession session, GraphicsBackend backendAPI, int64_t gameFormat) {
    uint32_t fmtCount = 0;
    if (XR_FAILED(xrEnumerateSwapchainFormats(session, 0, &fmtCount, nullptr)) || fmtCount == 0) {
        return (backendAPI == GraphicsBackend::Vulkan) ? 43 : 28;
    }

    std::vector<int64_t> fmts(fmtCount);
    xrEnumerateSwapchainFormats(session, fmtCount, &fmtCount, fmts.data());

    if (backendAPI == GraphicsBackend::Vulkan) {
        std::vector<int64_t> preferred;
        if (gameFormat == 44 || gameFormat == 50) {
            preferred = {44, 50, 37, 43}; // B8G8R8A8_UNORM, B8G8R8A8_SRGB, R8G8B8A8_UNORM, R8G8B8A8_SRGB
        } else {
            preferred = {37, 43, 44, 50}; // R8G8B8A8_UNORM, R8G8B8A8_SRGB, B8G8R8A8_UNORM, B8G8R8A8_SRGB
        }
        for (int64_t pref : preferred) {
            for (int64_t supported : fmts) {
                if (supported == pref) {
                    LOG_DEBUG("OpenXRSwapchainManager: Selected Vulkan swapchain format %lld", pref);
                    return pref;
                }
            }
        }
        return 43; // fallback: VK_FORMAT_R8G8B8A8_SRGB
    } else {
        std::vector<int64_t> familyPreference;
        if (gameFormat == 28 || gameFormat == 29) {
            familyPreference = {28, 29};
        } else if (gameFormat == 87 || gameFormat == 91) {
            familyPreference = {87, 91};
        } else if (gameFormat == 24) {
            familyPreference = {24};
        } else if (gameFormat == 10) {
            familyPreference = {10};
        } else {
            familyPreference = {gameFormat, 28, 87, 24, 29, 91, 10};
        }

        for (int64_t pref : familyPreference) {
            for (int64_t supported : fmts) {
                if (supported == pref) {
                    LOG_DEBUG("OpenXRSwapchainManager: Selected DXGI swapchain format %lld matching game family", pref);
                    return pref;
                }
            }
        }

        const int64_t dxgiPreferred[] = {28, 87, 24, 29, 91, 10, 2};
        for (int64_t pref : dxgiPreferred) {
            for (int64_t supported : fmts) {
                if (supported == pref) {
                    LOG_DEBUG("OpenXRSwapchainManager: Selected DXGI fallback format %lld", pref);
                    return pref;
                }
            }
        }
        return 28; // default: DXGI_FORMAT_R8G8B8A8_UNORM
    }
}

} // namespace openxr
} // namespace vrinject
