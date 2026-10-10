#include "model2/CM2Lighting.hpp"

#include <cmath>
#include <cstring>

#include "base/Matrix.hpp"
#include "model2/M2Scene.hpp"
#include "util/Unimplemented.hpp"

namespace {

constexpr float DIFFUSE_MAX = 10.0f;    // DAT_142cfb700

}

// ref: FUN_1411ff570
CM2Lighting::CM2Lighting() {
    std::memset(&this->m_position, 0, sizeof(float) * 3);
    std::memset(&this->m_worldPosition, 0, sizeof(float) * 3);
    std::memset(&this->m_direction, 0, sizeof(float) * 3);
    std::memset(&this->m_ambient, 0, sizeof(this->m_ambient));
    std::memset(&this->m_diffuse, 0, sizeof(this->m_diffuse));
    std::memset(this->m_pad64, 0, sizeof(this->m_pad64));
    this->m_scene = nullptr;
    this->m_08 = 0;
    this->m_range = 0.0f;
    this->m_enabled = 0;
    this->m_link = nullptr;
    this->m_next = nullptr;
    this->m_flags = 0;
    this->m_type = LIGHT_POINT;
    this->m_78 = 0.5f;
    this->m_7c = 1.0f;
}

CM2Lighting::~CM2Lighting() {
    this->Unlink();
}

// ref: FUN_1411fffe0
void CM2Lighting::SetScene(CM2Scene* scene) {
    this->m_scene = scene;
    if (scene != nullptr) {
        this->m_08 = scene->m_frame - 1;
        return;
    }
    this->m_08 = 0;
}

// ref: FUN_141200300
void CM2Lighting::SetEnabled(int32_t enabled) {
    if ((this->m_enabled == 0) == (enabled == 0)) {
        return;
    }
    this->m_enabled = enabled;
    if (this->m_scene == nullptr) {
        return;
    }
    if (enabled == 0) {
        this->Unlink();
        return;
    }
    this->Link();
}

// ref: FUN_141200210
void CM2Lighting::SetLightType(int32_t type) {
    if (this->m_type == type) {
        return;
    }
    this->m_type = type;
    if (this->m_enabled != 0 && this->m_scene != nullptr) {
        this->Unlink();
        this->Link();
    }
}

// ref: FUN_141200250
void CM2Lighting::SetPosition(const float* position) {
    this->m_position[0] = position[0];
    this->m_position[1] = position[1];
    this->m_position[2] = position[2];
    if (this->m_scene != nullptr) {
        // the position through the scene's view matrix (scene +0xd478) into m_worldPosition
        FROST_UNIMPLEMENTED();
    }
    if (this->m_enabled != 0 && this->m_scene != nullptr && this->m_type == LIGHT_POINT) {
        this->Unlink();
        this->Link();
    }
}

// ref: FUN_1412001f0
void CM2Lighting::SetDirection(const float* direction) {
    this->m_direction[0] = direction[0];
    this->m_direction[1] = direction[1];
    this->m_direction[2] = direction[2];
    VectorNormalize(this->m_direction);
}

// ref: FUN_1411ffcd0
float* CM2Lighting::GetDiffuse(float* out) const {
    for (int32_t i = 0; i < 3; i++) {
        float value = this->m_diffuse[i];
        out[i] = DIFFUSE_MAX <= value ? DIFFUSE_MAX : value;
    }
    return out;
}

// ref: FUN_141200500
void CM2Lighting::Unlink() {
    if (this->m_link == nullptr && this->m_next == nullptr) {
        return;
    }
    // under the scene's light lock (scene +0xd0)
    if (this->m_scene != nullptr) {
        FROST_UNIMPLEMENTED();
    }
    if (this->m_link != nullptr) {
        *this->m_link = this->m_next;
    }
    if (this->m_next != nullptr) {
        this->m_next->m_link = this->m_link;
    }
    this->m_link = nullptr;
    this->m_next = nullptr;
}

