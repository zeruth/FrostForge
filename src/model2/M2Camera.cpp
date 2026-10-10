#include "model2/M2Camera.hpp"

#include <cmath>
#include <cstring>
#include <new>

#include "base/Matrix.hpp"
#include "gx/Gx.hpp"
#include "gx/GxRenderTargets.hpp"
#include "gx/d3d12/CommandList.hpp"
#include "storm/Memory.hpp"

// Engine/Source/Services/Camera.cpp.

namespace {

const char* const s_cameraFile = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Services\\Camera.cpp";

// DAT_142726830, DAT_1426c70ac, DAT_142722f80
constexpr float INV_TWO_PI = 0.15915494f;
constexpr float TWO_PI = 6.2831855f;
// DAT_142632530, DAT_1426c9758
constexpr float HALF_PI = 1.5707964f;
constexpr float PI = 3.1415927f;

void SetAngle(CCameraAngle* angle, float value) {
    angle->Set(&value);
}

CM2Camera* NewCamera(int32_t line) {
    void* memory = SMemAlloc(sizeof(CM2Camera), 1, s_cameraFile, 0, line, 0);
    auto camera = static_cast<CM2Camera*>(memory);
    camera->m_refCount = 1;
    camera->m_position[0] = 100.0f;
    camera->m_position[1] = 0.0f;
    camera->m_position[2] = 0.0f;
    camera->m_target[0] = 0.0f;
    camera->m_target[1] = 0.0f;
    camera->m_target[2] = 0.0f;
    camera->m_direction[0] = -100.0f;
    camera->m_direction[1] = 0.0f;
    camera->m_direction[2] = 0.0f;
    camera->m_28 = 100.0f;
    camera->m_farClip = 5000.0f;
    camera->m_nearClip = 8.0f;
    new (&camera->m_yaw) CCameraAngle();
    camera->m_yaw.m_angle = -0.0f;
    camera->m_yaw.m_cos = std::cos(-0.0f);
    camera->m_yaw.m_sin = std::sin(-0.0f);
    new (&camera->m_fieldOfView) CCameraAngle();
    camera->m_fieldOfView.m_angle = HALF_PI;
    camera->m_fieldOfView.m_cos = std::cos(HALF_PI);
    camera->m_fieldOfView.m_sin = std::sin(HALF_PI);
    new (&camera->m_roll) CCameraAngle();
    camera->m_roll.m_angle = -0.0f;
    camera->m_roll.m_cos = camera->m_yaw.m_cos;
    camera->m_roll.m_sin = camera->m_yaw.m_sin;
    new (&camera->m_pitch) CCameraAngle();
    camera->m_pitch.m_angle = -0.0f;
    camera->m_pitch.m_cos = camera->m_yaw.m_cos;
    camera->m_pitch.m_sin = camera->m_yaw.m_sin;
    return camera;
}

}  // namespace

// ref: FUN_1412964c0
void CCameraAngle::Set(const float* angle) {
    float value = *angle;
    float turns = static_cast<float>(static_cast<int32_t>(value * INV_TWO_PI)) * TWO_PI;
    if (value < 0.0f) {
        turns = turns + -TWO_PI;
    }
    this->m_angle = value - turns;
    this->m_cos = std::cos(value - turns);
    this->m_sin = std::sin(value - turns);
}

// ref: FUN_141295830
CM2Camera* M2CameraCreateDefault() {
    return NewCamera(0x6a);
}

// ref: FUN_141295980
CM2Camera* M2CameraClone(CM2Camera* source) {
    CM2Camera* camera = NewCamera(0x71);
    std::memcpy(camera->m_position, source->m_position, sizeof(camera->m_position));
    std::memcpy(camera->m_target, source->m_target, sizeof(camera->m_target));
    std::memcpy(camera->m_direction, source->m_direction, sizeof(camera->m_direction));
    camera->m_28 = source->m_28;
    camera->m_nearClip = source->m_nearClip;
    camera->m_farClip = source->m_farClip;
    SetAngle(&camera->m_yaw, source->m_yaw.m_angle);
    SetAngle(&camera->m_fieldOfView, source->m_fieldOfView.m_angle);
    SetAngle(&camera->m_roll, source->m_roll.m_angle);
    SetAngle(&camera->m_pitch, source->m_pitch.m_angle);
    return camera;
}

// ref: FUN_141295d20
void M2CameraGetVector(CM2Camera* camera, int32_t which, float* out) {
    if (which == CM2Camera::VECTOR_POSITION) {
        std::memcpy(out, camera->m_position, sizeof(float) * 3);
    } else if (which == CM2Camera::VECTOR_TARGET) {
        std::memcpy(out, camera->m_target, sizeof(float) * 3);
    } else if (which == CM2Camera::VECTOR_DIRECTION) {
        std::memcpy(out, camera->m_direction, sizeof(float) * 3);
    } else if (which == CM2Camera::VECTOR_REVERSE) {
        out[0] = -camera->m_direction[0];
        out[1] = -camera->m_direction[1];
        out[2] = -camera->m_direction[2];
    }
}

// ref: FUN_141295da0
float M2CameraGetFloat(CM2Camera* camera, int32_t which) {
    switch (which) {
    case CM2Camera::FLOAT_YAW:
        return camera->m_yaw.m_angle;
    case CM2Camera::FLOAT_28:
        return camera->m_28;
    case CM2Camera::FLOAT_FAR_CLIP:
        return camera->m_farClip;
    case CM2Camera::FLOAT_NEAR_CLIP:
        return camera->m_nearClip;
    case CM2Camera::FLOAT_FIELD_OF_VIEW:
        return camera->m_fieldOfView.m_angle;
    case CM2Camera::FLOAT_ROLL:
        return camera->m_roll.m_angle;
    case CM2Camera::FLOAT_PITCH:
        return camera->m_pitch.m_angle;
    default:
        return 0.0f;
    }
}

