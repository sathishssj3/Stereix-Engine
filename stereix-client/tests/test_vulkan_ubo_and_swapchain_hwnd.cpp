#include <gtest/gtest.h>
#include "rendering/stereo/stereo_renderer.h"
#include "rendering/dx12/dx12_stereo_resource_manager.h"
#include "rendering/vulkan/vulkan_lifecycle_manager.h"
#include "heuristics/render_frame_snapshot.h"
#include "core/subsystem_context.h"
#include "core/config_manager.h"
#include "core/logger.h"
#include "core/frame_coordinator.h"

using namespace vrinject;

namespace {

class NullTestLogger : public ILogger {
public:
    void Init(const std::string&) override {}
    void Log(Level, const char*, int, const char*, ...) override {}
    void Shutdown() override {}
};

class VulkanUboAndSwapchainHwndTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto config = std::make_shared<ConfigManager>();
        auto logger = std::make_shared<NullTestLogger>();
        SubsystemContext::Get().Initialize(logger, config);
    }

    void TearDown() override {
        vulkan::VulkanLifecycleManager::Get().SetOutputWindow(nullptr);
        SubsystemContext::Get().Shutdown();
    }
};

} // namespace

TEST_F(VulkanUboAndSwapchainHwndTest, StereoShaderConstantsMatchFull512ByteLayout) {
    // HIGH-07: Guards against 256-byte UBO truncation that strips BUG-32 FOV/IPD/convergence fields
    EXPECT_EQ(sizeof(StereoShaderConstants), 512u);
    EXPECT_EQ(sizeof(DX12StereoShaderConstants), 512u);
    EXPECT_GE(offsetof(StereoShaderConstants, leftEyeFovTan), 256u);
    EXPECT_GE(offsetof(StereoShaderConstants, stereoConvergence), 256u);
}

TEST_F(VulkanUboAndSwapchainHwndTest, VulkanLifecycleSnapshotCarriesNativeHwndIndependentlyOfSwapchain) {
    auto& mgr = vulkan::VulkanLifecycleManager::Get();
    VkInstance fakeInstance = reinterpret_cast<VkInstance>(0x1001);
    VkPhysicalDevice fakePhys = reinterpret_cast<VkPhysicalDevice>(0x1002);
    VkDevice fakeDevice = reinterpret_cast<VkDevice>(0x1003);
    VkSwapchainKHR fakeSwapchain = reinterpret_cast<VkSwapchainKHR>(0xDEAD0001ULL);
    VkQueue fakeQueue = reinterpret_cast<VkQueue>(0x1004);
    void* fakeHwnd = reinterpret_cast<void*>(0x00001234ULL);

    mgr.OnInstanceCreated(fakeInstance, VK_API_VERSION_1_2);
    mgr.OnDeviceCreated(fakeInstance, fakePhys, fakeDevice, nullptr);

    VkSwapchainCreateInfoKHR scInfo{};
    scInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    scInfo.imageExtent = {1920, 1080};
    scInfo.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    mgr.OnSwapchainCreated(fakeDevice, fakeSwapchain, &scInfo);
    mgr.SetOutputWindow(fakeHwnd);

    RenderFrameSnapshot snap = mgr.CreateSnapshot(fakeQueue);
    EXPECT_EQ(snap.backend, GraphicsBackend::Vulkan);
    EXPECT_EQ(snap.nativeSwapchain, static_cast<void*>(fakeSwapchain));
    EXPECT_EQ(snap.nativeHwnd, fakeHwnd);
    EXPECT_EQ(snap.width, 1920u);
    EXPECT_EQ(snap.height, 1080u);

    mgr.OnSwapchainDestroyed(fakeDevice, fakeSwapchain);
    mgr.OnDeviceDestroyed(fakeDevice);
    mgr.OnInstanceDestroyed(fakeInstance);
}

TEST_F(VulkanUboAndSwapchainHwndTest, FrameOneVulkanPresentDoesNotDereferenceSwapchainAsCom) {
    // CRIT-04: Previously OnPresentBegin blindly cast nativeSwapchain (a VkSwapchainKHR) to IDXGISwapChain*
    // and called sc->GetDesc(&desc), causing an immediate 0xC0000005 crash on Frame 1 in Vulkan games.
    RenderFrameSnapshot snap{};
    snap.backend = GraphicsBackend::Vulkan;
    snap.nativeSwapchain = reinterpret_cast<void*>(0xDEAD0001ULL); // Invalid COM vtable pointer
    snap.nativeHwnd = nullptr;
    snap.nativeDevice = nullptr;
    snap.nativeContext = nullptr;
    snap.width = 1920;
    snap.height = 1080;

    auto* coordinator = SubsystemContext::Get().GetFrameCoordinator();
    ASSERT_NE(coordinator, nullptr);

    EXPECT_NO_FATAL_FAILURE({
        coordinator->OnPresentBegin(snap);
        coordinator->OnPresentEnd();
    });
}
