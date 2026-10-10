#ifndef MODEL2_M2_CAMERA_HPP
#define MODEL2_M2_CAMERA_HPP

// The camera a model carries (Engine/Source/Services/Camera.cpp, 0x98, reference counted):
// a position, a target and the direction between them, the clip distances, and four angles
// each kept with its sine and cosine. M2 models make one per M2Camera; the UI's model frames
// view their scene through one.

#include <cstdint>

class GxRenderContext;

// An angle wrapped to [0, 2 pi) with its cosine and sine (vtable 0x1427267e0, 0x18).
class CCameraAngle {
    public:
    float m_angle = -0.0f;          // 0x08
    float m_cos = 0.0f;             // 0x0c
    float m_sin = 0.0f;             // 0x10

    // ref: FUN_1412964c0
    virtual void Set(const float* angle);
};
static_assert(sizeof(CCameraAngle) == 0x18, "CCameraAngle is 0x18 bytes");

class CM2Camera {
    public:
    // The vectors (GetVector, SetVector).
    enum : int32_t {
        VECTOR_POSITION = 7,
        VECTOR_TARGET = 8,
        VECTOR_DIRECTION = 9,
        VECTOR_REVERSE = 10,   // the direction negated; setting it moves the position
    };
    // The floats (GetFloat, SetFloat).
    enum : int32_t {
        FLOAT_YAW = 0,
        FLOAT_28 = 1,
        FLOAT_FAR_CLIP = 2,
        FLOAT_NEAR_CLIP = 3,
        FLOAT_FIELD_OF_VIEW = 4,
        FLOAT_ROLL = 5,
        FLOAT_PITCH = 6,
    };

    int32_t m_refCount;             // 0x00
    float m_position[3];            // 0x04
    float m_target[3];              // 0x10
    float m_direction[3];           // 0x1c
    float m_28;                     // 0x28 100
    float m_farClip;                // 0x2c 5000
    float m_nearClip;               // 0x30 8
    uint32_t m_pad34;
    CCameraAngle m_yaw;             // 0x38
    CCameraAngle m_fieldOfView;     // 0x50
    CCameraAngle m_roll;            // 0x68
    CCameraAngle m_pitch;           // 0x80
};
static_assert(sizeof(CM2Camera) == 0x98, "CM2Camera is 0x98 bytes");

// ref: FUN_141295830
CM2Camera* M2CameraCreateDefault();
// ref: FUN_141295980
CM2Camera* M2CameraClone(CM2Camera* camera);
// ref: FUN_141295d20
void M2CameraGetVector(CM2Camera* camera, int32_t which, float* out);
// ref: FUN_141295da0
float M2CameraGetFloat(CM2Camera* camera, int32_t which);
// ref: FUN_141295ff0
void M2CameraRelease(CM2Camera** camera);
// ref: FUN_141296020
void M2CameraSetVector(CM2Camera* camera, int32_t which, const float* value);
// ref: FUN_141296160
void M2CameraSetFloat(CM2Camera* camera, int32_t which, float value);
// The engine's count helpers on the camera's first word (FUN_140317cb0, FUN_140318030).
void M2CameraAddRef(CM2Camera* camera);
int32_t M2CameraGetRefCount(CM2Camera* camera);
// The projection through the camera for a rectangle (left, bottom, right, top) and the view
// (FUN_1412963f0).
void M2CameraGetMatrices(CM2Camera* camera, const float* rect, float* view, float* projection);
// An orthographic pair for a rectangle and an origin (FUN_141296240).
void M2CameraGetOrthoMatrices(const float* rect, const float* origin, void* unused, float* view, float* projection);
// The projection into the vertex constants (FUN_1412aeb60).
void M2SetProjectionConstants(GxRenderContext* context, const float* projection);

#endif
