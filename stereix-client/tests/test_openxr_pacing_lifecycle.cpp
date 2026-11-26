#include <gtest/gtest.h>
#include "openxr/openxr_frame_submitter.h"
#include "openxr/openxr_health_monitor.h"
#include <chrono>

using namespace vrinject::openxr;

TEST(OpenXRPacingLifecycleTest, BeginShutdownAndDestructionDoNotHang) {
    // HIGH-09: Ensures OpenXRFrameSubmitter tears down cleanly without blocking or deadlocking
    auto start = std::chrono::steady_clock::now();
    {
        OpenXRHealthMonitor health;
        OpenXRFrameSubmitter submitter(&health);
        EXPECT_EQ(submitter.GetState(), SubmitterState::WAIT_FRAME);
        submitter.BeginShutdown();
        submitter.BeginShutdown(); // Idempotent
    }
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    EXPECT_LT(elapsedMs, 2000);
}

TEST(OpenXRPacingLifecycleTest, BeginAndAcquireRejectsNullSessionAndNullSwapchainCleanly) {
    // HIGH-09: BeginAndAcquireDX11/DX12/Vulkan must fail closed on null session/swapchain and initialize pose outputs
    OpenXRHealthMonitor health;
    OpenXRFrameSubmitter submitter(&health);

    XrPosef leftPose{};
    XrPosef rightPose{};
    XrFovf leftFov{};
    XrFovf rightFov{};

    EXPECT_FALSE(submitter.BeginAndAcquireDX11(
        XR_NULL_HANDLE, XR_NULL_HANDLE, nullptr,
        leftPose, leftFov, rightPose, rightFov));
    EXPECT_FLOAT_EQ(leftPose.orientation.w, 1.0f);
    EXPECT_FLOAT_EQ(rightPose.orientation.w, 1.0f);

    ID3D12Resource* dx12Left = reinterpret_cast<ID3D12Resource*>(0x1);
    ID3D12Resource* dx12Right = reinterpret_cast<ID3D12Resource*>(0x1);
    EXPECT_FALSE(submitter.BeginAndAcquireDX12(
        XR_NULL_HANDLE, XR_NULL_HANDLE, nullptr,
        dx12Left, dx12Right,
        leftPose, leftFov, rightPose, rightFov));
    EXPECT_EQ(dx12Left, nullptr);
    EXPECT_EQ(dx12Right, nullptr);

    VkImage vkLeft = reinterpret_cast<VkImage>(0x1);
    VkImage vkRight = reinterpret_cast<VkImage>(0x1);
    EXPECT_FALSE(submitter.BeginAndAcquireVulkan(
        XR_NULL_HANDLE, XR_NULL_HANDLE, nullptr,
        vkLeft, vkRight,
        leftPose, leftFov, rightPose, rightFov));
    EXPECT_EQ(vkLeft, VK_NULL_HANDLE);
    EXPECT_EQ(vkRight, VK_NULL_HANDLE);

    submitter.BeginShutdown();
}

TEST(OpenXRPacingLifecycleTest, HudAndCinemaQuadConfigurationRoundTrip) {
    OpenXRHealthMonitor health;
    OpenXRFrameSubmitter submitter(&health);

    XrSwapchain fakeHud = reinterpret_cast<XrSwapchain>(0xABCD);
    submitter.SetHudQuad(fakeHud, 1280, 720, 1.2f, 1.4f, 0.8f);
    EXPECT_TRUE(submitter.GetHudQuadConfig().enabled);
    EXPECT_EQ(submitter.GetHudQuadConfig().swapchain, fakeHud);
    EXPECT_EQ(submitter.GetHudQuadConfig().width, 1280u);
    EXPECT_EQ(submitter.GetHudQuadConfig().height, 720u);

    submitter.ClearHudQuad();
    EXPECT_FALSE(submitter.GetHudQuadConfig().enabled);
    EXPECT_EQ(submitter.GetHudQuadConfig().swapchain, XR_NULL_HANDLE);

    submitter.SetCinemaQuad(true, 3.0f, 1920.0f, 1080.0f);
    EXPECT_TRUE(submitter.GetCinemaQuadConfig().enabled);
    EXPECT_FLOAT_EQ(submitter.GetCinemaQuadConfig().distance, 3.0f);
}
