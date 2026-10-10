#ifndef CLIENT_GRAPHICSSETTINGS_HPP
#define CLIENT_GRAPHICSSETTINGS_HPP

#include <cstdint>

// The graphics settings (Engine world options, 0x14135b000..0x141366000): the quality groups
// the options UI moves as one (graphicsShadowQuality -> shadowMode, shadowTextureSize, ...),
// the value each of the ten quality levels gives every member, which values a device can take,
// and the ~140 CVars behind them.

struct GRAPHICSLEVELVALUE {
    const char* m_name;
    int32_t m_value;
};

// The CVars (FUN_14135db10), registered with the world's startup.
void GraphicsRegisterCVars();

// Whether the world reads the raid variants (DAT_1430aaac8 == 1) (FUN_14134e340).
int32_t GraphicsSettingsMode();

// The value a quality level gives a CVar, 0 when it has none (FUN_14135c860).
int32_t GraphicsLevelValue(const char* cvar, uint32_t level, bool raid);
// The quality level a group's UI setting stands for (FUN_14135cba0), and the UI setting a
// quality level shows as (FUN_14135c8d0).
int32_t GraphicsGroupLevel(const char* group, int32_t setting, bool raid);
int32_t GraphicsGroupSetting(const char* group, int32_t level, bool raid);
// Why a group's setting cannot be had on this machine, 0 when it can; ')' for an unknown
// group, 1 for a level out of range (FUN_1413627a0).
uint8_t GraphicsSettingValidity(const char* group, int32_t setting, bool raid);
// A level's {name, value} list, null-terminated (FUN_14135ce70).
const GRAPHICSLEVELVALUE* GraphicsLevelValues(uint32_t level, bool raid);

// Where the RenderScale default comes from (FUN_141363b80).
void GraphicsSetRenderScaleDefault(float (*getter)());

#endif
