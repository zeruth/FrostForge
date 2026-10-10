#ifndef SOUND_SE3BUS_HPP
#define SOUND_SE3BUS_HPP

// The sound buses' layout (SE3Bus.cpp).

#include <cstdint>

#include "storm/list/TSList.hpp"

namespace FMOD {
class ChannelGroup;
}

struct SE3SoundKitObject;

// One bus (0x48): a SoundBus record, its channel group, and the kit instances playing on it.
struct SE3SoundBus {
    int32_t m_record;               // 0x00 the SoundBus record's id
    int8_t m_bus;                   // 0x04 the bus (SoundBus field 6)
    int8_t m_parent;                // 0x05 the parent bus, -1 for the master
    uint8_t m_muted;                // 0x06
    uint8_t m_enabled;              // 0x07
    uint8_t m_flag;                 // 0x08 SoundBus field 1, bit 0
    uint8_t m_09;                   // 0x09
    uint8_t m_pad0a[6];
    FMOD::ChannelGroup* m_group;    // 0x10
    int32_t m_18;                   // 0x18
    uint8_t m_1c;                   // 0x1C SoundBus field 5 (and the value it moves to)
    uint8_t m_1d;                   // 0x1D
    uint8_t m_pad1e[2];
    float m_volume;                 // 0x20 SoundBus field 4
    float m_targetVolume;           // 0x24
    uint8_t m_28;                   // 0x28 SoundBus field 2
    uint8_t m_29;                   // 0x29
    uint8_t m_2a;                   // 0x2A SoundBus field 3
    uint8_t m_2b;                   // 0x2B
    uint8_t m_pad2c[4];
    TSList<SE3SoundKitObject, TSGetLink<SE3SoundKitObject>, 8> m_kits;  // 0x30 linked at +0x3b8
};
static_assert(sizeof(SE3SoundBus) == 0x48, "SE3SoundBus is 0x48 bytes");

// TSGrowableArray<SE3SoundBus> (DAT_1430a96b0).
struct SE3BUSARRAY {
    uint32_t m_count;               // 0x00
    uint32_t m_pad04;
    SE3SoundBus* m_data;            // 0x08
    uint32_t m_capacity;            // 0x10
    uint32_t m_pad14;
    uint32_t m_granularity;         // 0x18
};
extern SE3BUSARRAY g_se3Buses;

// SoundBus field 1, bit 0, of a bus.
// ref: FUN_1412d7e70
bool SE3BusGetFlag(int8_t bus);
// The volume the game set for a bus (1 when it set none).
float SE3BusGetVolume(int8_t bus);

#endif
