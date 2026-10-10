#include "model2/CM2Lighting.hpp"

#include <cstring>

#include "base/Matrix.hpp"
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
    this->m_70 = 0;
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
