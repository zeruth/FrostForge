#ifndef CASC_SHMEM_HPP
#define CASC_SHMEM_HPP

// pkg/casc/1.4.41/src/dynamic/shmem_control_block.cpp, shmem_control.win32.cpp and
// pid_tracking.cpp: the "shmem" file every process using a container maps. It holds a header
// (the global mutex name, the version of each index table), the free space table and the
// table of processes bound to the container.

#include "blz/Allocator.hpp"
#include "casc/Common.hpp"
#include "os/Os.hpp"
#include <cstddef>
#include <cstdint>

namespace casc {

// The header (version 5, 600 bytes in a 0x1000-byte page).
struct SHMEMHEADER {
    uint32_t m_version;             // 0x000: 4 or 5
    uint32_t m_headerSize;          // 0x004
    char m_mutexName[0x100];        // 0x008: "Global\<container path>"
    uint32_t m_freeSpaceFormat;     // 0x108: 0x2ab8
    uint32_t m_freeSpaceOffset;     // 0x10c
    uint32_t m_tableVersions[16];   // 0x110: the current version of each index table
    uint32_t m_flags;               // 0x150: bit 0 exclusive, bit 1 tables present
    uint32_t m_tableFlags;          // 0x154: bit n: extra table n present
    uint32_t m_tableSizes[32];      // 0x158
    uint32_t m_tableOffsets[32];    // 0x1d8
};
static_assert(offsetof(SHMEMHEADER, m_tableVersions) == 0x110, "SHMEMHEADER layout");
static_assert(offsetof(SHMEMHEADER, m_tableSizes) == 0x158, "SHMEMHEADER layout");
static_assert(offsetof(SHMEMHEADER, m_tableOffsets) == 0x1d8, "SHMEMHEADER layout");

// The free space table: spans of each archive that are free, as two arrays of 40-bit
// big-endian values (sizes, then offsets).
const uint32_t FREESPACE_MAX = 0x442;
struct FREESPACETABLE {
    uint32_t m_state;               // 0x00: 0 being rebuilt, 1 valid
    uint32_t m_count;               // 0x04
    uint8_t m_08[0x18];             // 0x08
    uint8_t m_sizes[FREESPACE_MAX][5];    // 0x20
    uint8_t m_offsets[FREESPACE_MAX][5];  // 0x156a
};

// The process table (0x228 bytes in a version 5 shmem file).
struct PIDTABLE {
    uint32_t m_state;               // 0x00: 1 idle, 2 being written
    uint32_t m_writers;             // 0x04: bound processes not in mode 2
    uint32_t m_count;               // 0x08
    uint32_t m_lastSlot;            // 0x0c
    uint64_t m_generation;          // 0x10
    uint32_t m_capacity;            // 0x18
    int32_t m_pidOffset;            // 0x1c: from &m_pidOffset to the pid array
    uint32_t m_capacity2;           // 0x20
    int32_t m_modeOffset;           // 0x24: from &m_modeOffset to the mode array

    uint32_t* Pids() {
        return reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(&this->m_pidOffset) + this->m_pidOffset);
    }
    int32_t* Modes() {
        return reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&this->m_modeOffset) + this->m_modeOffset);
    }
};

// What a container keeps of its binding (0x48 bytes, at container index +0x68).
struct SHMEMBINDING {
    SHMEMHEADER* m_header;          // 0x00
    FREESPACETABLE* m_freeSpace;    // 0x08
    uint64_t m_generation;          // 0x10
    uint32_t m_slot;                // 0x18: our slot in the process table, -1 when none
    uint8_t m_created;              // 0x1c: we created the file
    OSMAPPING m_mapping;            // 0x20
};
static_assert(sizeof(SHMEMBINDING) == 0x48, "SHMEMBINDING is 0x48 bytes");

// What the shmem file should look like: version, flags, extra tables.
struct SHMEMVERSION {
    uint32_t m_version;
    uint32_t m_flags;
    uint32_t m_extraTables;
};

int32_t ShmemBind(blz::allocator* allocator, const char* path, int32_t mode, STATSINK* stats, SHMEMBINDING* binding,
    bool* retry);
void ShmemUnbind(SHMEMBINDING* binding);

// The drive a path is on: 0 unknown, 1 remote, 2 fixed, 3 removable, 4 RAM.
int32_t DriveType(const char* path);

bool PidTableValid(PIDTABLE* table);
void PidTableInit(PIDTABLE* table, uint32_t size);
int32_t PidTableAdd(PIDTABLE* table, SHMEMBINDING* binding, int32_t mode);
void PidTableRemove(PIDTABLE* table, SHMEMBINDING* binding, int32_t mode);
void PidTableRecount(PIDTABLE* table);

// A free span: 40-bit offset, 32-bit size (the 9-byte records the free space rebuild sorts).
void FreeSpaceRebuild(FREESPACETABLE* table, uint8_t** spans, uint32_t count);

}

#endif
