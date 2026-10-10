// The graphics settings: quality groups, the value tables of the ten quality levels, what a
// device can take, and the world/render CVars with their callbacks (FUN_14135db10).

#include "client/GraphicsSettings.hpp"
#include "client/ConsoleDevice.hpp"
#include "client/RenderSettings.hpp"
#include "console/Command.hpp"
#include "console/Var.hpp"
#include "gx/Gx.hpp"
#include "os/Os.hpp"
#include "storm/String.hpp"
#include "texture/Texture.hpp"
#include "util/Fnv.hpp"
#include "util/Unimplemented.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

struct GRAPHICSVALIDITY {
    const char* m_name;
    const char* m_raidName;
    uint32_t (*m_check)(int32_t value);
};

// The world's options the CVars below set (Engine world, 0x1430ab9c0.. / 0x142d00c20..).
// FROST: the world that reads them is not ported; they are kept where its port will find them.
int32_t g_worldGraphicsMode;            // DAT_1430aaac8: 1 while the raid settings apply
float g_groundEffectDistSquared;        // DAT_1430ab9c0
int32_t g_groundEffectDensity;          // DAT_1430ab9c4
float g_groundEffectFade;               // DAT_1430ab9c8
uint8_t g_groundEffectAnimation;        // DAT_1430ab9cc
float g_wmoDoodadDist;                  // DAT_1430ab9d0
uint32_t g_worldRenderFlags = 0x37ffff; // DAT_1430ab9d4
uint8_t g_worldOptionsChanged;          // DAT_1430abef8
uint8_t g_dayNightMultithreaded;        // DAT_1430ab790
float g_groundEffectDist = 70.0f;       // DAT_142d00c20
float g_entityShadowFadeScale = 10.0f;  // DAT_142d00c28
float g_teleportMaxNoLoadDist = 200.0f; // DAT_142d00c30
float g_farClip = 1.0f;                 // DAT_142d00c34
float g_nearClip = 0.1f;                // DAT_142d00c38
float g_horizonStart = 1.0f;            // DAT_142d00c3c
float g_horizonClip = 1.0f;             // DAT_142d00c40
float g_lodObjectCullSize;              // DAT_142d00ca0
float g_lodObjectCullDist;              // DAT_142d00ca4
float g_lodObjectMinSize;               // DAT_142d00ca8
float g_lodObjectFadeScale;             // DAT_142d00cac
float g_terrainLodDist = 400.0f;        // DAT_142d0119c
float g_terrainLodDiv = 768.0f;         // DAT_142d011a0
float g_wmoLodDistScale = 1.0f;         // DAT_142d011a4
float g_entityLodOffset = 5.0f;         // DAT_142d011a8
float g_entityLodDist = 5.0f;           // DAT_142d011ac
float g_entityLodScale = 1.0f;          // DAT_142d011b0
float g_wmoLodDist;                     // DAT_1430ad2f0
float g_wmoPortalFadeScale = 1000.0f;   // DAT_142d01224
uint8_t g_wmoPortalInteriorFade;        // DAT_1430ad358
uint32_t g_maxLightCount;               // DAT_142d01228
float g_maxLightDist = 2048.0f;         // DAT_142d0122c
uint32_t g_particleDensityEnabled;      // DAT_142cfbb34
float g_particleDensity;                // DAT_142cfbb38
float g_particleMTDensity;              // DAT_142cfbb3c
uint32_t g_occlusionMaxJobs;            // DAT_142d5eae8
uint8_t g_renderSettingsChangeCount;    // DAT_142cfc00f: render settings switched on
// The two CVars the valar setting drives (DAT_1430a5c10, DAT_1430a5c20), registered by
// FUN_1412aefd0 (not ported).
CVar* g_valarCVars[2];

