#include <gtest/gtest.h>
#include <cmath>
#include "rendering/stereo/stereo_camera_generator.h"
#include "rendering/stereo/stereo_frame_builder.h"
#include "hooks/input_manager.h"
#include "openxr/openxr_frame_submitter.h"

using namespace vrinject;

TEST(StereoCameraGenerator, TranslationCorrectness) {
    CameraSnapshot cam;
    cam.view = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    cam.projection = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 1,
        0, 0, -0.1f, 0
    };
    cam.position = { 0, 0, 0 };

    StereoConstants constants;
    constants.ipd = 0.063f;
    constants.convergence = 1.0f;

    EyeView left, right;
    RenderFrameSnapshot dummyFrame;
    dummyFrame.leftPose.orientation.w = 1.0f;
    dummyFrame.rightPose.orientation.w = 1.0f;
    // OpenXR pose gives world space translation from head.
    dummyFrame.leftPose.position.x = -0.0315f;
    dummyFrame.rightPose.position.x = 0.0315f;
    StereoCameraGenerator::Generate(dummyFrame, cam, constants, left, right);

    // Left eye translates right in view space, so m[3][0] increases
    EXPECT_FLOAT_EQ(left.view.m[3][0], 0.0315f);
    EXPECT_FLOAT_EQ(right.view.m[3][0], -0.0315f);

    // Left eye translates left in world space
    EXPECT_FLOAT_EQ(left.eyePosition.x, -0.0315f);
    EXPECT_FLOAT_EQ(right.eyePosition.x, 0.0315f);
}

TEST(StereoCameraGenerator, ProjectionShift) {
    CameraSnapshot cam;
    cam.view = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    cam.projection = {
        2.0f, 0, 0, 0, // xScale = 2.0
        0, 2.0f, 0, 0,
        0, 0, 1, 1,
        0, 0, -0.1f, 0
    };
    cam.position = { 0, 0, 0 };

    StereoConstants constants;
    constants.ipd = 0.063f;
    constants.convergence = 1.0f;

    EyeView left, right;
    RenderFrameSnapshot dummyFrame;
    dummyFrame.leftPose.orientation.w = 1.0f;
    dummyFrame.rightPose.orientation.w = 1.0f;
    
    // We want (tanRight + tanLeft) / (tanRight - tanLeft) = 0.063f
    // If tanRight = 1.063 and tanLeft = -0.937, sum = 0.126, diff = 2.0. 0.126 / 2.0 = 0.063
    dummyFrame.leftFov.angleRight = atan(1.063f);
    dummyFrame.leftFov.angleLeft = atan(-0.937f);
    dummyFrame.leftFov.angleUp = atan(1.0f);
    dummyFrame.leftFov.angleDown = atan(-1.0f);

    dummyFrame.rightFov.angleRight = atan(0.937f);
    dummyFrame.rightFov.angleLeft = atan(-1.063f);
    dummyFrame.rightFov.angleUp = atan(1.0f);
    dummyFrame.rightFov.angleDown = atan(-1.0f);

    StereoCameraGenerator::Generate(dummyFrame, cam, constants, left, right);

    // Shift magnitude = (halfIpd * xScale) / convergence
    // = (0.0315 * 2.0) / 1.0 = 0.063
    EXPECT_FLOAT_EQ(left.projection.m[2][0], 0.063f);
    EXPECT_FLOAT_EQ(right.projection.m[2][0], -0.063f);
}

TEST(StereoFrameBuilder, InverseMath) {
    CameraSnapshot cam;
    cam.view = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    // Identity for simplicity
    cam.vp = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };

    DepthSnapshot depth;
    EyeView left, right;
    StereoConstants constants;

    StereoFrameContext ctx = StereoFrameBuilder::Build(1, cam, depth, left, right, constants, 1920, 1080);
    
    // Inverse of identity is identity
    EXPECT_FLOAT_EQ(ctx.inverseViewProjection.m[0][0], 1.0f);
    EXPECT_FLOAT_EQ(ctx.inverseViewProjection.m[3][3], 1.0f);
}