// ref: FUN_141295ff0
void M2CameraRelease(CM2Camera** camera) {
    CM2Camera* object = *camera;
    object->m_refCount = object->m_refCount - 1;
    if (object->m_refCount == 0) {
        SMemFree(object, s_cameraFile, 0, 0);
    }
    *camera = nullptr;
}

// ref: FUN_141296020
void M2CameraSetVector(CM2Camera* camera, int32_t which, const float* value) {
    if (which == CM2Camera::VECTOR_POSITION) {
        std::memcpy(camera->m_position, value, sizeof(float) * 3);
    } else if (which == CM2Camera::VECTOR_TARGET) {
        std::memcpy(camera->m_target, value, sizeof(float) * 3);
    } else {
        if (which == CM2Camera::VECTOR_DIRECTION) {
            std::memcpy(camera->m_direction, value, sizeof(float) * 3);
            camera->m_target[0] = camera->m_position[0] + value[0];
            camera->m_target[1] = value[1] + camera->m_position[1];
            camera->m_target[2] = value[2] + camera->m_position[2];
        } else if (which == CM2Camera::VECTOR_REVERSE) {
            std::memcpy(camera->m_direction, value, sizeof(float) * 3);
            camera->m_position[0] = camera->m_target[0] - value[0];
            camera->m_position[1] = camera->m_target[1] - value[1];
            camera->m_position[2] = camera->m_target[2] - value[2];
        }
        goto check;
    }
    camera->m_direction[0] = camera->m_target[0] - camera->m_position[0];
    camera->m_direction[1] = camera->m_target[1] - camera->m_position[1];
    camera->m_direction[2] = camera->m_target[2] - camera->m_position[2];
check:
    if (camera->m_direction[0] * camera->m_direction[0] + camera->m_direction[1] * camera->m_direction[1]
            + camera->m_direction[2] * camera->m_direction[2]
        < 0.005f) {  // DAT_14261a960
        camera->m_direction[0] = 0.0f;
        camera->m_direction[1] = 0.0f;
        camera->m_direction[2] = 1.0f;
    }
}

// ref: FUN_141296160
void M2CameraSetFloat(CM2Camera* camera, int32_t which, float value) {
    switch (which) {
    case CM2Camera::FLOAT_YAW:
        camera->m_yaw.Set(&value);
        return;
    case CM2Camera::FLOAT_28:
        camera->m_28 = value;
        return;
    case CM2Camera::FLOAT_FAR_CLIP:
        camera->m_farClip = value;
        return;
    case CM2Camera::FLOAT_NEAR_CLIP:
        camera->m_nearClip = value;
        return;
    case CM2Camera::FLOAT_FIELD_OF_VIEW:
        if (value <= 0.0f || PI <= value) {
            value = HALF_PI;
        }
        camera->m_fieldOfView.Set(&value);
        return;
    case CM2Camera::FLOAT_ROLL:
        camera->m_roll.Set(&value);
        return;
    case CM2Camera::FLOAT_PITCH:
        camera->m_pitch.Set(&value);
        return;
    default:
        return;
    }
}

// ref: FUN_140317cb0
void M2CameraAddRef(CM2Camera* camera) {
    camera->m_refCount = camera->m_refCount + 1;
}

// ref: FUN_140318030
int32_t M2CameraGetRefCount(CM2Camera* camera) {
    return camera->m_refCount;
}

// ref: FUN_141296240
void M2CameraGetOrthoMatrices(const float* rect, const float* origin, void* unused, float* view, float* projection) {
    (void)unused;
    float halfWidth = (rect[2] + rect[0]) * 0.5f;   // DAT_14261a988
    float halfHeight = (rect[3] + rect[1]) * 0.5f;
    MatrixIdentity(projection);
    // DAT_142726834, DAT_14261a9bc
    Orthographic(rect[1] + -halfHeight, rect[3] + -halfHeight, rect[0] + -halfWidth, rect[2] + -halfWidth, -500.0f,
        500.0f, projection);
    float flip[3] = {1.0f, 1.0f, -1.0f};
    MatrixScale(projection, flip);
    MatrixIdentity(view);
    float move[3] = {origin[0] - halfHeight, origin[1] - halfWidth, 0.0f};
    MatrixTranslate(view, move);
}

// ref: FUN_1412963f0
void M2CameraGetMatrices(CM2Camera* camera, const float* rect, float* view, float* projection) {
    MatrixPerspective(camera->m_fieldOfView.m_angle, (rect[3] - rect[1]) / (rect[2] - rect[0]), camera->m_nearClip,
        camera->m_farClip, projection);
    float eye[3] = {0.0f, 0.0f, 0.0f};
    float up[3] = {
        camera->m_roll.m_sin * camera->m_pitch.m_sin,
        -(camera->m_roll.m_sin * camera->m_pitch.m_cos),
        camera->m_roll.m_cos,
    };
    MatrixLookAt(eye, camera->m_direction, up, view);
    M2SetProjectionConstants(g_gxRenderContext, projection);
}

// The projection, transposed, into the vertex constants at slot 4 while a frame is open.
// ref: FUN_1412aeb60
void M2SetProjectionConstants(GxRenderContext* context, const float* projection) {
    if (!GxFrameBegan()) {
        return;
    }
    float* constants = context->LockConstants(0, 4, 0x40);
    for (int32_t row = 0; row < 4; row++) {
        for (int32_t column = 0; column < 4; column++) {
            constants[row * 4 + column] = projection[column * 4 + row];
        }
    }
    context->UnlockConstants(0, 4);
}