namespace {

// The quality groups (DAT_142d00978, DAT_142d00998 for the raid settings).
// DIVERGED: blz::unordered_map<blz::string, blz::vector<blz::string>> in the reference.
std::unordered_map<std::string, std::vector<std::string>> s_groups[2];

// The RenderScale default's source (DAT_142ccbff8, a blz::function).
float (*s_renderScaleDefault)();

// ref: FUN_14135c0d0
void AddGroup(bool raid, const char* name, std::initializer_list<const char*> members) {
    std::vector<std::string>& list = s_groups[raid ? 1 : 0][name];
    for (const char* member : members) {
        list.emplace_back(member);
    }
}

const std::vector<std::string>* FindGroup(const char* name, bool raid) {
    auto& groups = s_groups[raid ? 1 : 0];
    auto found = groups.find(name);
    return found == groups.end() ? nullptr : &found->second;
}

// strtol with the reference's "0x" check: hexadecimal when the text starts with 0x or 0X.
long ParseLong(const char* text) {
    int32_t radix = 10;
    if (text != nullptr && text[0] == '0' && ((text[1] + 0xa8u) & 0xdf) == 0) {
        radix = 16;
    }
    return strtol(text, nullptr, radix);
}

unsigned long ParseULong(const char* text) {
    int32_t radix = 10;
    if (text != nullptr && text[0] == '0' && ((text[1] + 0xa8u) & 0xdf) == 0) {
        radix = 16;
    }
    return strtoul(text, nullptr, radix);
}

// A setting with a raid twin applies when its kind is the one in use.
bool Applies(void* userData) {
    return (userData != nullptr) == (GraphicsSettingsMode() == 1);
}

void Print(const char* format, ...) {
    char text[0x100];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    ConsoleWriteText(text);
}

// The CVar values each quality level sets (PTR_PTR_142d009c0): [raid][level].
const GRAPHICSLEVELVALUE s_level0[] = {
    { "terrainMipLevel", 1 },
    { "componentTextureLevel", 1 },
    { "worldBaseMip", 1 },
    { "spellClutter", 100 },
    { "projectedTextures", 0 },
    { "farClip", 705 },
    { "horizonClip", 705 },
    { "horizonStart", 177 },
    { "wmoLodDist", 400 },
    { "doodadLodScale", 50 },
    { "terrainLodDist", 500 },
    { "terrainLodDiv", 384 },
    { "entityLodDist", 25 },
    { "entityShadowFadeScale", 10 },
    { "groundEffectDist", 70 },
    { "groundEffectDensity", 16 },
    { "lodObjectCullSize", 35 },
    { "lodObjectCullDist", 30 },
    { "lodObjectMinSize", 0 },
    { "lodObjectFadeScale", 50 },
    { "shadowMode", 0 },
    { "shadowTextureSize", 1024 },
    { "shadowSoft", 0 },
    { "shadowBlendCascades", 0 },
    { "waterDetail", 0 },
    { "reflectionMode", 0 },
    { "rippleDetail", 0 },
    { "sunshafts", 0 },
    { "particleDensity", 0 },
    { "particleMTDensity", 0 },
    { "weatherDensity", 0 },
    { "ssao", 0 },
    { "groundEffectAnimation", 0 },
    { nullptr, 0 },
};
const GRAPHICSLEVELVALUE s_level1[] = {
    { "terrainMipLevel", 1 },
    { "componentTextureLevel", 1 },
    { "worldBaseMip", 1 },
    { "spellClutter", 75 },
    { "projectedTextures", 0 },
    { "farClip", 2906 },
    { "horizonClip", 2906 },
    { "horizonStart", 727 },
    { "wmoLodDist", 400 },
    { "doodadLodScale", 50 },
    { "terrainLodDist", 500 },
    { "terrainLodDiv", 384 },
    { "entityLodDist", 25 },
    { "entityShadowFadeScale", 10 },
    { "groundEffectDist", 70 },
    { "groundEffectDensity", 32 },
    { "lodObjectCullSize", 25 },
    { "lodObjectCullDist", 30 },
    { "lodObjectMinSize", 0 },
    { "lodObjectFadeScale", 80 },
    { "shadowMode", 0 },
    { "shadowTextureSize", 1024 },
    { "shadowSoft", 0 },
    { "shadowBlendCascades", 0 },
    { "waterDetail", 0 },
    { "reflectionMode", 0 },
    { "rippleDetail", 0 },
    { "sunshafts", 0 },
    { "particleDensity", 10 },
    { "particleMTDensity", 33 },
    { "weatherDensity", 0 },
    { "ssao", 0 },
    { "groundEffectAnimation", 0 },
    { nullptr, 0 },
};
const GRAPHICSLEVELVALUE s_level2[] = {
    { "terrainMipLevel", 0 },
    { "componentTextureLevel", 0 },
    { "worldBaseMip", 0 },
    { "spellClutter", 50 },
    { "projectedTextures", 1 },
    { "farClip", 5108 },
    { "horizonClip", 5108 },
    { "horizonStart", 1277 },
    { "wmoLodDist", 400 },
    { "doodadLodScale", 75 },
    { "terrainLodDist", 500 },
    { "terrainLodDiv", 384 },
    { "entityLodDist", 25 },
    { "entityShadowFadeScale", 10 },
    { "groundEffectDist", 70 },
    { "groundEffectDensity", 64 },
    { "lodObjectCullSize", 20 },
    { "lodObjectCullDist", 30 },
    { "lodObjectMinSize", 0 },
    { "lodObjectFadeScale", 90 },
    { "shadowMode", 0 },
    { "shadowTextureSize", 1024 },
    { "shadowSoft", 0 },
    { "shadowBlendCascades", 0 },
    { "waterDetail", 0 },
    { "reflectionMode", 0 },
    { "rippleDetail", 0 },
    { "sunshafts", 0 },
    { "particleDensity", 25 },
    { "particleMTDensity", 50 },
    { "weatherDensity", 1 },
    { "ssao", 0 },
    { "groundEffectAnimation", 0 },
    { nullptr, 0 },
};
const GRAPHICSLEVELVALUE s_level3[] = {
    { "terrainMipLevel", 0 },
    { "componentTextureLevel", 0 },
    { "worldBaseMip", 0 },
    { "spellClutter", 25 },
    { "projectedTextures", 1 },
    { "farClip", 5108 },
    { "horizonClip", 5108 },
    { "horizonStart", 1277 },
    { "wmoLodDist", 400 },
    { "doodadLodScale", 75 },
    { "terrainLodDist", 500 },
    { "terrainLodDiv", 512 },
    { "entityLodDist", 25 },
    { "entityShadowFadeScale", 15 },
    { "groundEffectDist", 110 },
    { "groundEffectDensity", 64 },
    { "lodObjectCullSize", 20 },
    { "lodObjectCullDist", 30 },
    { "lodObjectMinSize", 0 },
    { "lodObjectFadeScale", 91 },
    { "shadowMode", 1 },
    { "shadowTextureSize", 1024 },
    { "shadowSoft", 0 },
    { "shadowBlendCascades", 0 },
    { "waterDetail", 1 },
    { "reflectionMode", 0 },
    { "rippleDetail", 0 },
    { "sunshafts", 0 },
    { "particleDensity", 50 },
    { "particleMTDensity", 80 },
    { "weatherDensity", 1 },
    { "ssao", 1 },
    { "groundEffectAnimation", 1 },
    { nullptr, 0 },
};
const GRAPHICSLEVELVALUE s_level4[] = {
    { "terrainMipLevel", 0 },
    { "componentTextureLevel", 0 },
    { "worldBaseMip", 0 },
    { "spellClutter", -1 },
    { "projectedTextures", 1 },
    { "farClip", 6000 },
    { "horizonClip", 6000 },
    { "horizonStart", 1277 },
    { "wmoLodDist", 400 },
    { "doodadLodScale", 100 },
    { "terrainLodDist", 500 },
    { "terrainLodDiv", 512 },
    { "entityLodDist", 25 },
    { "entityShadowFadeScale", 20 },
    { "groundEffectDist", 160 },
    { "groundEffectDensity", 64 },
    { "lodObjectCullSize", 20 },
    { "lodObjectCullDist", 30 },
    { "lodObjectMinSize", 0 },
    { "lodObjectFadeScale", 92 },
    { "shadowMode", 2 },
    { "shadowTextureSize", 1024 },
    { "shadowSoft", 0 },
    { "shadowBlendCascades", 0 },
    { "waterDetail", 2 },
    { "reflectionMode", 0 },
    { "rippleDetail", 1 },
    { "sunshafts", 1 },
    { "particleDensity", 80 },
    { "particleMTDensity", 80 },
    { "weatherDensity", 3 },
    { "ssao", 2 },
    { "groundEffectAnimation", 1 },
    { nullptr, 0 },
};
const GRAPHICSLEVELVALUE s_level5[] = {
    { "terrainMipLevel", 0 },
    { "componentTextureLevel", 0 },
    { "worldBaseMip", 0 },
    { "spellClutter", -1 },
    { "projectedTextures", 1 },
    { "farClip", 6000 },
    { "horizonClip", 6000 },
    { "horizonStart", 1277 },
    { "wmoLodDist", 400 },
    { "doodadLodScale", 100 },
    { "terrainLodDist", 500 },
    { "terrainLodDiv", 512 },
    { "entityLodDist", 25 },
    { "entityShadowFadeScale", 20 },
    { "groundEffectDist", 185 },
    { "groundEffectDensity", 80 },
    { "lodObjectCullSize", 19 },
    { "lodObjectCullDist", 30 },
    { "lodObjectMinSize", 0 },
    { "lodObjectFadeScale", 93 },
    { "shadowMode", 3 },
    { "shadowTextureSize", 2048 },
    { "shadowSoft", 0 },
    { "shadowBlendCascades", 1 },
    { "waterDetail", 2 },
    { "reflectionMode", 0 },
    { "rippleDetail", 1 },
    { "sunshafts", 2 },
    { "particleDensity", 80 },
    { "particleMTDensity", 100 },
    { "weatherDensity", 3 },
    { "ssao", 3 },
    { "groundEffectAnimation", 1 },
    { nullptr, 0 },
};
const GRAPHICSLEVELVALUE s_level6[] = {
    { "terrainMipLevel", 0 },
    { "componentTextureLevel", 0 },
    { "worldBaseMip", 0 },
    { "spellClutter", -1 },
    { "projectedTextures", 1 },
    { "farClip", 7000 },
    { "horizonClip", 7000 },
    { "horizonStart", 1277 },
    { "wmoLodDist", 400 },
    { "doodadLodScale", 100 },
    { "terrainLodDist", 500 },
    { "terrainLodDiv", 512 },
    { "entityLodDist", 25 },
    { "entityShadowFadeScale", 25 },
    { "groundEffectDist", 200 },
    { "groundEffectDensity", 80 },
    { "lodObjectCullSize", 18 },
    { "lodObjectCullDist", 30 },
    { "lodObjectMinSize", 0 },
    { "lodObjectFadeScale", 100 },
    { "shadowMode", 3 },
    { "shadowTextureSize", 2048 },
    { "shadowSoft", 0 },
    { "shadowBlendCascades", 1 },
    { "waterDetail", 2 },
    { "reflectionMode", 0 },
    { "rippleDetail", 1 },
    { "sunshafts", 2 },
    { "particleDensity", 100 },
    { "particleMTDensity", 100 },
    { "weatherDensity", 3 },
    { "ssao", 3 },
    { "groundEffectAnimation", 1 },
    { nullptr, 0 },
};
const GRAPHICSLEVELVALUE s_level7[] = {
    { "terrainMipLevel", 0 },
    { "componentTextureLevel", 0 },
    { "worldBaseMip", 0 },
    { "spellClutter", -1 },
    { "projectedTextures", 1 },
    { "farClip", 8000 },
    { "horizonClip", 8000 },
    { "horizonStart", 1277 },
    { "wmoLodDist", 400 },
    { "doodadLodScale", 125 },
    { "terrainLodDist", 500 },
    { "terrainLodDiv", 768 },
    { "entityLodDist", 25 },
    { "entityShadowFadeScale", 30 },
    { "groundEffectDist", 260 },
    { "groundEffectDensity", 128 },
    { "lodObjectCullSize", 18 },
    { "lodObjectCullDist", 30 },
    { "lodObjectMinSize", 0 },
    { "lodObjectFadeScale", 101 },
    { "shadowMode", 3 },
    { "shadowTextureSize", 2048 },
    { "shadowSoft", 1 },
    { "shadowBlendCascades", 1 },
    { "waterDetail", 2 },
    { "reflectionMode", 0 },
    { "rippleDetail", 1 },
    { "sunshafts", 2 },
    { "particleDensity", 100 },
    { "particleMTDensity", 100 },
    { "weatherDensity", 3 },
    { "ssao", 4 },
    { "groundEffectAnimation", 1 },
    { nullptr, 0 },
};
const GRAPHICSLEVELVALUE s_level8[] = {
    { "terrainMipLevel", 0 },
    { "componentTextureLevel", 0 },
    { "worldBaseMip", 0 },
    { "spellClutter", -1 },
    { "projectedTextures", 1 },
    { "farClip", 8000 },
    { "horizonClip", 8000 },
    { "horizonStart", 1277 },
    { "wmoLodDist", 400 },
    { "doodadLodScale", 150 },
    { "terrainLodDist", 500 },
    { "terrainLodDiv", 768 },
    { "entityLodDist", 25 },
    { "entityShadowFadeScale", 40 },
    { "groundEffectDist", 300 },
    { "groundEffectDensity", 128 },
    { "lodObjectCullSize", 16 },
    { "lodObjectCullDist", 30 },
    { "lodObjectMinSize", 30 },
    { "lodObjectFadeScale", 125 },
    { "shadowMode", 4 },
    { "shadowTextureSize", 2048 },
    { "shadowSoft", 1 },
    { "shadowBlendCascades", 1 },
    { "waterDetail", 2 },
    { "reflectionMode", 0 },
    { "rippleDetail", 1 },
    { "sunshafts", 2 },
    { "particleDensity", 100 },
    { "particleMTDensity", 100 },
    { "weatherDensity", 3 },
    { "ssao", 4 },
    { "groundEffectAnimation", 1 },
    { nullptr, 0 },
};
const GRAPHICSLEVELVALUE s_level9[] = {
    { "terrainMipLevel", 0 },
    { "componentTextureLevel", 0 },
    { "worldBaseMip", 0 },
    { "spellClutter", 0 },
    { "projectedTextures", 1 },
    { "farClip", 10000 },
    { "horizonClip", 10000 },
    { "horizonStart", 1277 },
    { "wmoLodDist", 400 },
    { "doodadLodScale", 150 },
    { "terrainLodDist", 500 },
    { "terrainLodDiv", 768 },
    { "entityLodDist", 25 },
    { "entityShadowFadeScale", 50 },
    { "groundEffectDist", 320 },
    { "groundEffectDensity", 256 },
    { "lodObjectCullSize", 14 },
    { "lodObjectCullDist", 30 },
    { "lodObjectMinSize", 20 },
    { "lodObjectFadeScale", 150 },
    { "shadowMode", 4 },
    { "shadowTextureSize", 2048 },
    { "shadowSoft", 1 },
    { "shadowBlendCascades", 1 },
    { "waterDetail", 3 },
    { "reflectionMode", 3 },
    { "rippleDetail", 2 },
    { "sunshafts", 2 },
    { "particleDensity", 100 },
    { "particleMTDensity", 100 },
    { "weatherDensity", 3 },
    { "ssao", 4 },
    { "groundEffectAnimation", 1 },
    { nullptr, 0 },
};
const GRAPHICSLEVELVALUE s_levelRaid0[] = {
    { "RAIDterrainMipLevel", 1 },
    { "RAIDcomponentTextureLevel", 1 },
    { "RAIDworldBaseMip", 1 },
    { "RAIDspellClutter", 100 },
    { "RAIDprojectedTextures", 0 },
    { "RAIDfarClip", 705 },
    { "RAIDhorizonClip", 705 },
    { "RAIDhorizonStart", 177 },
    { "RAIDwmoLodDist", 400 },
    { "RAIDdoodadLodScale", 50 },
    { "RAIDterrainLodDist", 500 },
    { "RAIDterrainLodDiv", 384 },
    { "RAIDentityLodDist", 25 },
    { "RAIDentityShadowFadeScale", 10 },
    { "RAIDgroundEffectDist", 70 },
    { "RAIDgroundEffectDensity", 16 },
    { "RAIDlodObjectCullSize", 35 },
    { "RAIDlodObjectCullDist", 30 },
    { "RAIDlodObjectMinSize", 0 },
    { "RAIDlodObjectFadeScale", 50 },
    { "RAIDshadowMode", 0 },
    { "RAIDshadowTextureSize", 1024 },
    { "RAIDshadowSoft", 0 },
    { "RAIDshadowBlendCascades", 0 },
    { "RAIDwaterDetail", 0 },
    { "RAIDreflectionMode", 0 },
    { "RAIDrippleDetail", 0 },
    { "RAIDsunshafts", 0 },
    { "RAIDparticleDensity", 0 },
    { "RAIDparticleMTDensity", 0 },
    { "RAIDweatherDensity", 0 },
    { "RAIDssao", 0 },
    { "RAIDgroundEffectAnimation", 0 },
    { nullptr, 0 },
};
const GRAPHICSLEVELVALUE s_levelRaid1[] = {
    { "RAIDterrainMipLevel", 1 },
    { "RAIDcomponentTextureLevel", 1 },
    { "RAIDworldBaseMip", 1 },
    { "RAIDspellClutter", 75 },
    { "RAIDprojectedTextures", 0 },
    { "RAIDfarClip", 2906 },
    { "RAIDhorizonClip", 2906 },
    { "RAIDhorizonStart", 727 },
    { "RAIDwmoLodDist", 400 },
    { "RAIDdoodadLodScale", 50 },
    { "RAIDterrainLodDist", 500 },
    { "RAIDterrainLodDiv", 384 },
    { "RAIDentityLodDist", 25 },
    { "RAIDentityShadowFadeScale", 10 },
    { "RAIDgroundEffectDist", 70 },
    { "RAIDgroundEffectDensity", 32 },
    { "RAIDlodObjectCullSize", 25 },
    { "RAIDlodObjectCullDist", 30 },
    { "RAIDlodObjectMinSize", 0 },
    { "RAIDlodObjectFadeScale", 80 },
    { "RAIDshadowMode", 0 },
    { "RAIDshadowTextureSize", 1024 },
    { "RAIDshadowSoft", 0 },
    { "RAIDshadowBlendCascades", 0 },
    { "RAIDwaterDetail", 0 },
    { "RAIDreflectionMode", 0 },
    { "RAIDrippleDetail", 0 },
    { "RAIDsunshafts", 0 },
    { "RAIDparticleDensity", 10 },
    { "RAIDparticleMTDensity", 33 },
    { "RAIDweatherDensity", 0 },
    { "RAIDssao", 0 },
    { "RAIDgroundEffectAnimation", 0 },
    { nullptr, 0 },
};
const GRAPHICSLEVELVALUE s_levelRaid2[] = {
    { "RAIDterrainMipLevel", 0 },
    { "RAIDcomponentTextureLevel", 0 },
    { "RAIDworldBaseMip", 0 },
    { "RAIDspellClutter", 50 },
    { "RAIDprojectedTextures", 1 },
    { "RAIDfarClip", 5108 },
    { "RAIDhorizonClip", 5108 },
    { "RAIDhorizonStart", 1277 },
    { "RAIDwmoLodDist", 400 },
    { "RAIDdoodadLodScale", 75 },
    { "RAIDterrainLodDist", 500 },
    { "RAIDterrainLodDiv", 384 },
    { "RAIDentityLodDist", 25 },
    { "RAIDentityShadowFadeScale", 10 },
    { "RAIDgroundEffectDist", 70 },
    { "RAIDgroundEffectDensity", 64 },
    { "RAIDlodObjectCullSize", 20 },
    { "RAIDlodObjectCullDist", 30 },
    { "RAIDlodObjectMinSize", 0 },
    { "RAIDlodObjectFadeScale", 90 },
    { "RAIDshadowMode", 0 },
    { "RAIDshadowTextureSize", 1024 },
    { "RAIDshadowSoft", 0 },
    { "RAIDshadowBlendCascades", 0 },
    { "RAIDwaterDetail", 0 },
    { "RAIDreflectionMode", 0 },
    { "RAIDrippleDetail", 0 },
    { "RAIDsunshafts", 0 },
    { "RAIDparticleDensity", 25 },
    { "RAIDparticleMTDensity", 50 },
    { "RAIDweatherDensity", 1 },
    { "RAIDssao", 0 },
    { "RAIDgroundEffectAnimation", 0 },
    { nullptr, 0 },
};
const GRAPHICSLEVELVALUE s_levelRaid3[] = {
    { "RAIDterrainMipLevel", 0 },
    { "RAIDcomponentTextureLevel", 0 },
    { "RAIDworldBaseMip", 0 },
    { "RAIDspellClutter", 25 },
    { "RAIDprojectedTextures", 1 },
    { "RAIDfarClip", 5108 },
    { "RAIDhorizonClip", 5108 },
    { "RAIDhorizonStart", 1277 },
    { "RAIDwmoLodDist", 400 },
    { "RAIDdoodadLodScale", 75 },
    { "RAIDterrainLodDist", 500 },
    { "RAIDterrainLodDiv", 512 },
    { "RAIDentityLodDist", 25 },
    { "RAIDentityShadowFadeScale", 15 },
    { "RAIDgroundEffectDist", 110 },
    { "RAIDgroundEffectDensity", 64 },
    { "RAIDlodObjectCullSize", 20 },
    { "RAIDlodObjectCullDist", 30 },
    { "RAIDlodObjectMinSize", 0 },
    { "RAIDlodObjectFadeScale", 91 },
    { "RAIDshadowMode", 1 },
    { "RAIDshadowTextureSize", 1024 },
    { "RAIDshadowSoft", 0 },
    { "RAIDshadowBlendCascades", 0 },
    { "RAIDwaterDetail", 1 },
    { "RAIDreflectionMode", 0 },
    { "RAIDrippleDetail", 0 },
    { "RAIDsunshafts", 0 },
    { "RAIDparticleDensity", 50 },
    { "RAIDparticleMTDensity", 80 },
    { "RAIDweatherDensity", 1 },
    { "RAIDssao", 1 },
    { "RAIDgroundEffectAnimation", 1 },
    { nullptr, 0 },
};
const GRAPHICSLEVELVALUE s_levelRaid4[] = {
    { "RAIDterrainMipLevel", 0 },
    { "RAIDcomponentTextureLevel", 0 },
    { "RAIDworldBaseMip", 0 },
    { "RAIDspellClutter", -1 },
    { "RAIDprojectedTextures", 1 },
    { "RAIDfarClip", 6000 },
    { "RAIDhorizonClip", 6000 },
    { "RAIDhorizonStart", 1277 },
    { "RAIDwmoLodDist", 400 },
    { "RAIDdoodadLodScale", 100 },
    { "RAIDterrainLodDist", 500 },
    { "RAIDterrainLodDiv", 512 },
    { "RAIDentityLodDist", 25 },
    { "RAIDentityShadowFadeScale", 20 },
    { "RAIDgroundEffectDist", 160 },
    { "RAIDgroundEffectDensity", 64 },
    { "RAIDlodObjectCullSize", 20 },
    { "RAIDlodObjectCullDist", 30 },
    { "RAIDlodObjectMinSize", 0 },
    { "RAIDlodObjectFadeScale", 92 },
    { "RAIDshadowMode", 2 },
    { "RAIDshadowTextureSize", 1024 },
    { "RAIDshadowSoft", 0 },
    { "RAIDshadowBlendCascades", 0 },
    { "RAIDwaterDetail", 2 },
    { "RAIDreflectionMode", 0 },
    { "RAIDrippleDetail", 1 },
    { "RAIDsunshafts", 1 },
    { "RAIDparticleDensity", 80 },
    { "RAIDparticleMTDensity", 80 },
    { "RAIDweatherDensity", 3 },
    { "RAIDssao", 2 },
    { "RAIDgroundEffectAnimation", 1 },
    { nullptr, 0 },
};
const GRAPHICSLEVELVALUE s_levelRaid5[] = {
    { "RAIDterrainMipLevel", 0 },
    { "RAIDcomponentTextureLevel", 0 },
    { "RAIDworldBaseMip", 0 },
    { "RAIDspellClutter", -1 },
    { "RAIDprojectedTextures", 1 },
    { "RAIDfarClip", 6000 },
    { "RAIDhorizonClip", 6000 },
    { "RAIDhorizonStart", 1277 },
    { "RAIDwmoLodDist", 400 },
    { "RAIDdoodadLodScale", 100 },
    { "RAIDterrainLodDist", 500 },
    { "RAIDterrainLodDiv", 512 },
    { "RAIDentityLodDist", 25 },
    { "RAIDentityShadowFadeScale", 20 },
    { "RAIDgroundEffectDist", 185 },
    { "RAIDgroundEffectDensity", 80 },
    { "RAIDlodObjectCullSize", 19 },
    { "RAIDlodObjectCullDist", 30 },
    { "RAIDlodObjectMinSize", 0 },
    { "RAIDlodObjectFadeScale", 93 },
    { "RAIDshadowMode", 3 },
    { "RAIDshadowTextureSize", 2048 },
    { "RAIDshadowSoft", 0 },
    { "RAIDshadowBlendCascades", 1 },
    { "RAIDwaterDetail", 2 },
    { "RAIDreflectionMode", 0 },
    { "RAIDrippleDetail", 1 },
    { "RAIDsunshafts", 2 },
    { "RAIDparticleDensity", 80 },
    { "RAIDparticleMTDensity", 100 },
    { "RAIDweatherDensity", 3 },
    { "RAIDssao", 3 },
    { "RAIDgroundEffectAnimation", 1 },
    { nullptr, 0 },
};
const GRAPHICSLEVELVALUE s_levelRaid6[] = {
    { "RAIDterrainMipLevel", 0 },
    { "RAIDcomponentTextureLevel", 0 },
    { "RAIDworldBaseMip", 0 },
    { "RAIDspellClutter", -1 },
    { "RAIDprojectedTextures", 1 },
    { "RAIDfarClip", 7000 },
    { "RAIDhorizonClip", 7000 },
    { "RAIDhorizonStart", 1277 },
    { "RAIDwmoLodDist", 400 },
    { "RAIDdoodadLodScale", 100 },
    { "RAIDterrainLodDist", 500 },
    { "RAIDterrainLodDiv", 512 },
    { "RAIDentityLodDist", 25 },
    { "RAIDentityShadowFadeScale", 25 },
    { "RAIDgroundEffectDist", 200 },
    { "RAIDgroundEffectDensity", 80 },
    { "RAIDlodObjectCullSize", 18 },
    { "RAIDlodObjectCullDist", 30 },
    { "RAIDlodObjectMinSize", 0 },
    { "RAIDlodObjectFadeScale", 100 },
    { "RAIDshadowMode", 3 },
    { "RAIDshadowTextureSize", 2048 },
    { "RAIDshadowSoft", 0 },
    { "RAIDshadowBlendCascades", 1 },
    { "RAIDwaterDetail", 2 },
    { "RAIDreflectionMode", 0 },
    { "RAIDrippleDetail", 1 },
    { "RAIDsunshafts", 2 },
    { "RAIDparticleDensity", 100 },
    { "RAIDparticleMTDensity", 100 },
    { "RAIDweatherDensity", 3 },
    { "RAIDssao", 3 },
    { "RAIDgroundEffectAnimation", 1 },
    { nullptr, 0 },
};
const GRAPHICSLEVELVALUE s_levelRaid7[] = {
    { "RAIDterrainMipLevel", 0 },
    { "RAIDcomponentTextureLevel", 0 },
    { "RAIDworldBaseMip", 0 },
    { "RAIDspellClutter", -1 },
    { "RAIDprojectedTextures", 1 },
    { "RAIDfarClip", 8000 },
    { "RAIDhorizonClip", 8000 },
    { "RAIDhorizonStart", 1277 },
    { "RAIDwmoLodDist", 400 },
    { "RAIDdoodadLodScale", 125 },
    { "RAIDterrainLodDist", 500 },
    { "RAIDterrainLodDiv", 768 },
    { "RAIDentityLodDist", 25 },
    { "RAIDentityShadowFadeScale", 30 },
    { "RAIDgroundEffectDist", 260 },
    { "RAIDgroundEffectDensity", 128 },
    { "RAIDlodObjectCullSize", 18 },
    { "RAIDlodObjectCullDist", 30 },
    { "RAIDlodObjectMinSize", 0 },
    { "RAIDlodObjectFadeScale", 101 },
    { "RAIDshadowMode", 3 },
    { "RAIDshadowTextureSize", 2048 },
    { "RAIDshadowSoft", 1 },
    { "RAIDshadowBlendCascades", 1 },
    { "RAIDwaterDetail", 2 },
    { "RAIDreflectionMode", 0 },
    { "RAIDrippleDetail", 1 },
    { "RAIDsunshafts", 2 },
    { "RAIDparticleDensity", 100 },
    { "RAIDparticleMTDensity", 100 },
    { "RAIDweatherDensity", 3 },
    { "RAIDssao", 4 },
    { "RAIDgroundEffectAnimation", 1 },
    { nullptr, 0 },
};
const GRAPHICSLEVELVALUE s_levelRaid8[] = {
    { "RAIDterrainMipLevel", 0 },
    { "RAIDcomponentTextureLevel", 0 },
    { "RAIDworldBaseMip", 0 },
    { "RAIDspellClutter", -1 },
    { "RAIDprojectedTextures", 1 },
    { "RAIDfarClip", 8000 },
    { "RAIDhorizonClip", 8000 },
    { "RAIDhorizonStart", 1277 },
    { "RAIDwmoLodDist", 400 },
    { "RAIDdoodadLodScale", 150 },
    { "RAIDterrainLodDist", 500 },
    { "RAIDterrainLodDiv", 768 },
    { "RAIDentityLodDist", 25 },
    { "RAIDentityShadowFadeScale", 40 },
    { "RAIDgroundEffectDist", 300 },
    { "RAIDgroundEffectDensity", 128 },
    { "RAIDlodObjectCullSize", 16 },
    { "RAIDlodObjectCullDist", 30 },
    { "RAIDlodObjectMinSize", 30 },
    { "RAIDlodObjectFadeScale", 125 },
    { "RAIDshadowMode", 4 },
    { "RAIDshadowTextureSize", 2048 },
    { "RAIDshadowSoft", 1 },
    { "RAIDshadowBlendCascades", 1 },
    { "RAIDwaterDetail", 2 },
    { "RAIDreflectionMode", 0 },
    { "RAIDrippleDetail", 1 },
    { "RAIDsunshafts", 2 },
    { "RAIDparticleDensity", 100 },
    { "RAIDparticleMTDensity", 100 },
    { "RAIDweatherDensity", 3 },
    { "RAIDssao", 4 },
    { "RAIDgroundEffectAnimation", 1 },
    { nullptr, 0 },
};
const GRAPHICSLEVELVALUE s_levelRaid9[] = {
    { "RAIDterrainMipLevel", 0 },
    { "RAIDcomponentTextureLevel", 0 },
    { "RAIDworldBaseMip", 0 },
    { "RAIDspellClutter", 0 },
    { "RAIDprojectedTextures", 1 },
    { "RAIDfarClip", 10000 },
    { "RAIDhorizonClip", 10000 },
    { "RAIDhorizonStart", 1277 },
    { "RAIDwmoLodDist", 400 },
    { "RAIDdoodadLodScale", 150 },
    { "RAIDterrainLodDist", 500 },
    { "RAIDterrainLodDiv", 768 },
    { "RAIDentityLodDist", 25 },
    { "RAIDentityShadowFadeScale", 50 },
    { "RAIDgroundEffectDist", 320 },
    { "RAIDgroundEffectDensity", 256 },
    { "RAIDlodObjectCullSize", 14 },
    { "RAIDlodObjectCullDist", 30 },
    { "RAIDlodObjectMinSize", 20 },
    { "RAIDlodObjectFadeScale", 150 },
    { "RAIDshadowMode", 4 },
    { "RAIDshadowTextureSize", 2048 },
    { "RAIDshadowSoft", 1 },
    { "RAIDshadowBlendCascades", 1 },
    { "RAIDwaterDetail", 3 },
    { "RAIDreflectionMode", 3 },
    { "RAIDrippleDetail", 2 },
    { "RAIDsunshafts", 2 },
    { "RAIDparticleDensity", 100 },
    { "RAIDparticleMTDensity", 100 },
    { "RAIDweatherDensity", 3 },
    { "RAIDssao", 4 },
    { "RAIDgroundEffectAnimation", 1 },
    { nullptr, 0 },
};
const GRAPHICSLEVELVALUE* const s_levelValues[2][10] = {
    { s_level0, s_level1, s_level2, s_level3, s_level4, s_level5, s_level6, s_level7, s_level8, s_level9 },
    { s_levelRaid0, s_levelRaid1, s_levelRaid2, s_levelRaid3, s_levelRaid4, s_levelRaid5, s_levelRaid6, s_levelRaid7, s_levelRaid8, s_levelRaid9 },
};

// ---- the world's option setters -----------------------------------------------------------

// ref: FUN_141377b90
void WorldOptionsChanged() {
    g_worldOptionsChanged = 1;
}

// ref: FUN_141351ff0
void WorldSetGroundEffectAnimation(bool animate) {
    if (g_groundEffectAnimation != static_cast<uint8_t>(animate)) {
        g_groundEffectAnimation = animate;
        WorldOptionsChanged();
    }
}

// ref: FUN_141352010
void WorldSetGroundEffectDensity(int32_t density) {
    if (g_groundEffectDensity != density) {
        g_groundEffectDensity = density;
        WorldOptionsChanged();
    }
}

// ref: FUN_141352030
void WorldSetGroundEffectFade(float fade) {
    if (g_groundEffectFade != fade) {
        g_groundEffectFade = fade;
        WorldOptionsChanged();
    }
}

// ref: FUN_141352050
void WorldSetGroundEffectDist(float dist) {
    if (g_groundEffectDist != dist) {
        g_groundEffectDistSquared = dist * dist;
        g_groundEffectDist = dist;
        WorldOptionsChanged();
    }
}

// 10 .. 100.
// ref: FUN_141352080
void WorldSetEntityShadowFadeScale(float scale) {
    if (scale < 10.0f) {
        g_entityShadowFadeScale = 10.0f;
        return;
    }
    g_entityShadowFadeScale = 100.0f <= scale ? 100.0f : scale;
}

// ref: FUN_141352740
void WorldSetRenderFlag(uint32_t flag, int32_t enable) {
    if (enable != 0) {
        g_worldRenderFlags |= flag;
        return;
    }
    g_worldRenderFlags &= ~flag;
}

// 705 .. 5180.
// ref: FUN_141352a90
void WorldSetWmoDoodadDist(float dist) {
    if (dist <= 705.0f) {
        dist = 705.0f;
    }
    if (5180.0f <= dist) {
        dist = 5180.0f;
    }
    g_wmoDoodadDist = dist;
}

// ref: FUN_141365b60
void WorldSetFarClip(float clip) {
    if (5180.0f <= clip) {
        clip = 5180.0f;
    }
    if (clip <= 705.0f) {
        clip = 705.0f;
    }
    g_farClip = clip;
}

// ref: FUN_141365b80
void WorldSetHorizonClip(float clip) {
    if (50000.0f <= clip) {
        clip = 50000.0f;
    }
    if (clip <= 1000.0f) {
        clip = 1000.0f;
    }
    g_horizonClip = clip;
}

// ref: FUN_141365ba0
void WorldSetHorizonStart(float start) {
    if (6000.0f <= start) {
        start = 6000.0f;
    }
    if (start <= 400.0f) {
        start = 400.0f;
    }
    g_horizonStart = start;
}

// ref: FUN_141365bc0
void WorldSetTeleportMaxNoLoadDist(float dist) {
    if (dist <= 200.0f) {
        dist = 200.0f;
    }
    if (400.0f <= dist) {
        dist = 400.0f;
    }
    g_teleportMaxNoLoadDist = dist;
}

// The near clip is fixed; the CVar's value is ignored.
// ref: FUN_141365be0
void WorldSetNearClip() {
    g_nearClip = 0.2f;
}

// ref: FUN_141365bf0
void WorldSetLodObjectCullDist(float dist) {
    g_lodObjectCullDist = dist;
}

// ref: FUN_141365c00
void WorldSetLodObjectCullSize(float size) {
    g_lodObjectCullSize = size;
}

// ref: FUN_141365c10
void WorldSetLodObjectFadeScale(float scale) {
    g_lodObjectFadeScale = scale;
}

// ref: FUN_141365c20
void WorldSetLodObjectMinSize(float size) {
    g_lodObjectMinSize = size;
}

// The ground effect density's range (FUN_141378600) and the distance's (FUN_141378610).
// ref: FUN_141378600
void GroundEffectDensityRange(int32_t* low, int32_t* high) {
    *low = 0x10;
    *high = 0x100;
}

// ref: FUN_141378610
void GroundEffectDistRange(float* low, float* high) {
    *low = 70.0f;
    *high = 500.0f;
}

// A percentage, 0.5 .. 2.
// ref: FUN_1413c0db0
void WorldSetEntityLodScale(float percent) {
    g_entityLodScale = percent * 0.01f;
    if (g_entityLodScale < 0.5f) {
        g_entityLodScale = 0.5f;
        return;
    }
    if (2.0f <= g_entityLodScale) {
        g_entityLodScale = 2.0f;
    }
}

// ref: FUN_1413c0fc0
void WorldSetEntityLodDist(float dist) {
    if (dist < 5.0f) {
        g_entityLodDist = 5.0f;
        return;
    }
    g_entityLodDist = 1000.0f <= dist ? 1000.0f : dist;
}

// ref: FUN_1413c0ff0
void WorldSetEntityLodOffset(float offset) {
    if (offset < 5.0f) {
        g_entityLodOffset = 5.0f;
        return;
    }
    g_entityLodOffset = 1000.0f <= offset ? 1000.0f : offset;
}

// ref: FUN_1413c1050
void WorldSetTerrainLodDist(float dist) {
    if (dist < 200.0f) {
        g_terrainLodDist = 200.0f;
        return;
    }
    g_terrainLodDist = 5180.0f <= dist ? 5180.0f : dist;
}

// ref: FUN_1413c1080
void WorldSetTerrainLodDiv(float div) {
    if (div < 256.0f) {
        g_terrainLodDiv = 256.0f;
        return;
    }
    g_terrainLodDiv = 1024.0f <= div ? 1024.0f : div;
}

// ref: FUN_1413c10b0
void WorldSetWmoLodDist(float dist) {
    if (dist < 200.0f) {
        g_wmoLodDist = 200.0f;
        return;
    }
    g_wmoLodDist = 5180.0f <= dist ? 5180.0f : dist;
}

// ref: FUN_1413c10e0
void WorldSetWmoLodDistScale(float scale) {
    if (scale < 0.5f) {
        g_wmoLodDistScale = 0.5f;
        return;
    }
    g_wmoLodDistScale = 2.0f <= scale ? 2.0f : scale;
}

// ref: FUN_14124b740
void ParticleSetDensityEnabled(uint32_t enabled) {
    g_particleDensityEnabled = enabled;
}

// The density, 0..1, of the main thread's particles or (mt) the job threads'.
// ref: FUN_14124e100
void ParticleSetDensity(float density, bool mt) {
    if (mt) {
        g_particleMTDensity = 0.0f;
        if (0.0f <= density) {
            g_particleMTDensity = 1.0f <= density ? 1.0f : density;
        }
        return;
    }
    if (density < 0.0f) {
        g_particleDensity = 0.0f;
        return;
    }
    g_particleDensity = 1.0f <= density ? 1.0f : density;
}

// 1 .. 16 jobs.
// ref: FUN_14254ff10
void OcclusionSetMaxJobs(uint32_t jobs) {
    if (jobs == 0) {
        g_occlusionMaxJobs = 1;
        return;
    }
    g_occlusionMaxJobs = jobs > 0x10 ? 0x10 : jobs;
}

// The sky's cloud level of detail.
// ref: FUN_142443390
void SkySetCloudLod(long lod) {
    (void)lod;
    // The sky module (the cloud layers of every sky, DAT_142d4ca20) is not ported.
    FROST_UNIMPLEMENTED();
}

// ---- the render settings the CVars change ---------------------------------------------------

// A switch counted in DAT_142cfc00f while on (FUN_1412b2260).
// ref: FUN_1412b2260
void RenderCountSwitch(bool on, uint8_t* state) {
    if (static_cast<uint8_t>(on) == *state) {
        return;
    }
    if (on) {
        g_renderSettingsChangeCount++;
        *state = 1;
        return;
    }
    g_renderSettingsChangeCount--;
    *state = 0;
}

uint8_t s_shadowSwitch;     // DAT_1430a5d88
uint8_t s_waterSwitch;      // DAT_1447ce202

void SetFlag(uint32_t bit, bool on) {
    g_pendingRenderSettings.m_flags = (on ? bit : 0) | (g_pendingRenderSettings.m_flags & ~bit);
}

// ref: FUN_1412b20e0
bool RenderSetSunshafts(uint32_t quality) {
    if (2 < quality) {
        return false;
    }
    g_pendingRenderSettings.m_sunshafts = static_cast<uint8_t>(quality);
    return true;
}

// ref: FUN_1412b5ac0
void RenderSetShadowMultithreaded(bool on) {
    SetFlag(0x20000, on);
}

// ref: FUN_1412b5b00
bool RenderSetShadowRt(uint32_t mode) {
    if (3 < mode) {
        return false;
    }
    if (mode != 0 && GxDeviceCaps()[0xaf] < 2) {
        return false;
    }
    g_pendingRenderSettings.m_flags ^= ((mode & 0xff) << 3 ^ g_pendingRenderSettings.m_flags) & 0x18;
    return true;
}

// ref: FUN_1412b5c20
void RenderSetShadowBlendCascades(bool on) {
    SetFlag(4, on);
}

// ref: FUN_1412b5fa0
bool RenderSetShadowMode(uint8_t mode) {
    if (mode < 5) {
        g_pendingRenderSettings.m_shadowMode = mode;
        RenderCountSwitch(true, &s_shadowSwitch);
        return true;
    }
    return false;
}

// ref: FUN_1412b6060
void RenderSetShadowSoft(bool on) {
    if (on && reinterpret_cast<int32_t*>(GxDeviceCaps())[2] == 0) {
        return;
    }
    SetFlag(2, on);
}

// 0x200, 0x400 or 0x800.
// ref: FUN_1412b60c0
bool RenderSetShadowTextureSize(int32_t size) {
    if (((size - 0x200u) & 0xfffff9ffu) == 0 && size != 0x600) {
        g_pendingRenderSettings.m_shadowTextureSize = static_cast<uint16_t>(size);
        return true;
    }
    return false;
}

// ref: FUN_1412af510
void RenderSetValar(uint8_t valar) {
    static const float s_first[4] = { 0.0f, 0.31f, 0.66f, 0.0f };   // DAT_142cfc398
    static const float s_second[4] = { 0.0f, 2.13f, 2.13f, 0.0f };  // DAT_142cfc3a8
    g_pendingRenderSettings.m_flags ^= (static_cast<uint32_t>(valar) << 6 ^ g_pendingRenderSettings.m_flags) & 0xc0;
    if (valar != 0) {
        if (g_valarCVars[0] != nullptr) {
            CVarSetFloat(g_valarCVars[0], s_first[valar & 3], true, false, false, true);
        }
        if (g_valarCVars[1] != nullptr) {
            CVarSetFloat(g_valarCVars[1], s_second[valar & 3], true, false, false, true);
        }
    }
}

// ref: FUN_1425272a0
bool RenderSetReflectionDownscale(uint32_t downscale) {
    if (downscale < 5) {
        g_pendingRenderSettings.m_reflectionDownscale = static_cast<uint8_t>(downscale);
        return true;
    }
    return false;
}

// ref: FUN_1425272c0
bool RenderSetReflectionMode(uint32_t mode) {
    if (mode != 0) {
        if (mode >= 4 || reinterpret_cast<int32_t*>(GxDeviceCaps())[2] == 0) {
            return false;
        }
    }
    g_pendingRenderSettings.m_reflectionMode = static_cast<uint8_t>(mode);
    return true;
}

// ref: FUN_142527300
bool RenderSetRippleDetail(int32_t detail) {
    if (detail < 3) {
        g_pendingRenderSettings.m_rippleDetail = static_cast<uint8_t>(detail);
        return true;
    }
    return false;
}

// ref: FUN_142527330
bool RenderSetWaterDetail(uint32_t detail) {
    if (detail < 4) {
        RenderCountSwitch(1 < detail, &s_waterSwitch);
        g_pendingRenderSettings.m_waterDetail = static_cast<uint8_t>(detail);
        return true;
    }
    return false;
}

// ref: FUN_1425580f0
bool RenderSetSsao(uint8_t ssao) {
    if (4 < ssao) {
        return false;
    }
    if (ssao != 0 && reinterpret_cast<int32_t*>(GxDeviceCaps())[2] == 0) {
        return false;
    }
    g_pendingRenderSettings.m_ssao = ssao;
    return true;
}

// ---- the checks of what a device can take (0 when it can) ---------------------------------

// ref: FUN_1402ca290
uint32_t CheckAlways(int32_t) {
    return 0;
}

// The component textures' lowest level needs two or more logical processors.
// ref: FUN_140309f20
uint32_t CheckComponentTextureLevel(int32_t level) {
    if (static_cast<uint32_t>(level) < 3) {
        if (level == 0) {
            return OsGetCpuInfo()->m_logical < 2 ? 4 : 0;
        }
        return 0;
    }
    return 1;
}

// ref: FUN_141355210
uint32_t CheckFarClip(int32_t clip) {
    float value = static_cast<float>(clip);
    if (value < 705.0f) {
        return 1;
    }
    return value > 5180.0f;
}

// ref: FUN_1413551c0
uint32_t CheckGroundEffectDensity(int32_t density) {
    if (static_cast<uint32_t>(density) < 0x10) {
        return 1;
    }
    return static_cast<uint32_t>(density) > 0x100;
}

// ref: FUN_1413551e0
uint32_t CheckGroundEffectDist(int32_t dist) {
    float value = static_cast<float>(static_cast<uint32_t>(dist));
    if (value < 70.0f) {
        return 1;
    }
    return value > 500.0f;
}

// ref: FUN_1412b6580
uint32_t CheckShadowMode(int32_t mode) {
    return 4u < static_cast<uint32_t>(mode);
}

// ref: FUN_1412b65c0
uint32_t CheckShadowTextureSize(int32_t size) {
    return !(((size - 0x200u) & 0xfffff9ffu) == 0 && size != 0x600);
}

// ref: FUN_1412b6590
uint32_t CheckShadowSoft(int32_t soft) {
    if (soft != 0) {
        return reinterpret_cast<int32_t*>(GxDeviceCaps())[2] == 0 ? 9 : 0;
    }
    return 0;
}

// ref: FUN_1412b6550
uint32_t CheckShadowRt(int32_t mode) {
    if (3u < static_cast<uint32_t>(mode)) {
        return 1;
    }
    if (mode != 0) {
        return GxDeviceCaps()[0xaf] < 2 ? 3 : 0;
    }
    return 0;
}

// ref: FUN_1412b2100
uint32_t CheckSunshafts(int32_t quality) {
    if (quality != 0) {
        return 2u < static_cast<uint32_t>(quality);
    }
    return 0;
}

// -1 (dynamic) or 0 .. 100.
// ref: FUN_14135bea0
uint32_t CheckSpellClutter(int32_t clutter) {
    if (clutter == -1) {
        return 0;
    }
    if (clutter < 0) {
        return 1;
    }
    return clutter > 100;
}

// ref: FUN_142527470
uint32_t CheckWaterDetail(int32_t detail) {
    if (detail != 0) {
        return 3u < static_cast<uint32_t>(detail);
    }
    return 0;
}

// ref: FUN_142527460
uint32_t CheckRippleDetail(int32_t detail) {
    if (detail != 0) {
        return 2 < detail;
    }
    return 0;
}

// ref: FUN_142527420
uint32_t CheckReflectionMode(int32_t mode) {
    if (mode == 0) {
        return 0;
    }
    if (3u < static_cast<uint32_t>(mode)) {
        return 1;
    }
    return reinterpret_cast<int32_t*>(GxDeviceCaps())[2] == 0 ? 9 : 0;
}

// ref: FUN_142527410
uint32_t CheckReflectionDownscale(int32_t downscale) {
    if (downscale != 0) {
        return 4u < static_cast<uint32_t>(downscale);
    }
    return 0;
}

// ref: FUN_142558820
uint32_t CheckSsao(int32_t ssao) {
    if (4u < static_cast<uint32_t>(ssao)) {
        return 1;
    }
    if (ssao != 0) {
        return reinterpret_cast<int32_t*>(GxDeviceCaps())[2] == 0 ? 9 : 0;
    }
    return 0;
}

// SSAO type 2 needs a D3D12-class device of a supported vendor and driver.
// ref: FUN_142558860
uint32_t CheckSsaoType(int32_t type) {
    if (2u < static_cast<uint32_t>(type)) {
        return 1;
    }
    if (type == 2) {
        if (reinterpret_cast<int32_t*>(GxDeviceCaps())[2] == 0) {
            return 9;
        }
        GxAdapterInfo* adapter = GxSelectGpuToUse();
        auto gpu = reinterpret_cast<const uint8_t*>(adapter->m_gpu);
        auto data = reinterpret_cast<const uint8_t*>(adapter->m_data);
        if (*reinterpret_cast<const int16_t*>(gpu + 0x10) == -0x7f7a) {
            return 0x8003;
        }
        if (data[0] == 6 && data[0xc9] < 5) {
            return 0x14;
        }
        if (*reinterpret_cast<const uint32_t*>(data + 0xac) < 5 && GxApiIsD3d11()) {
            return *reinterpret_cast<const int32_t*>(data + 0xac) == 2 ? 3 : 0x19;
        }
    }
    return 0;
}

// MSAA for alpha-tested geometry: refused on old Windows and some devices.
// ref: FUN_14135bec0
uint32_t CheckMsaaAlphaTest(int32_t mode) {
    const OSVERSION* version = OsGetVersion();
    GxAdapterInfo* adapter = GxSelectGpuToUse();
    if (mode == 0) {
        return 0;
    }
    if (mode != 1) {
        return 1;
    }
    auto data = reinterpret_cast<const uint8_t*>(adapter->m_data);
    if ((data[0x224] & 0x20) != 0) {
        return 0;
    }
    auto words = reinterpret_cast<const int32_t*>(version);
    if (words[0] == 10) {
        return 9;
    }
    if (words[0] == 6) {
        return static_cast<uint32_t>(words[1]) < 0xb ? 0x16 : 3;
    }
    return 3;
}

// ref: FUN_14135bfb0
uint32_t CheckResampleQuality(int32_t quality) {
    return RenderSettingsCheckUpscaler(static_cast<uint8_t>(quality));
}

// ref: FUN_14135bfc0
uint32_t CheckLowLatencyMode(int32_t mode) {
    return RenderSettingsCheckLowLatencyMode(static_cast<uint8_t>(mode));
}

// Valar needs a D3D12 device on Windows 10 with variable-rate shading tier 2 and ray tracing.
// ref: FUN_1412af200
uint32_t CheckValarMode(uint8_t valar, int32_t msaa) {
    if (valar == 0) {
        return 0;
    }
    if (2 < valar) {
        return 1;
    }
    auto version = reinterpret_cast<const int32_t*>(OsGetVersion());
    if (version[0] == 6) {
        return 0x1b;
    }
    if (version[0] == 10) {
        if (GxCurrentApi() != 2) {
            return 0xc;
        }
        if (!GxVariableRateShadingSupported(2)) {
            return 0xd;
        }
        if (reinterpret_cast<int32_t*>(GxDeviceCaps())[2] != 2) {
            return 10;
        }
        return msaa != 0 ? 3 : 0;
    }
    return 3;
}

// ref: FUN_14135bfd0
uint32_t CheckValar(int32_t valar) {
    return CheckValarMode(static_cast<uint8_t>(valar), g_renderSettings.m_msaaSamples);
}

// The CVars whose values a device may not support, and the check for each (0x142d00720).
const GRAPHICSVALIDITY s_validity[25] = {
    { "terrainMipLevel", "RAIDterrainMipLevel", CheckAlways },
    { "worldBaseMip", "RAIDworldBaseMip", CheckAlways },
    { "componentTextureLevel", "RAIDcomponentTextureLevel", CheckComponentTextureLevel },
    { "farClip", "RAIDfarClip", CheckFarClip },
    { "wmoLODDist", "RAIDwmoLODDist", CheckAlways },
    { "terrainLodDist", "RAIDterrainLodDist", CheckAlways },
    { "terrainLodDiv", "RAIDterrainLodDiv", CheckAlways },
    { "groundEffectDensity", "RAIDgroundEffectDensity", CheckGroundEffectDensity },
    { "groundEffectDist", "RAIDgroundEffectDist", CheckGroundEffectDist },
    { "shadowMode", "RAIDshadowMode", CheckShadowMode },
    { "shadowTextureSize", "RAIDshadowTextureSize", CheckShadowTextureSize },
    { "shadowSoft", "RAIDshadowSoft", CheckShadowSoft },
    { "shadowrt", "shadowrt", CheckShadowRt },
    { "sunShafts", "RAIDsunShafts", CheckSunshafts },
    { "spellClutter", "RAIDspellClutter", CheckSpellClutter },
    { "waterDetail", "RAIDwaterDetail", CheckWaterDetail },
    { "rippleDetail", "RAIDrippleDetail", CheckRippleDetail },
    { "reflectionMode", "RAIDreflectionMode", CheckReflectionMode },
    { "reflectionDownscale", "RAIDreflectionDownscale", CheckReflectionDownscale },
    { "ssao", "RAIDSSAO", CheckSsao },
    { "ResolvedSSAOType", "ResolvedSSAOType", CheckSsaoType },
    { "msaaAlphaTest", "msaaAlphaTest", CheckMsaaAlphaTest },
    { "ResampleQuality", "ResampleQuality", CheckResampleQuality },
    { "LowLatencyMode", "LowLatencyMode", CheckLowLatencyMode },
    { "vrsValar", "vrsValar", CheckValar },
};

// "<cx>x<cy>": a variable-rate shading rate, 0xff when not one or not supported.
// ref: FUN_14135cf50
uint8_t ParseShadingRate(const char* text) {
    int32_t x = 0;
    int32_t y = 0;
    uint8_t rate = 0xff;
    bool coarse = false;
    if (sscanf_s(text, "%dx%d", &x, &y) == 2) {
        if (x == 1 && (y == 1 || y == 2)) {
            rate = static_cast<uint8_t>(y - 1);
            coarse = false;
        } else if (x == 2 && y == 1) {
            rate = 4;
        } else if (x == 2 && (y == 2 || y == 4)) {
            rate = y == 2 ? 5 : 6;
            coarse = y == 4;
        } else if (x == 4 && (y == 2 || y == 4)) {
            rate = y == 2 ? 9 : 10;
        }
    }
    if (rate == 0xff) {
        ConsoleWriteText("Input must be 1x1, 1x2, 2x1, 2x2, 2x4, 4x2, or 4x4");
        return 0xff;
    }
    (void)coarse;
    if (!GxVariableRateShadingSupported(1)) {
        ConsoleWriteText("Variable Rate Shading not supported on this hardware");
        return 0xff;
    }
    return rate;
}

// The filter modes the texture filtering setting picks (DAT_142d00c44, DAT_142d00c50).
const uint8_t s_filterModes[6] = { 3, 4, 5, 5, 5, 5 };
const uint32_t s_filterLevels[6] = { 1, 1, 2, 4, 8, 16 };

// ---- the callbacks -----------------------------------------------------------------------

// ref: FUN_14135d4d0
bool GxMTDisableCallback(CVar*, const char*, const char* value, void*) {
    long disable = ParseLong(value);
    SetFlag(0x8000, disable == 0);
    ConsoleWriteText(disable != 0 ? "Multithreaded rendering disabled." : "Multithreaded rendering enabled.");
    return true;
}

bool SetMtFlag(const char* value, uint32_t bit, const char* enabled, const char* disabled) {
    long on = ParseLong(value);
    SetFlag(bit, on != 0);
    ConsoleWriteText(on == 0 ? disabled : enabled);
    return true;
}

// ref: FUN_14135d650
bool GxMTBeginDrawCallback(CVar*, const char*, const char* value, void*) {
    return SetMtFlag(value, 0x10000, "Multithreaded BeginDraw enabled.", "Multithreaded BeginDraw disabled.");
}

// ref: FUN_14135d950
bool GxMTShadowCallback(CVar*, const char*, const char* value, void* raid) {
    long on = ParseLong(value);
    if (Applies(raid)) {
        RenderSetShadowMultithreaded(on != 0);
        Print("Multithread shadows changed to %d.", on != 0);
    }
    return true;
}

// ref: FUN_14135d8d0
bool GxMTPrepassCallback(CVar*, const char*, const char* value, void*) {
    return SetMtFlag(value, 0x40000, "Multithreaded prepass enabled.", "Multithreaded prepass disabled.");
}

// ref: FUN_14135d6d0
bool GxMTOpaqueM2Callback(CVar*, const char*, const char* value, void*) {
    return SetMtFlag(value, 0x80000, "Multithreaded opaque pass enabled.", "Multithreaded opaque pass disabled.");
}

// ref: FUN_14135d750
bool GxMTOpaqueM2NoReflectCallback(CVar*, const char*, const char* value, void*) {
    return SetMtFlag(value, 0x100000, "Multithreaded opaque pass enabled.", "Multithreaded opaque pass disabled.");
}

// ref: FUN_14135d550
bool GxMTAlphaM2Callback(CVar*, const char*, const char* value, void*) {
    return SetMtFlag(value, 0x800000, "Multithreaded alpha M2 pass enabled.", "Multithreaded alpha M2 pass disabled.");
}

// ref: FUN_14135d7d0
bool GxMTOpaqueWMOCallback(CVar*, const char*, const char* value, void*) {
    return SetMtFlag(value, 0x200000, "Multithreaded opaque WMO pass enabled.", "Multithreaded opaque WMO pass disabled.");
}

// ref: FUN_14135d9f0
bool GxMTTerrainCallback(CVar*, const char*, const char* value, void*) {
    return SetMtFlag(value, 0x400000, "Multithreaded terrain pass enabled.", "Multithreaded terrain pass disabled.");
}

// ref: FUN_14135d850
bool GxMTParticulateVolumesCallback(CVar*, const char*, const char* value, void*) {
    return SetMtFlag(value, 0x2000000, "Multithreaded particulate volumes pass enabled.",
        "Multithreaded particulate volumes pass disabled.");
}

// ref: FUN_14135d5d0
bool GxMTAlphaWaterCallback(CVar*, const char*, const char* value, void*) {
    return SetMtFlag(value, 0x10000000, "Multithreaded alpha water volumes pass enabled.",
        "Multithreaded alpha water volumes pass disabled.");
}

// ref: FUN_14135c630
bool DnMTUpdateCallback(CVar*, const char*, const char* value, void*) {
    long on = ParseLong(value);
    g_dayNightMultithreaded = on != 0;
    ConsoleWriteText(on == 0 ? "Multithreaded daynight update disabled." : "Multithreaded daynight update enabled.");
    return true;
}

// ref: FUN_14135c810
bool FarclipCallback(CVar*, const char*, const char* value, void* raid) {
    float clip = SStrToFloat(value);
    if (Applies(raid)) {
        WorldSetFarClip(clip);
    }
    return true;
}

// ref: FUN_141362f40
bool NearclipCallback(CVar*, const char*, const char* value, void*) {
    SStrToFloat(value);
    WorldSetNearClip();
    return true;
}

// ref: FUN_14135da70
bool HorizonClipCallback(CVar*, const char*, const char* value, void* raid) {
    if (Applies(raid)) {
        WorldSetHorizonClip(SStrToFloat(value));
    }
    return true;
}

// ref: FUN_14135dac0
bool HorizonStartCallback(CVar*, const char*, const char* value, void* raid) {
    if (Applies(raid)) {
        WorldSetHorizonStart(SStrToFloat(value));
    }
    return true;
}

// 0 .. 100.
// ref: FUN_141362fb0
bool ParticleDensityCallback(CVar*, const char*, const char* value, void* raid) {
    uint32_t density = static_cast<uint32_t>(ParseLong(value));
    if (density < 0x65) {
        if (Applies(raid)) {
            ParticleSetDensityEnabled(density != 0);
            ParticleSetDensity(static_cast<float>(static_cast<int32_t>(density)) * 0.01f, false);
        }
        return true;
    }
    ConsoleWriteText("Value must be between 0 and 100.");
    return false;
}

// ref: FUN_141363060
bool ParticleMTDensityCallback(CVar*, const char*, const char* value, void* raid) {
    uint32_t density = static_cast<uint32_t>(ParseLong(value));
    if (density < 0x65) {
        if (Applies(raid)) {
            ParticleSetDensity(static_cast<float>(static_cast<int32_t>(density)) * 0.01f, true);
        }
        return true;
    }
    ConsoleWriteText("Value must be between 0 and 100.");
    return false;
}

// ref: FUN_1413644b0
bool WaterDetailCallback(CVar*, const char*, const char* value, void* raid) {
    long detail = ParseLong(value);
    if (CheckWaterDetail(detail) != 0) {
        ConsoleWriteText("Unsupported water detail mode.");
        return false;
    }
    if (Applies(raid)) {
        RenderSetWaterDetail(static_cast<uint32_t>(detail));
        Print("Water detail changed to %d", detail);
    }
    return true;
}

// ref: FUN_1413633c0
bool RippleDetailCallback(CVar*, const char*, const char* value, void* raid) {
    long detail = ParseLong(value);
    if (CheckRippleDetail(detail) != 0) {
        ConsoleWriteText("Unsupported ripple detail mode.");
        return false;
    }
    if (Applies(raid)) {
        RenderSetRippleDetail(detail);
        Print("Ripple detail changed to %d", detail);
    }
    return true;
}

// ref: FUN_141363220
bool ReflectionModeCallback(CVar*, const char*, const char* value, void* raid) {
    long mode = ParseLong(value);
    if (CheckReflectionMode(mode) != 0) {
        ConsoleWriteText("Unsupported reflection mode (0 = screen space, 1 = sky, 2 = sky/terrain, 3 = sky/terrain/wmo).");
        return false;
    }
    if (Applies(raid)) {
        RenderSetReflectionMode(static_cast<uint32_t>(mode));
        Print("Reflection mode changed to %d", mode);
    }
    return true;
}

// ref: FUN_141363190
bool ReflectionDownscaleCallback(CVar*, const char*, const char* value, void*) {
    long downscale = ParseLong(value);
    if (RenderSetReflectionDownscale(static_cast<uint32_t>(downscale))) {
        Print("Reflection downscale changed to %d", downscale);
        return true;
    }
    ConsoleWriteText("Unsupported ripple downscale (choose a value from 0-4).");
    return false;
}

// ref: FUN_141364120
bool SunShaftsCallback(CVar*, const char*, const char* value, void* raid) {
    long quality = ParseLong(value);
    if (CheckSunshafts(quality) != 0) {
        ConsoleWriteText("Unsupported sunshafts detail mode.");
        return false;
    }
    if (Applies(raid)) {
        RenderSetSunshafts(static_cast<uint32_t>(quality));
        Print("Sunshafts quality changed to %d", quality);
    }
    return true;
}

// ref: FUN_14135d2c0
bool GroundEffectDensityCallback(CVar*, const char*, const char* value, void* raid) {
    long density = ParseLong(value);
    int32_t low = 0;
    int32_t high = 0;
    GroundEffectDensityRange(&low, &high);
    if (low <= density && density <= high) {
        if (Applies(raid)) {
            WorldSetGroundEffectDensity(density);
        }
        return true;
    }
    Print("Value must be between %d and %d.", low, high);
    return false;
}

// ref: FUN_14135d450
bool GroundEffectFadeCallback(CVar*, const char*, const char* value, void* raid) {
    uint32_t fade = static_cast<uint32_t>(ParseLong(value));
    if (Applies(raid)) {
        WorldSetGroundEffectFade(static_cast<float>(fade) * 0.01f);
    }
    return true;
}

// ref: FUN_14135d390
bool GroundEffectDistCallback(CVar*, const char*, const char* value, void* raid) {
    float dist = SStrToFloat(value);
    float low = 0.0f;
    float high = 0.0f;
    GroundEffectDistRange(&low, &high);
    if (low <= dist && dist <= high) {
        if (Applies(raid)) {
            WorldSetGroundEffectDist(dist);
        }
        return true;
    }
    Print("Value must be between %g and %g.", static_cast<double>(low), static_cast<double>(high));
    return false;
}

// ref: FUN_14135d250
bool GroundEffectAnimationCallback(CVar*, const char*, const char* value, void* raid) {
    long animate = ParseLong(value);
    if (Applies(raid)) {
        WorldSetGroundEffectAnimation(animate != 0);
    }
    return true;
}

// ref: FUN_141363100
bool ProjectedTexturesCallback(CVar*, const char*, const char* value, void* raid) {
    long on = ParseLong(value);
    if (Applies(raid)) {
        ConsoleWriteText(on == 0 ? "Projected textures disabled." : "Projected textures enabled.");
        WorldSetRenderFlag(0x800, on);
    }
    return true;
}

// -1 for dynamic (from the target frame rate), else an intensity of 0 .. 100.
// ref: FUN_141364040
bool SpellClutterCallback(CVar*, const char*, const char* value, void* raid) {
    long clutter = ParseLong(value);
    if (0x65u < static_cast<unsigned long>(clutter + 1)) {
        ConsoleWriteText("Invalid. -1 is dynamic based on TargetFPS, otherwise [0-100] Intensity");
        return false;
    }
    if (Applies(raid)) {
        if (clutter == -1) {
            ConsoleWriteText("Spell Clutter set to dynamic");
            g_pendingRenderSettings.m_flags |= 0x100;
            return true;
        }
        ConsoleWriteText(clutter == 0 ? "Spell Clutter disabled" : "Spell Clutter intensity value set");
        g_pendingRenderSettings.m_flags &= ~0x100u;
        g_pendingRenderSettings.m_spellClutter = static_cast<uint8_t>(clutter);
    }
    return true;
}

// ref: FUN_141363c90
bool ShadowModeCallback(CVar*, const char*, const char* value, void* raid) {
    uint32_t mode = static_cast<uint32_t>(ParseLong(value));
    if (CheckShadowMode(mode) != 0) {
        ConsoleWriteText("Unsupported shadow mode.");
        return false;
    }
    if (Applies(raid)) {
        RenderSetShadowMode(static_cast<uint8_t>(mode));
        static const char* const s_names[5] = {
            "0 - Blob shadows, precomputed terrain shadows",
            "1 - 1 band dynamic shadows on units, 1024, precomputed terrain shadows",
            "2 - 2 band dynamic shadows on units and terrain, 1024",
            "3 - 3 band dynamic shadows on units and terrain, 2048",
            "4 - 4 band dynamic shadows on units and terrain, 2048",
        };
        // FUN_1412b41e0
        uint8_t index = static_cast<uint8_t>(mode);
        Print("Shadow mode changed to %s", index < 5 ? s_names[index] : "Unknown shadow mode");
    }
    return true;
}

// ref: FUN_141363f30
bool ShadowTextureSizeCallback(CVar*, const char*, const char* value, void* raid) {
    long size = ParseLong(value);
    if (CheckShadowTextureSize(size) != 0) {
        ConsoleWriteText("Unsupported shadow texture size.");
        return false;
    }
    if (Applies(raid)) {
        RenderSetShadowTextureSize(size);
        Print("Shadow texture size changed to %d.", size);
    }
    return true;
}

// ref: FUN_141363e60
bool ShadowSoftCallback(CVar*, const char*, const char* value, void* raid) {
    bool soft = ParseLong(value) != 0;
    if (CheckShadowSoft(soft) != 0) {
        ConsoleWriteText("Unsupported soft shadow mode.");
        return false;
    }
    if (Applies(raid)) {
        RenderSetShadowSoft(soft);
        Print("Soft shadows changed to %d.", soft);
    }
    return true;
}

// ref: FUN_141363bf0
bool ShadowBlendCascadesCallback(CVar*, const char*, const char* value, void* raid) {
    long blend = ParseLong(value);
    if (Applies(raid)) {
        RenderSetShadowBlendCascades(blend != 0);
        Print("Shadow cascade blending changed to %d", blend != 0);
    }
    return true;
}

// An unsupported value the first time (no old value) falls back to 0.
// ref: FUN_141363d60
bool ShadowRtCallback(CVar* cvar, const char* oldValue, const char* value, void* raid) {
    long mode = ParseLong(value);
    if (CheckShadowRt(mode) == 0) {
        if (Applies(raid)) {
            RenderSetShadowRt(static_cast<uint32_t>(mode));
            static const char* const s_names[4] = {
                "0 (Disabled)",
                "1 (Fair) - Reduced-resolution directional light shadows",
                "2 (Good) - Reduced-resolution directional & local light shadows",
                "3 (High) - Full-resolution directional & local light shadows",
            };
            // FUN_1412b4500
            Print("Shadow RT mode changed to %s",
                static_cast<uint32_t>(mode) < 4 ? s_names[mode] : "Unknown shadow RT mode");
        }
        return true;
    }
    if (oldValue == nullptr) {
        CVarSet(cvar, "0", true, false, false, true);
        return false;
    }
    ConsoleWriteText("Unsupported shadow RT mode.");
    return false;
}

// ref: FUN_141362e40
bool MaxLightCountCallback(CVar*, const char*, const char* value, void*) {
    uint32_t count = static_cast<uint32_t>(ParseLong(value));
    if (count < 0x21) {
        g_maxLightCount = count < 0x20 ? count : 0x20;
        return true;
    }
    Print("maxLightCount must be in range 0 to %d.", 0x20);
    return false;
}

// ref: FUN_141362ec0
bool MaxLightDistCallback(CVar*, const char*, const char* value, void*) {
    float dist = SStrToFloat(value);
    if (0.0f <= dist && dist <= 2048.0f) {
        g_maxLightDist = 2048.0f <= dist ? 2048.0f : dist;
        if (g_maxLightDist < 0.0f) {
            g_maxLightDist = 0.0f;
        }
        return true;
    }
    Print("maxLightDist must be in range 0 to %g.", 2048.0);
    return false;
}

// ref: FUN_141363490
bool SSAOCallback(CVar*, const char*, const char* value, void* raid) {
    uint32_t ssao = static_cast<uint32_t>(ParseLong(value));
    if (CheckSsao(ssao) != 0) {
        ConsoleWriteText("Unsupported SSAO mode.");
        return false;
    }
    if (Applies(raid)) {
        RenderSetSsao(static_cast<uint8_t>(ssao));
        Print("SSAO mode set to %d", ssao);
    }
    return true;
}

// ref: FUN_141363560
bool SSAOTypeCallback(CVar*, const char*, const char* value, void*) {
    uint32_t type = static_cast<uint32_t>(ParseLong(value));
    if (CheckSsaoType(type) != 0) {
        ConsoleWriteText("Unsupported SSAO type.");
        return false;
    }
    // FUN_142558130
    if (CheckSsaoType(type & 0xff) == 0) {
        g_pendingRenderSettings.m_ssaoType = static_cast<uint8_t>(type);
    }
    Print("SSAO type set to %d", type);
    return true;
}

// 0 .. 3.
// ref: FUN_14135c5b0
bool SkyCloudLODCallback(CVar*, const char*, const char* value, void*) {
    long lod = ParseLong(value);
    if (lod < 0) {
        lod = 0;
    } else if (3 < lod) {
        lod = 3;
    }
    SkySetCloudLod(lod);
    Print("SkyCloudLOD set to %i", lod);
    return true;
}

// ref: FUN_141364350
bool TextureFilteringModeCallback(CVar*, const char*, const char* value, void*) {
    long mode = ParseLong(value);
    // FUN_1413653d0
    int32_t count = 6;
    if (count <= mode) {
        Print("Texture filtering mode must be in range 0 to %d.", count - 1);
        return false;
    }
    // FUN_1413653b0, FUN_1413653c0
    g_pendingRenderSettings.m_filterMode = s_filterModes[mode];
    g_pendingRenderSettings.m_filterLevel = static_cast<uint8_t>(s_filterLevels[mode]);
    ConsoleWriteText("Texture filtering mode updated.");
    return true;
}

// ref: FUN_141364210
bool TerrainLodDistCallback(CVar*, const char*, const char* value, void* raid) {
    float dist = SStrToFloat(value);
    if (Applies(raid)) {
        WorldSetTerrainLodDist(dist);
    }
    return true;
}

// ref: FUN_141364260
bool TerrainLodDivCallback(CVar*, const char*, const char* value, void* raid) {
    float div = SStrToFloat(value);
    if (Applies(raid)) {
        WorldSetTerrainLodDiv(div);
    }
    return true;
}

// ref: FUN_1413645a0
bool WmoLodDistCallback(CVar*, const char*, const char* value, void* raid) {
    float dist = SStrToFloat(value);
    if (Applies(raid)) {
        WorldSetWmoLodDist(dist);
    }
    return true;
}

// ref: FUN_1413645f0
bool WmoLodDistScaleCallback(CVar*, const char*, const char* value, void* raid) {
    float scale = SStrToFloat(value);
    if (Applies(raid)) {
        WorldSetWmoLodDistScale(scale);
    }
    return true;
}

// ref: FUN_141364580
bool WmoDoodadDistCallback(CVar*, const char*, const char* value, void*) {
    WorldSetWmoDoodadDist(SStrToFloat(value));
    return true;
}

// 1 .. 1000.
// ref: FUN_141364640
bool WmoPortalFadeScaleCallback(CVar*, const char*, const char* value, void*) {
    g_wmoPortalFadeScale = SStrToFloat(value);
    if (1000.0f <= g_wmoPortalFadeScale) {
        g_wmoPortalFadeScale = 1000.0f;
    }
    if (g_wmoPortalFadeScale <= 1.0f) {
        g_wmoPortalFadeScale = 1.0f;
    }
    return true;
}

// ref: FUN_141364670
bool WmoPortalInteriorFadeCallback(CVar*, const char*, const char* value, void*) {
    g_wmoPortalInteriorFade = ParseULong(value) != 0;
    return true;
}

// ref: FUN_14135c750
bool EntityLodDistCallback(CVar*, const char*, const char* value, void* raid) {
    if (Applies(raid)) {
        WorldSetEntityLodDist(SStrToFloat(value));
    }
    return true;
}

// ref: FUN_14135c7a0
bool EntityLodOffsetCallback(CVar*, const char*, const char* value, void*) {
    WorldSetEntityLodOffset(SStrToFloat(value));
    return true;
}

// ref: FUN_14135c7c0
bool EntityShadowFadeScaleCallback(CVar*, const char*, const char* value, void* raid) {
    if (Applies(raid)) {
        WorldSetEntityShadowFadeScale(SStrToFloat(value));
    }
    return true;
}

// ref: FUN_14135c690
bool DoodadLodScaleCallback(CVar*, const char*, const char* value, void* raid) {
    if (Applies(raid)) {
        WorldSetEntityLodScale(SStrToFloat(value));
    }
    return true;
}

// 0 .. 1.
// ref: FUN_1413642b0
bool TerrainMipLevelCallback(CVar*, const char*, const char* value, void* raid) {
    long level = ParseLong(value);
    if (1 < level) {
        ConsoleWriteText("Terrain mip level must be in range 0 - 1.");
        return false;
    }
    if (Applies(raid)) {
        ConsoleWriteFormat(0, "Terrain mip level changed to %d.", level);
    }
    return true;
}

// ref: FUN_14026bea0
bool WorldMaxMipLevelCallback(CVar*, const char*, const char*, void*) {
    return true;
}

// ref: FUN_1413646c0
bool WorldBaseMipCallback(CVar*, const char*, const char* value, void* raid) {
    long mip = ParseLong(value);
    if (Applies(raid)) {
        TextureSetResolutionReduction(static_cast<uint32_t>(mip));
    }
    return true;
}

// ref: FUN_141362f60
bool OcclusionMaxJobsCallback(CVar*, const char*, const char* value, void*) {
    OcclusionSetMaxJobs(static_cast<uint32_t>(ParseLong(value)));
    return true;
}

// ref: FUN_14135bf50
bool LowLatencyModeCallback(CVar*, const char*, const char* value, void*) {
    uint32_t mode = static_cast<uint32_t>(ParseLong(value));
    bool unsupported = RenderSettingsCheckLowLatencyMode(static_cast<uint8_t>(mode)) != 0;
    if (!unsupported) {
        g_pendingRenderSettings.m_lowLatencyMode = static_cast<uint8_t>(mode);
    }
    return !unsupported;
}

bool MaxJobsCallback(const char* value, const char* format) {
    uint32_t jobs = static_cast<uint32_t>(ParseLong(value));
    if (0x20 < jobs) {
        Print(format, 0x20);
        return false;
    }
    return true;
}

// ref: FUN_141364930
bool WorldViewCullMaxJobsCallback(CVar*, const char*, const char* value, void*) {
    return MaxJobsCallback(value, "worldViewCullMaxJobs must be in range 0 to %d");
}

// ref: FUN_141364730
bool WorldIntersectMaxJobsCallback(CVar*, const char*, const char* value, void*) {
    return MaxJobsCallback(value, "worldIntersectMaxJobs must be in range 0 to %u");
}

// ref: FUN_1413629e0
bool LocateViewerMaxJobsCallback(CVar*, const char*, const char* value, void*) {
    return MaxJobsCallback(value, "locateViewerMaxJobs must be in range 0 to %u");
}

// ref: FUN_1413632f0
bool RenderScaleCallback(CVar*, const char*, const char* value, void*) {
    float scale = SStrToFloat(value);
    if (!RenderSettingsSetRenderScale(scale)) {
        ConsoleWriteText("Failed to set render scale.");
        return false;
    }
    ConsoleWriteFormat(0, "Render scale changed to %g", static_cast<double>(scale));
    return true;
}

// ref: FUN_141363350
bool ResampleQualityCallback(CVar*, const char*, const char* value, void*) {
    uint32_t quality = static_cast<uint32_t>(ParseLong(value));
    if (!RenderSettingsSetUpscaler(static_cast<uint8_t>(quality))) {
        ConsoleWriteText("Failed to set resample quality.");
        return false;
    }
    ConsoleWriteFormat(0, "Resample quality changed to %d", quality);
    return true;
}

// "<colour samples>[x<coverage samples>]", as powers of two.
// ref: FUN_141362d90
bool MSAAQualityCallback(CVar*, const char*, const char* value, void*) {
    int32_t samples = 0;
    char separator = 0;
    int32_t coverage = 0;
    if (sscanf_s(value, "%u%c%u", &samples, &separator, 1, &coverage) > 0) {
        if (RenderSettingsSetMsaa(static_cast<uint8_t>(samples), static_cast<uint8_t>(coverage))) {
            if (coverage + samples != 0) {
                ConsoleWriteFormat(0, "MSAA set to %d color samples, %d coverage samples", 1 << (samples & 0x1f),
                    1 << ((coverage + samples) & 0x1f));
                return true;
            }
            ConsoleWriteFormat(0, "MSAA disabled");
            return true;
        }
        ConsoleWriteText("Failed to set MSAA quality.");
    }
    return false;
}

// ref: FUN_141362d00
bool MSAAAlphaTestCallback(CVar*, const char*, const char* value, void*) {
    long mode = ParseLong(value);
    if (CheckMsaaAlphaTest(mode) != 0) {
        ConsoleWriteText("Unsupported MSAA Alpha Test mode.");
        return false;
    }
    SetFlag(1, mode != 0);
    ConsoleWriteText(mode == 0 ? "MSAA for alpha-test disabled." : "MSAA for alpha-test enabled.");
    return true;
}

// ref: FUN_141364000
bool SpecularCallback(CVar*, const char*, const char* value, void*) {
    float specular = SStrToFloat(value);
    if (0.0f <= specular && specular <= 1.0f) {
        RenderSettingsSet04(specular);
        return true;
    }
    ConsoleWriteText("Specular multiplier must be between 0.0 and 1.0");
    return false;
}

// ref: FUN_141364480
bool VrsWorldGeoCallback(CVar*, const char*, const char* value, void*) {
    uint8_t rate = ParseShadingRate(value);
    if (rate == 0xff) {
        return false;
    }
    g_pendingRenderSettings.m_25 = rate;
    return true;
}

// ref: FUN_141364400
bool VrsParticlesCallback(CVar*, const char*, const char* value, void*) {
    uint8_t rate = ParseShadingRate(value);
    if (rate == 0xff) {
        return false;
    }
    g_pendingRenderSettings.m_26 = rate;
    return true;
}

// ref: FUN_141364430
bool VrsValarCallback(CVar*, const char*, const char* value, void*) {
    RenderSetValar(static_cast<uint8_t>(ParseLong(value)));
    return true;
}

bool LodObjectCallback(const char* value, void* raid, float low, float high, const char* message,
    void (*set)(float)) {
    if (!Applies(raid)) {
        return true;
    }
    float amount = SStrToFloat(value);
    if (amount < low || high < amount) {
        ConsoleWriteText(message);
        return false;
    }
    set(amount);
    return true;
}

// ref: FUN_141362b00
bool LodObjectCullSizeCallback(CVar*, const char*, const char* value, void* raid) {
    return LodObjectCallback(value, raid, 1.0f, 100.0f, "lodObjectCullSize must be in range 1 to 100.",
        WorldSetLodObjectCullSize);
}

// ref: FUN_141362a60
bool LodObjectCullDistCallback(CVar*, const char*, const char* value, void* raid) {
    return LodObjectCallback(value, raid, 1.0f, 200.0f, "lodObjectCullDist must be in range 1.0 to 200.0.",
        WorldSetLodObjectCullDist);
}

// ref: FUN_141362c40
bool LodObjectMinSizeCallback(CVar*, const char*, const char* value, void* raid) {
    return LodObjectCallback(value, raid, 0.0f, 1000.0f, "lodObjectMinSize must be in range 0 to 1000.",
        WorldSetLodObjectMinSize);
}

// ref: FUN_141362ba0
bool LodObjectFadeScaleCallback(CVar*, const char*, const char* value, void* raid) {
    return LodObjectCallback(value, raid, 50.0f, 5000.0f, "lodObjectFadescale must be in range 50 to 5000.",
        WorldSetLodObjectFadeScale);
}

// ref: FUN_141362cd0
bool LodObjectSizeScaleCallback(CVar*, const char*, const char*, void*) {
    ConsoleWriteText("lodObjectSizeScale cannot be changed.");
    return true;
}

// A group's UI value: each member CVar takes the value the matching quality level gives it.
// ref: FUN_14135d020
bool GraphicsTextureResolutionCallback(CVar* cvar, const char*, const char* value, void* raid) {
    bool isRaid = raid != nullptr;
    const char* name = cvar->m_name.m_data;
    const std::vector<std::string>* members = FindGroup(name, isRaid);
    float setting = SStrToFloat(value);
    int32_t level = GraphicsGroupLevel(name, static_cast<int32_t>(setting), isRaid);
    if (static_cast<uint32_t>(level) >= 10) {
        return false;
    }
    if (members != nullptr) {
        for (const std::string& member : *members) {
            CVar* target = CVarLookup(t2util::ConstFnvHashStringI(member.c_str()));
            if (target == nullptr) {
                break;
            }
            CVarSetInt(target, GraphicsLevelValue(member.c_str(), static_cast<uint32_t>(level), isRaid), true, false,
                false, true);
        }
    }
    return true;
}

// ref: FUN_1413641f0
bool TeleportMaxNoLoadDistCallback(CVar*, const char*, const char* value, void*) {
    WorldSetTeleportMaxNoLoadDist(SStrToFloat(value));
    return true;
}

// ref: FUN_14135c6e0
bool DynamicLodCallback(CVar*, const char*, const char* value, void*) {
    unsigned long on = ParseULong(value);
    if (1 < on) {
        ConsoleWriteText("dynamicLod value must be 0 or 1");
        return false;
    }
    ConsoleWriteText(on != 0 ? "dynamicLod enabled" : "dynamicLod disabled");
    return true;
}

// ref: FUN_1413648d0
bool WorldPreloadSortCallback(CVar*, const char*, const char* value, void*) {
    ConsoleWriteText(ParseLong(value) == 0 ? "World preload object sort disabled." : "World preload object sort enabled.");
    return true;
}

// ref: FUN_1413647b0
bool WorldLoadSortCallback(CVar*, const char*, const char* value, void*) {
    ConsoleWriteText(ParseLong(value) == 0 ? "World load object sort disabled." : "World load object sort enabled.");
    return true;
}

// ref: FUN_141364870
bool WorldPreloadNonCriticalCallback(CVar*, const char*, const char* value, void*) {
    ConsoleWriteText(ParseLong(value) == 0 ? "World preload non critical disabled." : "World preload non critical enabled.");
    return true;
}

// ref: FUN_141364810
bool WorldPreloadHighResTexturesCallback(CVar*, const char*, const char* value, void*) {
    ConsoleWriteText(ParseLong(value) == 0 ? "World preload high res textures disabled."
                                           : "World preload high res textures enabled.");
    return true;
}

// Each group's UI value gets the coded default its quality level setting gives it.
// ref: FUN_141363610
void ApplyGraphicsQualityDefaults(bool raid) {
    CVar* quality = CVarLookup(t2util::ConstFnvHashStringI(raid ? "RAIDgraphicsQuality" : "graphicsQuality"));
    if (quality == nullptr) {
        return;
    }
    long level = ParseLong(quality->m_defaultValue.GetString());
    for (const auto& group : s_groups[raid ? 1 : 0]) {
        CVar* cvar = CVarLookup(t2util::ConstFnvHashStringI(group.first.c_str()));
        if (cvar != nullptr) {
            CVarSetDefaultInt(cvar, GraphicsGroupSetting(group.first.c_str(), level, raid));
        }
    }
}

CVar* s_gxMTDisableCVar;
CVar* s_gxMTBeginDrawCVar;
CVar* s_gxMTShadowCVar;
CVar* s_gxMTPrepassCVar;
CVar* s_gxMTOpaqueM2CVar;
CVar* s_gxMTOpaqueM2NoReflectCVar;
CVar* s_gxMTAlphaM2CVar;
CVar* s_gxMTOpaqueWMOCVar;
CVar* s_gxMTTerrainCVar;
CVar* s_gxMTParticulateVolumesCVar;
CVar* s_gxMTAlphaWaterCVar;
CVar* s_dnMTUpdateCVar;
CVar* s_farclipCVar;
CVar* s_nearclipCVar;
CVar* s_horizonClipCVar;
CVar* s_horizonStartCVar;
CVar* s_particleDensityCVar;
CVar* s_particleMTDensityCVar;
CVar* s_waterDetailCVar;
CVar* s_rippleDetailCVar;
CVar* s_reflectionModeCVar;
CVar* s_reflectionDownscaleCVar;
CVar* s_sunShaftsCVar;
CVar* s_groundEffectDensityCVar;
CVar* s_groundEffectFadeCVar;
CVar* s_groundEffectDistCVar;
CVar* s_groundEffectAnimationCVar;
CVar* s_projectedTexturesCVar;
CVar* s_spellClutterCVar;
CVar* s_shadowModeCVar;
CVar* s_shadowTextureSizeCVar;
CVar* s_shadowSoftCVar;
CVar* s_shadowBlendCascadesCVar;
CVar* s_shadowRtCVar;
CVar* s_maxLightCountCVar;
CVar* s_maxLightDistCVar;
CVar* s_sSAOCVar;
CVar* s_sSAOTypeCVar;
CVar* s_preloadStreamingDistTerrainCVar;
CVar* s_preloadStreamingDistObjectCVar;
CVar* s_preloadLoadingDistTerrainCVar;
CVar* s_preloadLoadingDistObjectCVar;
CVar* s_skyCloudLODCVar;
CVar* s_textureFilteringModeCVar;
CVar* s_terrainLodDistCVar;
CVar* s_terrainLodDivCVar;
CVar* s_wmoLodDistCVar;
CVar* s_wmoLodDistScaleCVar;
CVar* s_wmoDoodadDistCVar;
CVar* s_wmoPortalFadeScaleCVar;
CVar* s_wmoPortalInteriorFadeCVar;
CVar* s_entityLodDistCVar;
CVar* s_entityLodOffsetCVar;
CVar* s_entityShadowFadeScaleCVar;
CVar* s_doodadLodScaleCVar;
CVar* s_terrainMipLevelCVar;
CVar* s_worldMaxMipLevelCVar;
CVar* s_worldBaseMipCVar;
CVar* s_occlusionMaxJobsCVar;
CVar* s_lowLatencyModeCVar;
CVar* s_minimapPortalMaxCVar;
CVar* s_worldViewCullMaxJobsCVar;
CVar* s_worldIntersectMaxJobsCVar;
CVar* s_locateViewerMaxJobsCVar;
CVar* s_lodObjectCullSizeCVar;
CVar* s_lodObjectCullDistCVar;
CVar* s_lodObjectMinSizeCVar;
CVar* s_lodObjectFadeScaleCVar;
CVar* s_lodObjectSizeScaleCVar;
CVar* s_rAIDsettingsEnabledCVar;
CVar* s_rAIDfarclipCVar;
CVar* s_rAIDWaterDetailCVar;
CVar* s_rAIDSSAOCVar;
CVar* s_rAIDgroundEffectDensityCVar;
CVar* s_rAIDgroundEffectFadeCVar;
CVar* s_rAIDgroundEffectDistCVar;
CVar* s_rAIDgroundEffectAnimationCVar;
CVar* s_rAIDshadowModeCVar;
CVar* s_rAIDshadowRtCVar;
CVar* s_rAIDterrainLodDistCVar;
CVar* s_rAIDTerrainLodDivCVar;
CVar* s_rAIDwmoLodDistCVar;
CVar* s_rAIDhorizonStartCVar;
CVar* s_rAIDhorizonClipCVar;
CVar* s_rAIDdoodadLodScaleCVar;
CVar* s_rAIDentityLodDistCVar;
CVar* s_rAIDentityShadowFadeScaleCVar;
CVar* s_rAIDterrainMipLevelCVar;
CVar* s_rAIDworldBaseMipCVar;
CVar* s_rAIDprojectedTexturesCVar;
CVar* s_rAIDspellClutterCVar;
CVar* s_rAIDshadowTextureSizeCVar;
CVar* s_rAIDshadowSoftCVar;
CVar* s_rAIDshadowBlendCascadesCVar;
CVar* s_rAIDreflectionModeCVar;
CVar* s_rAIDrippleDetailCVar;
CVar* s_rAIDsunShaftsCVar;
CVar* s_rAIDparticleDensityCVar;
CVar* s_rAIDparticleMTDensityCVar;
CVar* s_rAIDlodObjectCullSizeCVar;
CVar* s_rAIDlodObjectCullDistCVar;
CVar* s_rAIDlodObjectMinSizeCVar;
CVar* s_rAIDlodObjectFadeScaleCVar;
CVar* s_graphicsTextureResolutionCVar;
CVar* s_graphicsSpellDensityCVar;
CVar* s_graphicsProjectedTexturesCVar;
CVar* s_graphicsEnvironmentDetailCVar;
CVar* s_graphicsGroundClutterCVar;
CVar* s_graphicsShadowQualityCVar;
CVar* s_graphicsLiquidDetailCVar;
CVar* s_graphicsParticleDensityCVar;
CVar* s_graphicsSSAOCVar;
CVar* s_graphicsSunshaftsCVar;
CVar* s_raidGraphicsTextureResolutionCVar;
CVar* s_raidGraphicsSpellDensityCVar;
CVar* s_raidGraphicsProjectedTexturesCVar;
CVar* s_raidGraphicsEnvironmentDetailCVar;
CVar* s_raidGraphicsGroundClutterCVar;
CVar* s_raidGraphicsShadowQualityCVar;
CVar* s_raidGraphicsLiquidDetailCVar;
CVar* s_raidGraphicsParticleDensityCVar;
CVar* s_raidGraphicsSSAOCVar;
CVar* s_raidGraphicsSunshaftsCVar;
CVar* s_teleportMaxNoLoadDistCVar;

}

