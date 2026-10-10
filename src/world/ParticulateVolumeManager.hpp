#ifndef WORLD_PARTICULATE_VOLUME_MANAGER_HPP
#define WORLD_PARTICULATE_VOLUME_MANAGER_HPP

// WoW/Source/WorldClient2/ParticulateVolumeManager.cpp: CParticulateVolumeManager (0xa0), a
// scene's particulate volumes (the dust and motes map objects carry). It hands out up to 32
// volume slots from a queue of free indices, maps map object instances to their volumes, and
// registers itself in the managers' list (DAT_1430ac038).

#include "blz/Vector.hpp"
#include "os/Sync.hpp"
#include <cstddef>
#include <cstdint>

class CParticulateVolume;

extern const char s_particulateVolumePtrTag[];     // ".PEAVCParticulateVolume@@"

// blz::deque<uint32_t> as the manager builds it: blocks of sixteen, a ring of block pointers.
struct PARTICULATEINDEXQUEUE {
    uint32_t** m_blocks;            // 0x00
    uint64_t m_blockCount;          // 0x08 a power of two
    uint64_t m_first;               // 0x10
    uint64_t m_size;                // 0x18

    // The slot a value pushed at the back goes into.
    // ref: FUN_141397e00
    uint32_t* PushBack();
    // ref: FUN_141395330
    void Free();
};

// The map from map object instance to volume (blz::map, a red-black tree).
struct PARTICULATEVOLUMEMAP {
    void* m_root;                   // 0x00
    void* m_leftmost;               // 0x08
    void* m_rightmost;              // 0x10
    uint8_t m_18;                   // 0x18
    uint8_t m_pad19[7];
    uint64_t m_size;                // 0x20

    // ref: FUN_1413953a0
    void Free();
};

class CParticulateVolumeManager {
    public:
    enum : uint32_t {
        MAX_VOLUMES = 32,
    };

    blz::vector<CParticulateVolume*, s_particulateVolumePtrTag> m_volumes;   // 0x00
    uint64_t m_18;                  // 0x18 1
    uint64_t m_20[3];               // 0x20
    PARTICULATEINDEXQUEUE m_free;   // 0x38 the free slots, 0..31 to begin with
    OSMUTEX m_lock;                 // 0x58
    uint64_t m_60;                  // 0x60
    PARTICULATEVOLUMEMAP m_byInstance;  // 0x68
    void* m_buffer;                 // 0x90 a device object (FUN_142288400 releases it)
    uint64_t m_98;                  // 0x98

    // ref: FUN_141394e40
    CParticulateVolumeManager();
    // ref: FUN_141395400
    ~CParticulateVolumeManager();
};
static_assert(offsetof(CParticulateVolumeManager, m_free) == 0x38, "CParticulateVolumeManager layout");
static_assert(offsetof(CParticulateVolumeManager, m_lock) == 0x58, "CParticulateVolumeManager layout");
static_assert(offsetof(CParticulateVolumeManager, m_byInstance) == 0x68, "CParticulateVolumeManager layout");
static_assert(offsetof(CParticulateVolumeManager, m_buffer) == 0x90, "CParticulateVolumeManager layout");
static_assert(sizeof(CParticulateVolumeManager) == 0xa0, "CParticulateVolumeManager is 0xa0 bytes");

// A volume's teardown (0x1438 bytes; WoW/Source/WorldClient2/ParticulateVolume.cpp).
// ref: FUN_141407490
void ParticulateVolumeDestroy(CParticulateVolume* volume);

#endif