// ref: FUN_141200090
void CM2Lighting::Link() {
    if (this->m_enabled == 0 || this->m_scene == nullptr) {
        return;
    }
    // the scene's point (+0xc0) or directional (+0xb8) list, under its light lock
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141200000
void M2ModelLightingBegin(CM2ModelLighting* lighting, CM2Scene* scene, const float* position) {
    std::memset(lighting, 0, sizeof(CM2ModelLighting));
    lighting->m_flags = 0x20;
    if (scene != nullptr && (scene->m_flags & 0x200) != 0) {
        lighting->m_flags = 0x120;
    }
    lighting->m_scene = scene;
    std::memcpy(lighting->m_position, position, sizeof(lighting->m_position));
    if (scene == nullptr) {
        lighting->m_maxLights = 8;
    } else {
        lighting->m_maxLights = scene->m_maxLights;
    }
}

// ref: FUN_1411ff680
void M2ModelLightingAddDirectional(CM2ModelLighting* lighting, const float* color, const float* direction) {
    float d[3];
    if (lighting->m_scene == nullptr) {
        d[0] = direction[0];
        d[1] = direction[1];
        d[2] = direction[2];
    } else {
        const float* m = lighting->m_scene->m_camera.m;
        d[0] = direction[1] * m[4] + direction[0] * m[0] + direction[2] * m[8];
        d[1] = direction[1] * m[5] + direction[0] * m[1] + direction[2] * m[9];
        d[2] = direction[1] * m[6] + direction[0] * m[2] + direction[2] * m[10];
    }
    for (int32_t axis = 0; axis < 3; axis++) {
        lighting->m_axes[axis][1] = d[1] * color[axis] + lighting->m_axes[axis][1];
        lighting->m_axes[axis][0] = d[0] * color[axis] + lighting->m_axes[axis][0];
        lighting->m_axes[axis][2] = d[2] * color[axis] + lighting->m_axes[axis][2];
    }
    lighting->m_direction[1] = d[1] * color[2] + lighting->m_direction[1];
    lighting->m_direction[0] = color[2] * d[0] + lighting->m_direction[0];
    lighting->m_direction[2] = d[2] * color[2] + lighting->m_direction[2];
    lighting->m_directionColor[0] += color[0];
    lighting->m_directionColor[1] += color[1];
    lighting->m_directionColor[2] += color[2];
    std::memcpy(lighting->m_lightDirection, direction, 12);
    std::memcpy(lighting->m_directional, color, 12);
}

// ref: FUN_1411ff860
void M2ModelLightingAdd(CM2ModelLighting* lighting, CM2Lighting* light) {
    if (light->m_enabled == 0) {
        return;
    }
    if (light->m_type == CM2Lighting::LIGHT_POINT) {
        float dy = light->m_position[1] - lighting->m_position[1];
        float dx = light->m_position[0] - lighting->m_position[0];
        float dz = light->m_position[2] - lighting->m_position[2];
        float distance = dy * dy + dx * dx + dz * dz;
        uint32_t count = lighting->m_pointCount;
        uint32_t max = lighting->m_maxLights;
        if (count < max || distance < lighting->m_pointDistances[max - 1]) {
            uint32_t slot = count < max ? count : count - 1;
            while (slot != 0) {
                uint32_t above = slot - 1;
                if (lighting->m_pointDistances[above] < distance) {
                    break;
                }
                lighting->m_points[slot] = lighting->m_points[above];
                lighting->m_pointDistances[slot] = lighting->m_pointDistances[above];
                slot = above;
            }
            lighting->m_points[slot] = light;
            lighting->m_pointDistances[slot] = distance;
            if (lighting->m_pointCount < lighting->m_maxLights) {
                lighting->m_pointCount++;
            }
        }
        return;
    }
    for (int32_t i = 0; i < 3; i++) {
        lighting->m_ambient[i] += light->m_ambient[0][i];
        lighting->m_064[i] += light->m_ambient[1][i];
        lighting->m_070[i] += light->m_ambient[2][i];
    }
    M2ModelLightingAddDirectional(lighting, light->m_diffuse, light->m_direction);
    lighting->m_088[0] += reinterpret_cast<const float*>(light->m_pad64)[0];
    lighting->m_088[1] += reinterpret_cast<const float*>(light->m_pad64)[1];
    lighting->m_088[2] += reinterpret_cast<const float*>(light->m_pad64)[2];
}

// ref: FUN_141200330
void M2ModelLightingFinish(CM2ModelLighting* lighting, void* param) {
    (void)param;
    if ((lighting->m_flags & 2) != 0) {
        return;
    }
    float* dir = lighting->m_lightDirection;
    dir[0] = lighting->m_direction[0];
    dir[1] = lighting->m_direction[1];
    dir[2] = lighting->m_direction[2];
    if (dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2] <= 1.0e-05f) {  // DAT_1426c75c4
        dir[0] = 0.0f;
        dir[1] = 0.0f;
        dir[2] = -1.0f;
    } else {
        VectorNormalize(dir);
    }
    // DAT_1426c8874, DAT_1426193c4
    const float full = 1.25f;
    const float quarter = 0.25f;
    float x = dir[0];
    float y = dir[1];
    float z = dir[2];
    float a = y * lighting->m_axes[0][1] + x * lighting->m_axes[0][0] + z * lighting->m_axes[0][2];
    float b = y * lighting->m_axes[1][1] + x * lighting->m_axes[1][0] + z * lighting->m_axes[1][2];
    float c = y * lighting->m_axes[2][1] + x * lighting->m_axes[2][0] + z * lighting->m_axes[2][2];
    float cy = lighting->m_directionColor[1] * quarter;
    float cz = lighting->m_directionColor[2] * quarter;
    float ax = (lighting->m_directionColor[0] - a) * quarter;
    lighting->m_directional[0] = a * full - lighting->m_directionColor[0] * quarter;
    lighting->m_directional[1] = b * full - cy;
    lighting->m_directional[2] = c * full - cz;
    lighting->m_flags |= 2;
    lighting->m_ambient[0] = ax + lighting->m_ambient[0];
    lighting->m_ambient[1] = (lighting->m_directionColor[1] - b) * quarter + lighting->m_ambient[1];
    lighting->m_ambient[2] = (lighting->m_directionColor[2] - c) * quarter + lighting->m_ambient[2];
}