// The SSAO type check, for the ambient occlusion CVars.
uint32_t GraphicsCheckSsaoType(int32_t type) {
    return CheckSsaoType(type);
}

// ref: FUN_14134e340
int32_t GraphicsSettingsMode() {
    return g_worldGraphicsMode;
}

// ref: FUN_14135ce70
const GRAPHICSLEVELVALUE* GraphicsLevelValues(uint32_t level, bool raid) {
    return s_levelValues[raid ? 1 : 0][level];
}

// ref: FUN_14135c860
int32_t GraphicsLevelValue(const char* cvar, uint32_t level, bool raid) {
    for (const GRAPHICSLEVELVALUE* entry = GraphicsLevelValues(level, raid); entry->m_name != nullptr; entry++) {
        if (_stricmp(entry->m_name, cvar) == 0) {
            return entry->m_value;
        }
    }
    return 0;
}

// The members' values level by level: every level that changes one counts as a UI step.
// ref: FUN_14135cba0
int32_t GraphicsGroupLevel(const char* group, int32_t setting, bool raid) {
    const std::vector<std::string>* members = FindGroup(group, raid);
    if (members == nullptr) {
        return 0;
    }
    std::vector<int32_t> values(members->size(), -1);
    int32_t level = 0;
    for (; level < 10; level++) {
        bool changed = false;
        for (size_t i = 0; i < members->size(); i++) {
            int32_t value = GraphicsLevelValue((*members)[i].c_str(), static_cast<uint32_t>(level), raid);
            if (values[i] != value) {
                values[i] = value;
                changed = true;
            }
        }
        if (changed && --setting < 0) {
            break;
        }
    }
    return level;
}