TEST(StereoCameraGenerator, Quest3AsymmetricFov_OpticalCenterFusion_LH_And_RH) {
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    RenderFrameSnapshot frame;
    frame.leftPose.orientation = {0, 0, 0, 1};
    frame.rightPose.orientation = {0, 0, 0, 1};
    frame.leftPose.position = {-0.032f, 0.0f, 0.0f};
    frame.rightPose.position = {0.032f, 0.0f, 0.0f};

    // Typical Meta Quest 3 / Virtual Desktop asymmetric canted FOV
    frame.leftFov  = { -52.0f * kDegToRad,  40.0f * kDegToRad,  48.0f * kDegToRad, -48.0f * kDegToRad };
    frame.rightFov = { -40.0f * kDegToRad,  52.0f * kDegToRad,  48.0f * kDegToRad, -48.0f * kDegToRad };

    StereoConstants constants;
    constants.ipd = 0.064f;
    constants.nearPlane = 0.1f;
    constants.farPlane = 1000.0f;

    auto projectStraightAheadNdcX = [](const Matrix4x4& proj, bool isLH) -> float {
        float z = isLH ? 100.0f : -100.0f;
        float clipX = z * proj.m[2][0];
        float clipW = z * proj.m[2][3];
        return clipX / clipW;
    };

    for (bool isLH : { true, false }) {
        CameraSnapshot cam;
        cam.isLeftHanded = isLH;
        cam.reversedZ = true;
        cam.view = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

        EyeView left, right;
        StereoCameraGenerator::Generate(frame, cam, constants, left, right);

        float leftNdcX = projectStraightAheadNdcX(left.projection, isLH);
        float rightNdcX = projectStraightAheadNdcX(right.projection, isLH);

        // Straight ahead (0 deg) must map nasally rightward in Left Eye (> 0) and
        // symmetrically leftward in Right Eye (< 0) in BOTH LH and RH conventions!
        EXPECT_GT(leftNdcX, 0.15f);
        EXPECT_LT(rightNdcX, -0.15f);
        EXPECT_NEAR(leftNdcX, -rightNdcX, 1e-4f);

        // Verify shader EyeUvToTanRay maps each eye's optical-center UV to exact (0,0) ray
        float leftCenterU = (0.0f - left.fovTan[0]) / (left.fovTan[1] - left.fovTan[0]);
        float rightCenterU = (0.0f - right.fovTan[0]) / (right.fovTan[1] - right.fovTan[0]);
        float leftRayAtCenter = left.fovTan[0] + (left.fovTan[1] - left.fovTan[0]) * leftCenterU;
        float rightRayAtCenter = right.fovTan[0] + (right.fovTan[1] - right.fovTan[0]) * rightCenterU;
        EXPECT_NEAR(leftRayAtCenter, 0.0f, 1e-5f);
        EXPECT_NEAR(rightRayAtCenter, 0.0f, 1e-5f);
        EXPECT_NEAR(leftCenterU, 0.5f * leftNdcX + 0.5f, 1e-4f);
        EXPECT_NEAR(rightCenterU, 0.5f * rightNdcX + 0.5f, 1e-4f);
    }
}