// ref: FUN_1411ffb70
void M2ModelLightingEnd(CM2ModelLighting* lighting) {
    if ((lighting->m_flags & 1) != 0) {
        return;
    }
    if (lighting->m_scene != nullptr && (lighting->m_flags & 0x40) != 0) {
        PlaneTransform(lighting->m_plane, lighting->m_scene->m_camera.m);
    }
    lighting->m_flags |= 1;
}

// ref: FUN_14134ba00
void PlaneTransform(float* plane, const float* m) {
    float x = plane[0];
    float y = plane[1];
    float z = plane[2];
    float d = -plane[3];
    float px = x * d;
    float py = y * d;
    float pz = z * d;
    float nx = y * m[4] + x * m[0] + z * m[8];
    float ny = y * m[5] + x * m[1] + z * m[9];
    float nz = y * m[6] + x * m[2] + z * m[10];
    plane[0] = nx;
    plane[1] = ny;
    plane[2] = nz;
    float lengthSq = ny * ny + nx * nx + nz * nz;
    if (2.3841858e-07f < lengthSq) {  // DAT_14261a954
        float scale = 1.0f / std::sqrt(lengthSq);
        nx = nx * scale;
        ny = ny * scale;
        nz = nz * scale;
        plane[0] = nx;
        plane[1] = ny;
        plane[2] = nz;
    }
    plane[3] = -(nx * (px * m[0] + py * m[4] + pz * m[8] + m[12]) + (px * m[1] + py * m[5] + pz * m[9] + m[13]) * ny
        + (px * m[2] + py * m[6] + pz * m[10] + m[14]) * nz);
}