// ref: FUN_14135c8d0
int32_t GraphicsGroupSetting(const char* group, int32_t level, bool raid) {
    const std::vector<std::string>* members = FindGroup(group, raid);
    if (members == nullptr) {
        return 0;
    }
    std::vector<int32_t> values(members->size(), -1);
    int32_t setting = -1;
    int32_t result = -1;
    for (int32_t i = 0; i <= level; i++) {
        bool changed = false;
        for (size_t m = 0; m < members->size(); m++) {
            int32_t value = GraphicsLevelValue((*members)[m].c_str(), static_cast<uint32_t>(i), raid);
            if (values[m] != value) {
                values[m] = value;
                changed = true;
            }
        }
        if (!members->empty()) {
            if (changed) {
                setting++;
            }
            result = setting;
        }
    }
    return result;
}

// ref: FUN_1413627a0
uint8_t GraphicsSettingValidity(const char* group, int32_t setting, bool raid) {
    const std::vector<std::string>* members = FindGroup(group, raid);
    if (members == nullptr) {
        return ')';
    }
    int32_t level = GraphicsGroupLevel(group, setting, raid);
    if (level >= 10) {
        return 1;
    }
    for (const std::string& member : *members) {
        int32_t value = GraphicsLevelValue(member.c_str(), static_cast<uint32_t>(level), raid);
        for (const GRAPHICSVALIDITY& check : s_validity) {
            if (_stricmp(check.m_name, member.c_str()) == 0 || _stricmp(check.m_raidName, member.c_str()) == 0) {
                uint8_t why = static_cast<uint8_t>(check.m_check(value));
                if (why != 0) {
                    return why;
                }
                break;
            }
        }
    }
    return 0;
}

