#include "daynight/DayNight.hpp"

#include "storm/Memory.hpp"
#include <new>

namespace {

const char s_fogInfoTag[] = ".?AUFogInfo@@";
const char s_zoneLightTag[] = ".?AVCZoneLight@DayNight@@";
const char s_heightFogTag[] = ".?AUHeightFogBlendPercent@CurrentLight@DayNight@@";
const char s_currentFogTag[] = ".?AUCurrentFog@DayNight@@";

// The fogs the light state keeps (MODELFOG, FUN_1402394a0 each).
const size_t s_fogOffsets[] = {0xf8, 0x1b0, 0x250, 0x2f4, 0x394, 0x434, 0x4d4, 0x574};

// A blz::vector freed when it owns its storage (the destructor's pattern for each).
void FreeVector(uint8_t* base, size_t data, size_t capacity, const char* tag) {
    if (static_cast<int64_t>(DNField<uint64_t>(base, capacity)) >= 0) {
        SMemFree(DNField<void*>(base, data), tag, -6, 0);
        DNField<void*>(base, data) = nullptr;
    }
}

// The two fog blocks at +0x1450 and +0x14b8, the same shape (the constructor writes both).
void InitFogBlock(uint8_t* base, size_t at) {
    DNStore<uint64_t>(base, at + 0x18, 0x501502f9);
    DNStore<uint64_t>(base, at + 0x20, 0);
    DNStore<uint32_t>(base, at + 0x28, 0xc61c4000);
    DNStore<uint32_t>(base, at + 0x2c, 0);
    DNStore<uint64_t>(base, at + 0x30, 0x3f800000);
    DNStore<uint32_t>(base, at + 0x38, 0);
    DNStore<uint64_t>(base, at + 0x3c, 0x3a83126f);
    DNStore<uint32_t>(base, at + 0x44, 0);
    DNStore<uint64_t>(base, at + 0x48, 0x3f800000);
    DNStore<uint64_t>(base, at + 0x50, 0);
    DNStore<uint64_t>(base, at + 0x58, 0);
    DNStore<uint64_t>(base, at + 0x60, 0);
    DNStore<uint64_t>(base, at + 0x68, 0x3f800000);
    DNStore<uint32_t>(base, at + 0x70, 0);
    DNStore<uint8_t>(base, at + 0x74, 1);
}

}  // namespace

// ref: FUN_14244f2e0
void DNLightParamsInit(uint8_t* params) {
    std::memset(params, 0, 0x104);
}

// ref: FUN_142441dc0
void DNCurrentLightInit(uint8_t* light) {
    std::memset(light, 0, 0xb4);
    DNStore<uint64_t>(light, 0xb4, 0x3f800000);
    DNStore<uint64_t>(light, 0xbc, 0);
    DNStore<uint32_t>(light, 0xc4, 0);
    DNStore<uint8_t>(light, 0xd8, 0);
    DNStore<uint64_t>(light, 0xcc, 0);
    DNStore<float>(light, 0xd4, 1.0f);
    DNStore<uint32_t>(light, 0xdc, 0);
    DNStore<float>(light, 0xe0, 45.0f);
    // Eight 0x20-byte entries from +0xe4: a word, two quads and a byte.
    for (size_t entry = 0; entry < 8; entry++) {
        DNStore<uint32_t>(light, 0xe4 + entry * 0x20, 0);
        DNStore<uint64_t>(light, 0xe8 + entry * 0x20, 0);
        DNStore<uint64_t>(light, 0xf0 + entry * 0x20, 0);
        DNStore<uint64_t>(light, 0xf8 + entry * 0x20, 0);
        DNStore<uint8_t>(light, 0x100 + entry * 0x20, 0);
    }
    DNStore<uint32_t>(light, 0x1e4, 0);
    DNStore<uint8_t>(light, 0x1e8, 0);
    std::memset(light + 0x1ec, 0, 0x20);
    DNLightParamsInit(light + 0x20c);
    DNLightParamsInit(light + 0x310);
    DNStore<uint64_t>(light, 0x414, 0);
    DNStore<uint32_t>(light, 0x41c, 0);
    // Eight (1, 0, 0, 0, 0, 0) pairs from +0x420, then a zeroed tail.
    for (size_t entry = 0; entry < 8; entry++) {
        DNStore<uint64_t>(light, 0x420 + entry * 0x18, 0x3f800000);
        DNStore<uint64_t>(light, 0x428 + entry * 0x18, 0);
        DNStore<uint64_t>(light, 0x430 + entry * 0x18, 0);
    }
    DNStore<uint32_t>(light, 0x4d0, 0);
    DNStore<uint64_t>(light, 0x4d8, 0);
    DNStore<uint64_t>(light, 0x4e0, 0);
    DNStore<uint64_t>(light, 0x4e8, 0);
    DNStore<uint8_t>(light, 0xc8, 0);
}