TEST(StereoCameraGenerator, StageSpaceRoomPose_StrippedToHeadLocalIpd) {
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    // Standing Quest player at (0.45, 1.68, -0.32) rotated 60 deg yaw in STAGE space
    float halfYaw = 0.5f * (60.0f * kDegToRad);
    XrQuaternionf roomYaw = { 0.0f, std::sin(halfYaw), 0.0f, std::cos(halfYaw) };
    // Local +X rotated by +60 deg around +Y = (cos 60, 0, -sin 60)
    float rightX = std::cos(60.0f * kDegToRad);
    float rightZ = -std::sin(60.0f * kDegToRad);
    float halfIpd = 0.032f;

    RenderFrameSnapshot frame;
    frame.leftPose.orientation = roomYaw;
    frame.rightPose.orientation = roomYaw;
    frame.leftPose.position  = { 0.45f - rightX * halfIpd, 1.68f, -0.32f - rightZ * halfIpd };
    frame.rightPose.position = { 0.45f + rightX * halfIpd, 1.68f, -0.32f + rightZ * halfIpd };

    CameraSnapshot cam;
    cam.isLeftHanded = true;
    cam.view = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

    StereoConstants constants;
    constants.ipd = 0.072f; // User-configured IPD override must take effect

    EyeView left, right;
    StereoCameraGenerator::Generate(frame, cam, constants, left, right);

    EXPECT_NEAR(left.eyePosition.x, -0.036f, 1e-4f);
    EXPECT_NEAR(left.eyePosition.y,  0.0f,   1e-4f);
    EXPECT_NEAR(left.eyePosition.z,  0.0f,   1e-4f);
    EXPECT_NEAR(right.eyePosition.x, 0.036f, 1e-4f);
    EXPECT_NEAR(right.eyePosition.y, 0.0f,   1e-4f);
    EXPECT_NEAR(right.eyePosition.z, 0.0f,   1e-4f);
}

TEST(HeadTracking, QuaternionYawPitchDelta_And_WrapAround) {
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    auto quatYawRight = [](float rightDeg) -> XrQuaternionf {
        // Right turn in OpenXR (+X right, +Y up, -Z fwd) is negative rotation around +Y
        float half = -0.5f * rightDeg * kDegToRad;
        return { 0.0f, std::sin(half), 0.0f, std::cos(half) };
    };
    auto quatPitchUp = [](float upDeg) -> XrQuaternionf {
        // Pitch up in OpenXR is positive rotation around +X
        float half = 0.5f * upDeg * kDegToRad;
        return { std::sin(half), 0.0f, 0.0f, std::cos(half) };
    };

    float dYaw = 0.0f, dPitch = 0.0f;
    InputManager::ComputeHeadDeltaDeg(quatYawRight(0.0f), quatYawRight(15.0f), dYaw, dPitch);
    EXPECT_NEAR(dYaw, 15.0f, 1e-3f);
    EXPECT_NEAR(dPitch, 0.0f, 1e-3f);

    InputManager::ComputeHeadDeltaDeg(quatPitchUp(0.0f), quatPitchUp(10.0f), dYaw, dPitch);
    EXPECT_NEAR(dYaw, 0.0f, 1e-3f);
    EXPECT_NEAR(dPitch, 10.0f, 1e-3f);

    // Cross +/-180 deg boundary turning right (+178 -> -179 should be +3 deg, never -357 deg)
    InputManager::ComputeHeadDeltaDeg(quatYawRight(178.0f), quatYawRight(-179.0f), dYaw, dPitch);
    EXPECT_NEAR(dYaw, 3.0f, 1e-3f);

    // Single-frame tracking glitch > 35 deg must be suppressed cleanly
    InputManager::ComputeHeadDeltaDeg(quatYawRight(0.0f), quatYawRight(90.0f), dYaw, dPitch);
    EXPECT_FLOAT_EQ(dYaw, 0.0f);
    EXPECT_FLOAT_EQ(dPitch, 0.0f);
}

TEST(OpenXRFrameSubmitter, AnchoredHudQuad_PlacesAtEyeHeightInFrontOfGaze) {
    openxr::OpenXRFrameSubmitter submitter(nullptr);
    XrPosef standingPose = { {0.0f, 0.0f, 0.0f, 1.0f}, {0.5f, 1.65f, 0.2f} };
    XrSwapchain fakeSwapchain = reinterpret_cast<XrSwapchain>(static_cast<uintptr_t>(0x1234));

    submitter.SetHudQuadAnchored(fakeSwapchain, 1280, 720, standingPose, true, 1.45f, 1.35f, 0.76f);
    const auto& cfg = submitter.GetHudQuadConfig();
    EXPECT_TRUE(cfg.enabled);
    EXPECT_NEAR(cfg.pose.position.x, 0.5f, 1e-4f);
    EXPECT_NEAR(cfg.pose.position.y, 1.65f, 1e-4f);
    EXPECT_NEAR(cfg.pose.position.z, 0.2f - 1.45f, 1e-4f);
}
