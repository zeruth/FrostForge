#include "world/WorldClip.hpp"

namespace {

// DAT_142d00c30..DAT_142d00c40, with their static data.
float s_c30 = 200.0f;
float s_farClip = 1.0f;
float s_nearClip = 0.1f;
float s_farClip3 = 1.0f;
float s_farClip2 = 1.0f;

constexpr float FAR_CLIP_MIN = 705.0f;      // DAT_142734b08
constexpr float FAR_CLIP_MAX = 5180.0f;     // DAT_142734b14
constexpr float FAR_CLIP2_MIN = 1000.0f;    // DAT_14261b418
constexpr float FAR_CLIP2_MAX = 50000.0f;   // DAT_14273753c
constexpr float FAR_CLIP3_MIN = 400.0f;     // DAT_142737534
constexpr float FAR_CLIP3_MAX = 6000.0f;    // DAT_142737538
constexpr float C30_MIN = 200.0f;           // DAT_142737528
constexpr float C30_MAX = 400.0f;           // DAT_142737534
constexpr float NEAR_CLIP = 0.2f;           // DAT_1426c7afc

}  // namespace

// ref: FUN_141365000
float WorldGetFarClipCVar() {
    return s_farClip;
}

// ref: FUN_141365010
float WorldGetFarClip2CVar() {
    return s_farClip2;
}

// ref: FUN_141365020
float WorldGetFarClip3CVar() {
    return s_farClip3;
}

// ref: FUN_141365030
float WorldGetNearClipCVar() {
    return s_nearClip;
}

// ref: FUN_141365b60
void WorldSetFarClipCVar(float value) {
    s_farClip = WorldClampFarClip(value);
}

// ref: FUN_141365b80
void WorldSetFarClip2CVar(float value) {
    s_farClip2 = WorldClampFarClip2(value);
}

// ref: FUN_141365ba0
void WorldSetFarClip3CVar(float value) {
    s_farClip3 = WorldClampFarClip3(value);
}

// ref: FUN_141365bc0
void WorldSetC30CVar(float value) {
    if (value <= C30_MIN) {
        value = C30_MIN;
    }
    if (C30_MAX <= value) {
        value = C30_MAX;
    }
    s_c30 = value;
}

// ref: FUN_141365be0
void WorldSetNearClipCVar() {
    s_nearClip = NEAR_CLIP;
}

// ref: FUN_141364d70
float WorldClampFarClip(float farClip) {
    if (FAR_CLIP_MAX <= farClip) {
        farClip = FAR_CLIP_MAX;
    }
    if (farClip <= FAR_CLIP_MIN) {
        farClip = FAR_CLIP_MIN;
    }
    return farClip;
}

// ref: FUN_141364d90
float WorldClampFarClip2(float farClip) {
    if (FAR_CLIP2_MAX <= farClip) {
        farClip = FAR_CLIP2_MAX;
    }
    if (farClip <= FAR_CLIP2_MIN) {
        farClip = FAR_CLIP2_MIN;
    }
    return farClip;
}

// ref: FUN_141364db0
float WorldClampFarClip3(float farClip) {
    if (FAR_CLIP3_MAX <= farClip) {
        farClip = FAR_CLIP3_MAX;
    }
    if (farClip <= FAR_CLIP3_MIN) {
        farClip = FAR_CLIP3_MIN;
    }
    return farClip;
}

// ref: FUN_141364dd0
float WorldGetNearClip() {
    return NEAR_CLIP;
}

namespace {

// DAT_142d00c2c, set with the player's state (FUN_141354ff0).
bool s_playerAlive = true;

}  // namespace

// ref: FUN_1413659f0
bool WorldPlayerAlive() {
    return s_playerAlive;
}
