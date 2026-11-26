#include <gtest/gtest.h>
#include "hooks/dxgi_factory_hook.h"
#include "hooks/input_hook.h"
#include "rendering/dx12/dx12_stereo_resource_manager.h"

// Defined by dllmain.cpp in vrinject.dll; stubbed here for standalone test executable linkage
HMODULE g_hModule = nullptr;

using namespace vrinject;

TEST(Dx12ResizeAndQueueCaptureTest, ScopedCaptureSuppressionNestingAndReset) {
    // HIGH-12: Prevents DX12Hook::Initialize() dummy swapchain from poisoning g_capturedCommandQueue
    EXPECT_FALSE(DXGIFactoryHook::IsInternalInitSuppressed());
    {
        DXGIFactoryHook::ScopedCaptureSuppression outer;
        EXPECT_TRUE(DXGIFactoryHook::IsInternalInitSuppressed());
        {
            DXGIFactoryHook::ScopedCaptureSuppression inner;
            EXPECT_TRUE(DXGIFactoryHook::IsInternalInitSuppressed());
        }
        EXPECT_TRUE(DXGIFactoryHook::IsInternalInitSuppressed());
    }
    EXPECT_FALSE(DXGIFactoryHook::IsInternalInitSuppressed());

    DXGIFactoryHook::ClearCapturedCommandQueue();
    EXPECT_EQ(DXGIFactoryHook::GetCapturedCommandQueue().Get(), nullptr);
}

TEST(Dx12ResizeAndQueueCaptureTest, StereoResourceManagerRingBufferAndNullFenceHandleResize) {
    // HIGH-08: Ring buffer constants and null-safe HandleResize when fence manager is absent
    EXPECT_EQ(DX12StereoResourceManager::MAX_FRAMES_IN_FLIGHT, 3u);
    EXPECT_EQ(DX12StereoResourceManager::DESCRIPTORS_PER_FRAME, 5u);

    DX12StereoResourceManager mgr;
    EXPECT_FALSE(mgr.HandleResize(1920, 1080));
    EXPECT_FALSE(mgr.HandleResize(0, 0));
}

TEST(Dx12ResizeAndQueueCaptureTest, InputHookAllowlistsOnlyVerifiedVrMirrorProcesses) {
    // HIGH-04: Low-level keyboard/mouse forwarding must ONLY target verified VR mirror/compositor windows
    EXPECT_TRUE(InputHook::IsVerifiedVrMirrorProcessName(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\bin\\win64\\vrmonitor.exe"));
    EXPECT_TRUE(InputHook::IsVerifiedVrMirrorProcessName(L"C:\\Program Files\\SteamVR\\vrcompositor.exe"));
    EXPECT_TRUE(InputHook::IsVerifiedVrMirrorProcessName(L"C:\\Program Files\\Oculus\\Support\\oculus-diagnostics\\OculusMirror.exe"));
    EXPECT_TRUE(InputHook::IsVerifiedVrMirrorProcessName(L"C:\\Program Files\\Virtual Desktop Streamer\\VirtualDesktop.Streamer.exe"));
    EXPECT_TRUE(InputHook::IsVerifiedVrMirrorProcessName(L"vdstreamer.exe"));
    EXPECT_TRUE(InputHook::IsVerifiedVrMirrorProcessName(L"ALVR_Dashboard.exe"));

    // Must reject browsers, password managers, chat apps, editors, and spoofed filenames
    EXPECT_FALSE(InputHook::IsVerifiedVrMirrorProcessName(nullptr));
    EXPECT_FALSE(InputHook::IsVerifiedVrMirrorProcessName(L""));
    EXPECT_FALSE(InputHook::IsVerifiedVrMirrorProcessName(L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe"));
    EXPECT_FALSE(InputHook::IsVerifiedVrMirrorProcessName(L"C:\\Program Files\\KeePassXC\\KeePassXC.exe"));
    EXPECT_FALSE(InputHook::IsVerifiedVrMirrorProcessName(L"C:\\Windows\\System32\\notepad.exe"));
    EXPECT_FALSE(InputHook::IsVerifiedVrMirrorProcessName(L"C:\\Temp\\evil_vrmonitor.exe"));
    EXPECT_FALSE(InputHook::IsVerifiedVrMirrorProcessName(L"C:\\Temp\\vrmonitor.exe.exe"));
}
