#include "rendering/stereo/stereo_camera_generator.h"
#include <cmath>
#include <algorithm>

namespace vrinject {

// Helper to multiply two 4x4 matrices
static Matrix4x4 Multiply(const Matrix4x4& a, const Matrix4x4& b) {
    Matrix4x4 result;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            result.m[i][j] = a.m[i][0] * b.m[0][j] +
                             a.m[i][1] * b.m[1][j] +
                             a.m[i][2] * b.m[2][j] +
                             a.m[i][3] * b.m[3][j];
        }
    }
    return result;
}

// Rotate vector v by the conjugate (inverse) of unit quaternion q
static Vector3 RotateByConjugate(const XrQuaternionf& q, float vx, float vy, float vz) {
    float normSq = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (normSq < 1e-6f) {
        return { vx, vy, vz };
    }
    float invNorm = 1.0f / std::sqrt(normSq);
    float qx = -q.x * invNorm;
    float qy = -q.y * invNorm;
    float qz = -q.z * invNorm;
    float qw =  q.w * invNorm;

    // t = 2 * cross(q.xyz, v)
    float tx = 2.0f * (qy * vz - qz * vy);
    float ty = 2.0f * (qz * vx - qx * vz);
    float tz = 2.0f * (qx * vy - qy * vx);

    // v' = v + qw * t + cross(q.xyz, t)
    Vector3 out;
    out.x = vx + qw * tx + (qy * tz - qz * ty);
    out.y = vy + qw * ty + (qz * tx - qx * tz);
    out.z = vz + qw * tz + (qx * ty - qy * tx);
    return out;
}

