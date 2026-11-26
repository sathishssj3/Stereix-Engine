#pragma once
#include "core/math_types.h"
#include "core/graphics_types.h"
#include <cstdint>

namespace vrinject {

struct CameraSnapshot
{
    uint64_t frame = 0;

    Matrix4x4 view = {};
    Matrix4x4 projection = {};
    Matrix4x4 vp = {};

    Vector3 position = {};
    Quaternion rotation = {0.0f, 0.0f, 0.0f, 1.0f};

    float confidence = 0.0f;
    bool reversedZ = false;
    bool isLeftHanded = false;
    bool valid = false;
    
    GraphicsResourceIdentity resourceIdentity = {};

    bool IsValid() const {
        return valid && confidence > 0.5f && resourceIdentity.IsValid();
    }
};

} // namespace vrinject