// ref: FUN_1424549d0
DNLIGHT::DNLIGHT() {
    std::memset(this->m_data, 0, sizeof(this->m_data));
    for (size_t offset : s_fogOffsets) {
        new (this->m_data + offset) MODELFOG();
    }
    DNLightParamsInit(this->m_data + 0x618);
    // Two embedded vectors of five CZoneLights (their storage inline after them).
    DNStore<uint8_t*>(this, 0x730, this->m_data + 0x748);
    DNStore<uint64_t>(this, 0x740, 0x8000000000000005ull);
    DNStore<uint64_t>(this, 0x8c0, 0x8000000000000005ull);
    DNStore<uint8_t*>(this, 0x8b0, this->m_data + 0x8c8);
    DNCurrentLightInit(this->m_data + 0xa70);
    DNCurrentLightInit(this->m_data + 0xf60);
    InitFogBlock(this->m_data, 0x1450);
    DNStore<float>(this, 0x14c8, 1.0f);
    DNStore<float>(this, 0x14cc, 1.0f);
    InitFogBlock(this->m_data, 0x14b8);
    DNStore<float>(this, 0x1530, 1.0f);
    DNStore<uint64_t>(this, 0x1534, 0x3f800000);
    this->Reset();
}

// ref: FUN_142458d10
DNLIGHT::~DNLIGHT() {
    FreeVector(this->m_data, 0x1450, 0x1460, s_currentFogTag);
    FreeVector(this->m_data, 0x1438, 0x1448, s_heightFogTag);
    FreeVector(this->m_data, 0xf48, 0xf58, s_heightFogTag);
    FreeVector(this->m_data, 0x8b0, 0x8c0, s_zoneLightTag);
    FreeVector(this->m_data, 0x730, 0x740, s_zoneLightTag);
    FreeVector(this->m_data, 0x198, 0x1a8, s_fogInfoTag);
}

// ref: FUN_142454eb0
void DNLIGHT::Reset() {
    DNStore<uint32_t>(this, 0x15f4, 1);
    DNStore<uint32_t>(this, 0x0, 0);
    DNStore<uint8_t>(this, 0x4, 0);
    DNStore<uint32_t>(this, 0x15f0, 0);
    DNStore<uint16_t>(this, 0x15f8, 0);
    DNStore<uint64_t>(this, 0x8, 0);
    DNStore<uint32_t>(this, 0x10, 0);
    DNStore<uint64_t>(this, 0x40, 0);
    DNStore<uint64_t>(this, 0x48, 0);
    DNStore<uint32_t>(this, 0x54, 0);
    DNStore<uint8_t>(this, 0x58, 0);
    DNStore<uint64_t>(this, 0x60, 0);
    DNStore<uint64_t>(this, 0x68, 0);
    DNStore<uint64_t>(this, 0x70, 0);
    DNStore<uint16_t>(this, 0x78, 0);
    DNStore<uint32_t>(this, 0x90, 0);
    DNStore<uint64_t>(this, 0x80, 0);
    DNStore<uint64_t>(this, 0x88, 0);
    DNStore<uint32_t>(this, 0x9c, 0);
    DNStore<uint64_t>(this, 0x94, 0x3f800000);
    DNStore<uint64_t>(this, 0xb8, 0);
    DNStore<uint64_t>(this, 0xd0, 0);
    DNStore<uint64_t>(this, 0xc0, 0);
    DNStore<uint64_t>(this, 0xd8, 0);
    DNStore<uint64_t>(this, 0xc8, 0);
    DNStore<uint64_t>(this, 0xe0, 0);
    DNStore<uint64_t>(this, 0xe8, 0);
    DNStore<uint64_t>(this, 0xf0, 0);
    DNStore<uint32_t>(this, 0x2f0, 0);
    DNStore<uint64_t>(this, 0x724, 0);
    DNStore<uint64_t>(this, 0x738, 0);
    DNStore<uint64_t>(this, 0x8b8, 0);
    DNStore<uint8_t>(this, 0x71c, 0);
    DNStore<uint32_t>(this, 0x720, 0);
    // Each fog's mode (its word 0x16).
    DNStore<uint32_t>(this, 0x150, 1);
    DNStore<uint32_t>(this, 0x208, 5);
    DNStore<uint32_t>(this, 0x2a8, 7);
    DNStore<uint32_t>(this, 0x34c, 0);
    DNStore<uint32_t>(this, 0x3ec, 6);
    DNStore<uint32_t>(this, 0x48c, 0);
    DNStore<uint32_t>(this, 0x52c, 6);
}

// ref: FUN_142454fc0
void DNLIGHT::SetMap(int32_t mapId) {
    this->Reset();
    DNStore<int32_t>(this, 0x0, mapId);
    DNStore<uint32_t>(this, 0x15f0, 1);
    DNStore<uint8_t>(this, 0x15f8, 1);
}