void StereoCameraGenerator::Generate(const RenderFrameSnapshot& renderSnapshot,
                                     const CameraSnapshot& camera, 
                                     const StereoConstants& constants,
                                     EyeView& outLeftEye,
                                     EyeView& outRightEye) {
    bool isLeftHanded = camera.isLeftHanded;
    
    auto createEyeOffsetView = [isLeftHanded](const Vector3& localEyeOffset) -> Matrix4x4 {
        float tx = localEyeOffset.x;
        float ty = localEyeOffset.y;
        float tz = isLeftHanded ? -localEyeOffset.z : localEyeOffset.z;

        Matrix4x4 view = {};
        view.m[0][0] = 1.0f;
        view.m[1][1] = 1.0f;
        view.m[2][2] = 1.0f;
        view.m[3][0] = -tx;
        view.m[3][1] = -ty;
        view.m[3][2] = -tz;
        view.m[3][3] = 1.0f;
        return view;
    };

    // OpenXR defines FOV relative to straight-ahead center (angleLeft < 0, angleRight > 0).
    // Row-vector convention (clip = [x,y,z,1] * proj):
    //   LH (+Z fwd, m[2][3] = +1): ndc.x = (x/z)*m[0][0] + m[2][0] -> m[2][0] = -(R+L)/(R-L)
    //   RH (-Z fwd, m[2][3] = -1): ndc.x = (x/-z)*m[0][0] - m[2][0] -> m[2][0] = +(R+L)/(R-L)
    auto createProj = [isLeftHanded](const XrFovf& fov, bool reversedZ, float zn, float zf, float outFovTan[4]) -> Matrix4x4 {
        float tanLeft = tanf(fov.angleLeft);
        float tanRight = tanf(fov.angleRight);
        float tanDown = tanf(fov.angleDown);
        float tanUp = tanf(fov.angleUp);

        float tanWidth = tanRight - tanLeft;
        float tanHeight = tanUp - tanDown;
        if (tanWidth < 0.01f || tanHeight < 0.01f) {
            tanLeft = -1.0f;
            tanRight = 1.0f;
            tanDown = -0.5625f;
            tanUp = 0.5625f;
            tanWidth = 2.0f;
            tanHeight = 1.125f;
        }

        outFovTan[0] = tanLeft;
        outFovTan[1] = tanRight;
        outFovTan[2] = tanDown;
        outFovTan[3] = tanUp;
        
        float offsetSign = isLeftHanded ? -1.0f : 1.0f;
        Matrix4x4 proj = {};
        proj.m[0][0] = 2.0f / tanWidth;
        proj.m[1][1] = 2.0f / tanHeight;
        proj.m[2][0] = offsetSign * (tanRight + tanLeft) / tanWidth;
        proj.m[2][1] = offsetSign * (tanUp + tanDown) / tanHeight;
        proj.m[2][3] = isLeftHanded ? 1.0f : -1.0f;
        
        // Z mapping (DX style)
        if (reversedZ) {
            proj.m[2][2] = isLeftHanded ? -zn / (zf - zn) : zn / (zf - zn) - 1.0f;
            proj.m[3][2] = (zf * zn) / (zf - zn); 
        } else {
            proj.m[2][2] = isLeftHanded ? zf / (zf - zn) : zf / (zn - zf);
            proj.m[3][2] = isLeftHanded ? -(zf * zn) / (zf - zn) : (zf * zn) / (zn - zf);
        }
        return proj;
    };

    // Isolate ONLY the head-local inter-ocular eye offset from OpenXR STAGE/LOCAL space.
    // On real headsets (Quest / Index / Virtual Desktop), leftPose & rightPose include
    // standing room height (~1.65m) and arbitrary room yaw/position. Multiplying the raw
    // STAGE pose directly into camera.view shifts the reprojection camera off the game world.
    float midX = 0.5f * (renderSnapshot.leftPose.position.x + renderSnapshot.rightPose.position.x);
    float midY = 0.5f * (renderSnapshot.leftPose.position.y + renderSnapshot.rightPose.position.y);
    float midZ = 0.5f * (renderSnapshot.leftPose.position.z + renderSnapshot.rightPose.position.z);

    Vector3 leftLocal = RotateByConjugate(
        renderSnapshot.leftPose.orientation,
        renderSnapshot.leftPose.position.x - midX,
        renderSnapshot.leftPose.position.y - midY,
        renderSnapshot.leftPose.position.z - midZ);
    Vector3 rightLocal = RotateByConjugate(
        renderSnapshot.rightPose.orientation,
        renderSnapshot.rightPose.position.x - midX,
        renderSnapshot.rightPose.position.y - midY,
        renderSnapshot.rightPose.position.z - midZ);

    float dx = rightLocal.x - leftLocal.x;
    float dy = rightLocal.y - leftLocal.y;
    float dz = rightLocal.z - leftLocal.z;
    float measuredIpd = std::sqrt(dx * dx + dy * dy + dz * dz);
    float targetIpd = (constants.ipd > 0.01f && constants.ipd < 0.15f) ? constants.ipd : 0.064f;

    if (measuredIpd > 0.01f) {
        float ipdScale = targetIpd / measuredIpd;
        leftLocal.x *= ipdScale;  leftLocal.y *= ipdScale;  leftLocal.z *= ipdScale;
        rightLocal.x *= ipdScale; rightLocal.y *= ipdScale; rightLocal.z *= ipdScale;
    } else {
        leftLocal = { -targetIpd * 0.5f, 0.0f, 0.0f };
        rightLocal = { targetIpd * 0.5f, 0.0f, 0.0f };
    }

    Matrix4x4 leftPoseView = createEyeOffsetView(leftLocal);
    Matrix4x4 rightPoseView = createEyeOffsetView(rightLocal);

    bool hasValidGameView = (std::abs(camera.view.m[0][0]) > 0.0001f ||
                             std::abs(camera.view.m[1][1]) > 0.0001f ||
                             std::abs(camera.view.m[2][2]) > 0.0001f ||
                             std::abs(camera.view.m[3][3]) > 0.0001f);

    // Eye View = GameView * LocalEyeOffsetView
    if (hasValidGameView) {
        outLeftEye.view = Multiply(camera.view, leftPoseView);
        outRightEye.view = Multiply(camera.view, rightPoseView);
    } else {
        outLeftEye.view = leftPoseView;
        outRightEye.view = rightPoseView;
    }

    float nearP = constants.nearPlane > 0.001f ? constants.nearPlane : 0.1f;
    float farP = constants.farPlane > nearP ? constants.farPlane : 1000.0f;

    outLeftEye.projection = createProj(renderSnapshot.leftFov, camera.reversedZ, nearP, farP, outLeftEye.fovTan);
    outRightEye.projection = createProj(renderSnapshot.rightFov, camera.reversedZ, nearP, farP, outRightEye.fovTan);

    outLeftEye.viewProjection = Multiply(outLeftEye.view, outLeftEye.projection);
    outRightEye.viewProjection = Multiply(outRightEye.view, outRightEye.projection);

    // Extract world-space eye positions from updated view matrices
    auto extractPos = [](const Matrix4x4& v) -> Vector3 {
        Vector3 pos;
        pos.x = -(v.m[3][0] * v.m[0][0] + v.m[3][1] * v.m[0][1] + v.m[3][2] * v.m[0][2]);
        pos.y = -(v.m[3][0] * v.m[1][0] + v.m[3][1] * v.m[1][1] + v.m[3][2] * v.m[1][2]);
        pos.z = -(v.m[3][0] * v.m[2][0] + v.m[3][1] * v.m[2][1] + v.m[3][2] * v.m[2][2]);
        return pos;
    };

    outLeftEye.eyePosition = extractPos(outLeftEye.view);
    outRightEye.eyePosition = extractPos(outRightEye.view);
}

} // namespace vrinject