// ref: FUN_141363b80
void GraphicsSetRenderScaleDefault(float (*getter)()) {
    s_renderScaleDefault = getter;
}

// ref: FUN_14135db10
void GraphicsRegisterCVars() {
    AddGroup(false, "graphicsTextureResolution", { "terrainMipLevel", "componentTextureLevel", "worldBaseMip" });
    AddGroup(false, "graphicsSpellDensity", { "spellClutter" });
    AddGroup(false, "graphicsProjectedTextures", { "projectedTextures" });
    AddGroup(false, "graphicsGroundClutter", { "groundEffectDist", "groundEffectDensity", "groundEffectAnimation" });
    AddGroup(false, "graphicsEnvironmentDetail", { "lodObjectFadeScale", "lodObjectCullSize", "lodObjectCullDist", "lodObjectMinSize", "doodadLodScale" });
    AddGroup(false, "graphicsShadowQuality", { "shadowMode", "shadowTextureSize", "shadowSoft", "shadowBlendCascades" });
    AddGroup(false, "graphicsLiquidDetail", { "waterDetail", "reflectionMode", "rippleDetail" });
    AddGroup(false, "graphicsParticleDensity", { "particleDensity", "particleMTDensity", "weatherDensity" });
    AddGroup(false, "graphicsSSAO", { "ssao" });
    AddGroup(false, "graphicsSunshafts", { "sunshafts" });
    AddGroup(true, "raidGraphicsTextureResolution", { "RAIDterrainMipLevel", "RAIDcomponentTextureLevel", "RAIDworldBaseMip" });
    AddGroup(true, "raidGraphicsSpellDensity", { "RAIDspellClutter" });
    AddGroup(true, "raidGraphicsProjectedTextures", { "RAIDprojectedTextures" });
    AddGroup(true, "raidGraphicsViewDistance", { "RAIDfarClip", "RAIDhorizonClip", "RAIDhorizonStart", "RAIDwmoLodDist", "RAIDterrainLodDist", "RAIDterrainLodDiv", "RAIDentityLodDist", "RAIDentityShadowFadeScale" });
    AddGroup(true, "raidGraphicsGroundClutter", { "RAIDgroundEffectDist", "RAIDgroundEffectDensity", "RAIDgroundEffectAnimation" });
    AddGroup(true, "raidGraphicsEnvironmentDetail", { "RAIDlodObjectFadeScale", "RAIDlodObjectCullSize", "RAIDlodObjectCullDist", "RAIDlodObjectMinSize", "RAIDdoodadLodScale" });
    AddGroup(true, "raidGraphicsShadowQuality", { "RAIDshadowMode", "RAIDshadowTextureSize", "RAIDshadowSoft", "RAIDshadowBlendCascades" });
    AddGroup(true, "raidGraphicsLiquidDetail", { "RAIDwaterDetail", "RAIDreflectionMode", "RAIDrippleDetail" });
    AddGroup(true, "raidGraphicsParticleDensity", { "RAIDparticleDensity", "RAIDparticleMTDensity", "RAIDweatherDensity" });
    AddGroup(true, "raidGraphicsSSAO", { "RAIDSSAO" });
    AddGroup(true, "raidGraphicsSunshafts", { "RAIDsunshafts" });
    s_gxMTDisableCVar = CVarRegister("gxMTDisable", "Disable all render multithreading", 1, "0", GxMTDisableCallback, 1, false, nullptr, false);
    s_gxMTBeginDrawCVar = CVarRegister("gxMTBeginDraw", "Do BeginDraw multithreaded.", 1, "1", GxMTBeginDrawCallback, 1, false, nullptr, false);
    s_gxMTShadowCVar = CVarRegister("gxMTShadow", "Render shadow bands in parallel.", 1, "1", GxMTShadowCallback, 1, false, nullptr, false);
    s_gxMTPrepassCVar = CVarRegister("gxMTPrepass", "Render prepass in parallel.", 1, "1", GxMTPrepassCallback, 1, false, nullptr, false);
    s_gxMTOpaqueM2CVar = CVarRegister("gxMTOpaqueM2", "Render opaque model pass in parallel.", 1, "1", GxMTOpaqueM2Callback, 1, false, nullptr, false);
    s_gxMTOpaqueM2NoReflectCVar = CVarRegister("gxMTOpaqueM2NoReflect", "Render opaque model no reflection pass in parallel.", 1, "1", GxMTOpaqueM2NoReflectCallback, 1, false, nullptr, false);
    s_gxMTAlphaM2CVar = CVarRegister("gxMTAlphaM2", "Render transparent M2 pass in parallel.", 1, "1", GxMTAlphaM2Callback, 1, false, nullptr, false);
    s_gxMTOpaqueWMOCVar = CVarRegister("gxMTOpaqueWMO", "Render opaque WMO in parallel.", 1, "1", GxMTOpaqueWMOCallback, 1, false, nullptr, false);
    s_gxMTTerrainCVar = CVarRegister("gxMTTerrain", "Render terrain in parallel.", 1, "1", GxMTTerrainCallback, 1, false, nullptr, false);
    s_gxMTParticulateVolumesCVar = CVarRegister("gxMTParticulateVolumes", "Render Particulate Volumes in parallel.", 1, "0", GxMTParticulateVolumesCallback, 1, false, nullptr, false);
    s_gxMTAlphaWaterCVar = CVarRegister("gxMTAlphaWater", "Render Alpha Water Volumes in parallel.", 1, "1", GxMTAlphaWaterCallback, 1, false, nullptr, false);
    s_dnMTUpdateCVar = CVarRegister("dnMTUpdate", "Update Daynight in parralel.", 1, "1", DnMTUpdateCallback, 1, false, nullptr, false);
    s_farclipCVar = CVarRegister("farclip", "Far clip plane distance", 1, "5108", FarclipCallback, 1, false, nullptr, false);
    s_nearclipCVar = CVarRegister("nearclip", "Near clip plane distance", 1, "0.2", NearclipCallback, 1, false, nullptr, false);
    s_horizonClipCVar = CVarRegister("horizonClip", "Horizon end distance", 1, "1600", HorizonClipCallback, 1, false, nullptr, false);
    s_horizonStartCVar = CVarRegister("horizonStart", "Horizon start distance", 1, "1277", HorizonStartCallback, 1, false, nullptr, false);
    s_particleDensityCVar = CVarRegister("particleDensity", "Particle density", 1, "33", ParticleDensityCallback, 1, false, nullptr, false);
    s_particleMTDensityCVar = CVarRegister("particleMTDensity", "Multi-Tex particle density", 1, "33", ParticleMTDensityCallback, 1, false, nullptr, false);
    s_waterDetailCVar = CVarRegister("waterDetail", "Water surface detail", 1, "0", WaterDetailCallback, 1, false, nullptr, false);
    s_rippleDetailCVar = CVarRegister("rippleDetail", "Ripple surface detail", 1, "2", RippleDetailCallback, 1, false, nullptr, false);
    s_reflectionModeCVar = CVarRegister("reflectionMode", "Reflection mode", 1, "3", ReflectionModeCallback, 1, false, nullptr, false);
    s_reflectionDownscaleCVar = CVarRegister("reflectionDownscale", "Reflection downscale", 1, "0", ReflectionDownscaleCallback, 1, false, nullptr, false);
    s_sunShaftsCVar = CVarRegister("sunShafts", "SunShafts", 1, "0", SunShaftsCallback, 1, false, nullptr, false);
    s_groundEffectDensityCVar = CVarRegister("groundEffectDensity", "Ground effect density", 1, "16", GroundEffectDensityCallback, 1, false, nullptr, false);
    s_groundEffectFadeCVar = CVarRegister("groundEffectFade", "Ground effect fade", 1, "70", GroundEffectFadeCallback, 1, false, nullptr, false);
    s_groundEffectDistCVar = CVarRegister("groundEffectDist", "Ground effect dist", 1, "70", GroundEffectDistCallback, 1, false, nullptr, false);
    s_groundEffectAnimationCVar = CVarRegister("groundEffectAnimation", "Ground effect animation", 1, "0", GroundEffectAnimationCallback, 1, false, nullptr, false);
    s_projectedTexturesCVar = CVarRegister("projectedTextures", "Projected Textures", 1, "0", ProjectedTexturesCallback, 1, false, nullptr, false);
    s_spellClutterCVar = CVarRegister("spellClutter", "Cull unimportant spell effects. -1 means auto based on targetFPS otherwise [0-100], 0 means cull nothing for perf reasons, 100 means cull as much as you can", 1, "-1", SpellClutterCallback, 1, false, nullptr, false);
    s_shadowModeCVar = CVarRegister("shadowMode", "Quality of shadows (0-3)", 1, "0", ShadowModeCallback, 1, false, nullptr, false);
    s_shadowTextureSizeCVar = CVarRegister("shadowTextureSize", "Shadow texture size (1024-2048)", 1, "1024", ShadowTextureSizeCallback, 1, false, nullptr, false);
    s_shadowSoftCVar = CVarRegister("shadowSoft", "Soft shadows (0/1)", 1, "0", ShadowSoftCallback, 1, false, nullptr, false);
    s_shadowBlendCascadesCVar = CVarRegister("shadowBlendCascades", "Blend between shadow cascades (0/1)", 1, "0", ShadowBlendCascadesCallback, 1, false, nullptr, false);
    s_shadowRtCVar = CVarRegister("shadowRt", "Raytraced shadows (0-3)", 1, "0", ShadowRtCallback, 1, false, nullptr, false);
    s_maxLightCountCVar = CVarRegister("maxLightCount", "Maximum lights to render", 1, "128", MaxLightCountCallback, 1, false, nullptr, false);
    s_maxLightDistCVar = CVarRegister("maxLightDist", "Maximum distance to render lights", 1, "2048", MaxLightDistCallback, 1, false, nullptr, false);
    s_sSAOCVar = CVarRegister("SSAO", "Screen-Space Ambient Occlusion", 1, "0", SSAOCallback, 1, false, nullptr, false);
    s_sSAOTypeCVar = CVarRegister("SSAOType", "Screen-Space Ambient Occlusion Type", 1, "0", SSAOTypeCallback, 1, false, nullptr, false);
    s_preloadStreamingDistTerrainCVar = CVarRegister("preloadStreamingDistTerrain", "Terrain preload distance when streaming", 1, "256", nullptr, 5, false, nullptr, false);
    s_preloadStreamingDistObjectCVar = CVarRegister("preloadStreamingDistObject", "Object preload distance when streaming", 1, "64", nullptr, 5, false, nullptr, false);
    s_preloadLoadingDistTerrainCVar = CVarRegister("preloadLoadingDistTerrain", "Terrain preload distance when loading", 1, "1024", nullptr, 5, false, nullptr, false);
    s_preloadLoadingDistObjectCVar = CVarRegister("preloadLoadingDistObject", "Object preload distance when loading", 1, "512", nullptr, 5, false, nullptr, false);
    s_skyCloudLODCVar = CVarRegister("SkyCloudLOD", "Texture resolution for clouds", 1, "0", SkyCloudLODCallback, 1, false, nullptr, false);
    s_textureFilteringModeCVar = CVarRegister("textureFilteringMode", "Texture filtering mode", 1, "5", TextureFilteringModeCallback, 1, false, nullptr, false);
    s_terrainLodDistCVar = CVarRegister("terrainLodDist", "Terrain level of detail distance", 1, "500", TerrainLodDistCallback, 1, false, nullptr, false);
    s_terrainLodDivCVar = CVarRegister("TerrainLodDiv", "Terrain lod divisor", 1, "768", TerrainLodDivCallback, 1, false, nullptr, false);
    s_wmoLodDistCVar = CVarRegister("wmoLodDist", "Wmo level of detail distance", 1, "400", WmoLodDistCallback, 1, false, nullptr, false);
    s_wmoLodDistScaleCVar = CVarRegister("wmoLodDistScale", "Wmo level of detail distance scale", 1, "1.0", WmoLodDistScaleCallback, 1, false, nullptr, false);
    s_wmoDoodadDistCVar = CVarRegister("wmoDoodadDist", "Wmo doodad load distance", 1, "2000", WmoDoodadDistCallback, 1, false, nullptr, false);
    s_wmoPortalFadeScaleCVar = CVarRegister("wmoPortalFadeScale", "Wmo portal fade scale", 1, "1000", WmoPortalFadeScaleCallback, 1, false, nullptr, false);
    s_wmoPortalInteriorFadeCVar = CVarRegister("wmoPortalInteriorFade", "Wmo portal interior fade", 1, "0", WmoPortalInteriorFadeCallback, 1, false, nullptr, false);
    s_entityLodDistCVar = CVarRegister("entityLodDist", "Entity level of detail distance", 1, "10", EntityLodDistCallback, 1, false, nullptr, false);
    s_entityLodOffsetCVar = CVarRegister("entityLodOffset", "Entity level of detail offset", 1, "10", EntityLodOffsetCallback, 1, false, nullptr, false);
    s_entityShadowFadeScaleCVar = CVarRegister("entityShadowFadeScale", "Entity shadow fade scale", 1, "25", EntityShadowFadeScaleCallback, 1, false, nullptr, false);
    s_doodadLodScaleCVar = CVarRegister("doodadLodScale", "Doodad level of detail scale", 1, "100", DoodadLodScaleCallback, 1, false, nullptr, false);
    s_terrainMipLevelCVar = CVarRegister("terrainMipLevel", "Terrain blend map mip level", 1, "0", TerrainMipLevelCallback, 1, false, nullptr, false);
    s_worldMaxMipLevelCVar = CVarRegister("worldMaxMipLevel", "World maximum texture mip level", 1, "12", WorldMaxMipLevelCallback, 1, false, nullptr, false);
    s_worldBaseMipCVar = CVarRegister("worldBaseMip", "World texture base mip", 1, "1", WorldBaseMipCallback, 1, false, nullptr, false);
    s_occlusionMaxJobsCVar = CVarRegister("occlusionMaxJobs", "Maximum job threads for occlusion render", 1, "5", OcclusionMaxJobsCallback, 1, false, nullptr, false);
    s_lowLatencyModeCVar = CVarRegister("LowLatencyMode", "0=None, 1=BuiltIn, 2=Reflex", 1, "0", LowLatencyModeCallback, 1, false, nullptr, false);
    s_minimapPortalMaxCVar = CVarRegister("minimapPortalMax", "Max Number of Portals to traverse for minimap", 1, "99", nullptr, 4, false, nullptr, false);
    s_worldViewCullMaxJobsCVar = CVarRegister("worldViewCullMaxJobs", "Maximum job threads for culling", 1, "32", WorldViewCullMaxJobsCallback, 1, false, nullptr, false);
    s_worldIntersectMaxJobsCVar = CVarRegister("worldIntersectMaxJobs", "Maximum job threads for culling", 1, "32", WorldIntersectMaxJobsCallback, 1, false, nullptr, false);
    s_locateViewerMaxJobsCVar = CVarRegister("locateViewerMaxJobs", "Maximum job threads for LocateViewer", 1, "32", LocateViewerMaxJobsCallback, 1, false, nullptr, false);
    char renderScale[0x20];
    SStrPrintf(renderScale, sizeof(renderScale), "%f",
        static_cast<double>(s_renderScaleDefault != nullptr ? s_renderScaleDefault() : 1.0f));
    CVarRegister("RenderScale", "Render scale (for supersampling or undersampling)", 1, renderScale, RenderScaleCallback, 1, false, nullptr, false);
    CVarRegister("ResampleQuality", "Resample quality", 1, "3", ResampleQualityCallback, 1, false, nullptr, false);
    CVarRegister("MSAAQuality", "Multisampling AA quality", 1, "0", MSAAQualityCallback, 1, false, nullptr, false);
    CVarRegister("MSAAAlphaTest", "Enable MSAA for alpha-tested geometry", 1, CheckMsaaAlphaTest(1) == 0 ? "1" : "0", MSAAAlphaTestCallback, 1, false, nullptr, false);
    CVarRegister("specular", "Specular lighting multiplier (0-1)", 1, "1", SpecularCallback, 1, false, nullptr, false);
    CVarRegister("vrsWorldGeo", "Render scale like effect for terrain, buildings and liquids", 1, "1x1", VrsWorldGeoCallback, 1, false, nullptr, false);
    CVarRegister("vrsParticles", "Render scale like effect for particles. Only used if lots of particles are on screen", 1, "1x1", VrsParticlesCallback, 1, false, nullptr, false);
    CVarRegister("vrsValar", "Generate a shading rate mask based on velocity and luminance. Requires VRS Tier 2.", 1, "0", VrsValarCallback, 1, false, nullptr, false);
    s_lodObjectCullSizeCVar = CVarRegister("lodObjectCullSize", "Lod object culling size", 1, "15", LodObjectCullSizeCallback, 1, false, nullptr, false);
    s_lodObjectCullDistCVar = CVarRegister("lodObjectCullDist", "Lod object culling dist minimum", 1, "30", LodObjectCullDistCallback, 1, false, nullptr, false);
    s_lodObjectMinSizeCVar = CVarRegister("lodObjectMinSize", "Lod object min size", 1, "20", LodObjectMinSizeCallback, 1, false, nullptr, false);
    s_lodObjectFadeScaleCVar = CVarRegister("lodObjectFadeScale", "Lod object fade scale", 1, "100", LodObjectFadeScaleCallback, 1, false, nullptr, false);
    s_lodObjectSizeScaleCVar = CVarRegister("lodObjectSizeScale", "Scales all objects size for culling", 0x400, "1", LodObjectSizeScaleCallback, 1, false, nullptr, false);
    s_rAIDsettingsEnabledCVar = CVarRegister("RAIDsettingsEnabled", "Raid graphic settings are available", 1, "0", nullptr, 1, false, nullptr, false);
    s_rAIDfarclipCVar = CVarRegister("RAIDfarclip", "Raid Far clip plane distance", 1, "2112", FarclipCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDWaterDetailCVar = CVarRegister("RAIDWaterDetail", "Raid Water surface detail", 1, "0", WaterDetailCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDSSAOCVar = CVarRegister("RAIDSSAO", "Raid Screen-Space Ambient Occlusion", 1, "0", SSAOCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDgroundEffectDensityCVar = CVarRegister("RAIDgroundEffectDensity", "Raid Ground effect density", 1, "16", GroundEffectDensityCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDgroundEffectFadeCVar = CVarRegister("RAIDgroundEffectFade", "Raid Ground effect fade", 1, "70", GroundEffectFadeCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDgroundEffectDistCVar = CVarRegister("RAIDgroundEffectDist", "Raid Ground effect dist", 1, "70", GroundEffectDistCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDgroundEffectAnimationCVar = CVarRegister("RAIDgroundEffectAnimation", "Raid Ground effect animation", 1, "0", GroundEffectAnimationCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDshadowModeCVar = CVarRegister("RAIDshadowMode", "Raid Quality of shadows (0-3)", 1, "0", ShadowModeCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDshadowRtCVar = CVarRegister("RAIDshadowRt", "Raid Raytraced shadows (0-2)", 1, "0", ShadowRtCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDterrainLodDistCVar = CVarRegister("RAIDterrainLodDist", "Raid Terrain level of detail distance", 1, "500", TerrainLodDistCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDTerrainLodDivCVar = CVarRegister("RAIDTerrainLodDiv", "Raid Terrain lod divisor", 1, "768", TerrainLodDivCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDwmoLodDistCVar = CVarRegister("RAIDwmoLodDist", "Raid Wmo level of detail distance", 1, "400", WmoLodDistCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDhorizonStartCVar = CVarRegister("RAIDhorizonStart", "Raid Horizon start distance", 1, "777", HorizonStartCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDhorizonClipCVar = CVarRegister("RAIDhorizonClip", "Raid Horizon end distance", 1, "1600", HorizonClipCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDdoodadLodScaleCVar = CVarRegister("RAIDdoodadLodScale", "Raid doodad level of detail scale", 1, "100", DoodadLodScaleCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDentityLodDistCVar = CVarRegister("RAIDentityLodDist", "Raid Entity level of detail distance", 1, "10", EntityLodDistCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDentityShadowFadeScaleCVar = CVarRegister("RAIDentityShadowFadeScale", "Raid Entity shadow fade scale", 1, "25", EntityShadowFadeScaleCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDterrainMipLevelCVar = CVarRegister("RAIDterrainMipLevel", "Terrain blend map mip level", 1, "0", TerrainMipLevelCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDworldBaseMipCVar = CVarRegister("RAIDworldBaseMip", "World texture base mip", 1, "1", WorldBaseMipCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDprojectedTexturesCVar = CVarRegister("RAIDprojectedTextures", "Projected Textures", 1, "0", ProjectedTexturesCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDspellClutterCVar = CVarRegister("RAIDspellClutter", "Cull unimportant spell effects. -1 means auto based on targetFPS otherwise [0-100], 0 means cull nothing for perf reasons, 100 means cull as much as you can", 1, "-1", SpellClutterCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDshadowTextureSizeCVar = CVarRegister("RAIDshadowTextureSize", "Shadow texture size (1024-2048)", 1, "1024", ShadowTextureSizeCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDshadowSoftCVar = CVarRegister("RAIDshadowSoft", "Soft shadows (0/1)", 1, "0", ShadowSoftCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDshadowBlendCascadesCVar = CVarRegister("RAIDshadowBlendCascades", "Blend between shadow cascades (0/1)", 1, "0", ShadowBlendCascadesCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDreflectionModeCVar = CVarRegister("RAIDreflectionMode", "Reflection mode", 1, "3", ReflectionModeCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDrippleDetailCVar = CVarRegister("RAIDrippleDetail", "Ripple surface detail", 1, "2", RippleDetailCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDsunShaftsCVar = CVarRegister("RAIDsunShafts", "SunShafts", 1, "0", SunShaftsCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDparticleDensityCVar = CVarRegister("RAIDparticleDensity", "Particle density", 1, "33", ParticleDensityCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDparticleMTDensityCVar = CVarRegister("RAIDparticleMTDensity", "Multi-Tex particle density", 1, "33", ParticleMTDensityCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDlodObjectCullSizeCVar = CVarRegister("RAIDlodObjectCullSize", "Lod object culling size", 1, "15", LodObjectCullSizeCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDlodObjectCullDistCVar = CVarRegister("RAIDlodObjectCullDist", "Lod object culling dist minimum", 1, "30", LodObjectCullDistCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDlodObjectMinSizeCVar = CVarRegister("RAIDlodObjectMinSize", "Lod object min size", 1, "20", LodObjectMinSizeCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_rAIDlodObjectFadeScaleCVar = CVarRegister("RAIDlodObjectFadeScale", "Lod object fade scale", 1, "100", LodObjectFadeScaleCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_graphicsTextureResolutionCVar = CVarRegister("graphicsTextureResolution", "UI value of the graphics setting", 0x1800, "2", GraphicsTextureResolutionCallback, 1, false, nullptr, false);
    s_graphicsSpellDensityCVar = CVarRegister("graphicsSpellDensity", "UI value of the graphics setting", 0x1800, "5", GraphicsTextureResolutionCallback, 1, false, nullptr, false);
    s_graphicsProjectedTexturesCVar = CVarRegister("graphicsProjectedTextures", "UI value of the graphics setting", 0x1800, "1", GraphicsTextureResolutionCallback, 1, false, nullptr, false);
    s_graphicsEnvironmentDetailCVar = CVarRegister("graphicsEnvironmentDetail", "UI value of the graphics setting", 0x1800, "6", GraphicsTextureResolutionCallback, 1, false, nullptr, false);
    s_graphicsGroundClutterCVar = CVarRegister("graphicsGroundClutter", "UI value of the graphics setting", 0x1800, "6", GraphicsTextureResolutionCallback, 1, false, nullptr, false);
    s_graphicsShadowQualityCVar = CVarRegister("graphicsShadowQuality", "UI value of the graphics setting", 0x1800, "5", GraphicsTextureResolutionCallback, 1, false, nullptr, false);
    s_graphicsLiquidDetailCVar = CVarRegister("graphicsLiquidDetail", "UI value of the graphics setting", 0x1800, "3", GraphicsTextureResolutionCallback, 1, false, nullptr, false);
    s_graphicsParticleDensityCVar = CVarRegister("graphicsParticleDensity", "UI value of the graphics setting", 0x1800, "3", GraphicsTextureResolutionCallback, 1, false, nullptr, false);
    s_graphicsSSAOCVar = CVarRegister("graphicsSSAO", "UI value of the graphics setting", 0x1800, "4", GraphicsTextureResolutionCallback, 1, false, nullptr, false);
    s_graphicsSunshaftsCVar = CVarRegister("graphicsSunshafts", "UI value of the graphics setting", 0x1800, "3", GraphicsTextureResolutionCallback, 1, false, nullptr, false);
    s_raidGraphicsTextureResolutionCVar = CVarRegister("raidGraphicsTextureResolution", "UI value of the raidGraphics setting", 0x1800, "2", GraphicsTextureResolutionCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_raidGraphicsSpellDensityCVar = CVarRegister("raidGraphicsSpellDensity", "UI value of the raidGraphics setting", 0x1800, "5", GraphicsTextureResolutionCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_raidGraphicsProjectedTexturesCVar = CVarRegister("raidGraphicsProjectedTextures", "UI value of the raidGraphics setting", 0x1800, "0", GraphicsTextureResolutionCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_raidGraphicsEnvironmentDetailCVar = CVarRegister("raidGraphicsEnvironmentDetail", "UI value of the raidGraphics setting", 0x1800, "6", GraphicsTextureResolutionCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_raidGraphicsGroundClutterCVar = CVarRegister("raidGraphicsGroundClutter", "UI value of the raidGraphics setting", 0x1800, "6", GraphicsTextureResolutionCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_raidGraphicsShadowQualityCVar = CVarRegister("raidGraphicsShadowQuality", "UI value of the raidGraphics setting", 0x1800, "3", GraphicsTextureResolutionCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_raidGraphicsLiquidDetailCVar = CVarRegister("raidGraphicsLiquidDetail", "UI value of the raidGraphics setting", 0x1800, "1", GraphicsTextureResolutionCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_raidGraphicsParticleDensityCVar = CVarRegister("raidGraphicsParticleDensity", "UI value of the raidGraphics setting", 0x1800, "2", GraphicsTextureResolutionCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_raidGraphicsSSAOCVar = CVarRegister("raidGraphicsSSAO", "UI value of the raidGraphics setting", 0x1800, "2", GraphicsTextureResolutionCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_raidGraphicsSunshaftsCVar = CVarRegister("raidGraphicsSunshafts", "UI value of the raidGraphics setting", 0x1800, "1", GraphicsTextureResolutionCallback, 1, false, reinterpret_cast<void*>(1), false);
    s_teleportMaxNoLoadDistCVar = CVarRegister("teleportMaxNoLoadDist", "Max teleport distanace without preload", 1, "200", TeleportMaxNoLoadDistCallback, 1, false, nullptr, false);
    CVarRegister("dynamicLod", "Dynamic level of detail adjustment", 1, "1", DynamicLodCallback, 1, false, nullptr, false);
    RenderSettingsApply();
    CVarRegister("worldPreloadSort", "Sort objects by distance when preloading", 1, "1", WorldPreloadSortCallback, 1, false, nullptr, false);
    CVarRegister("worldLoadSort", "Sort objects by distance when loading", 1, "1", WorldLoadSortCallback, 1, false, nullptr, false);
    CVarRegister("worldPreloadNonCritical", "Require objects to be loaded in streaming non critical radius when preloading", 1, "2", WorldPreloadNonCriticalCallback, 1, false, nullptr, false);
    CVarRegister("worldPreloadHighResTextures", "Require high res textures to be loaded in streaming non critical radius when preloading", 1, "1", WorldPreloadHighResTexturesCallback, 1, false, nullptr, false);
    CVarRegister("worldPreloadNonCriticalTimeout", "World preload time (in seconds) when non-critical items are automatically ignored", 1, "45", nullptr, 1, false, nullptr, false);
    ApplyGraphicsQualityDefaults(true);
    ApplyGraphicsQualityDefaults(false);
}
