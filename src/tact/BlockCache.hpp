#ifndef TACT_BLOCK_CACHE_HPP
#define TACT_BLOCK_CACHE_HPP

// pkg/tact/3.6.39/src/async_state: the decoded bytes a span read had to decode beyond what
// it was asked for, kept for the next span of the same file (16 slots, least recently used
// replaced).

#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace tact {

struct BLOCKCACHESLOT {
    uint8_t m_ekey[16];             // 0x00
    uint32_t m_offset;              // 0x10: decoded offset
    uint32_t m_size;                // 0x14
    uint8_t* m_data;                // 0x18
    uint32_t m_capacity;            // 0x20
    uint64_t m_lastUse;             // 0x28
};
static_assert(sizeof(BLOCKCACHESLOT) == 0x30, "BLOCKCACHESLOT is 0x30 bytes");

// 0x318 bytes, at instance+0xa78.
struct BLOCKCACHE {
    SRWLOCK m_lock;                 // 0x000
    uint8_t m_enabled;              // 0x008
    BLOCKCACHESLOT m_slots[16];     // 0x010
    uint64_t m_clock;               // 0x310

    void Init();
    void Destroy();
    uint32_t Read(const uint8_t ekey[16], void* dest, uint32_t offset, uint32_t size);
    void Store(const uint8_t ekey[16], const void* data, uint32_t offset, uint32_t size);
    void Fill(BLOCKCACHESLOT* slot, const uint8_t ekey[16], const void* data, uint32_t offset, uint32_t size);
};
static_assert(offsetof(BLOCKCACHE, m_clock) == 0x310, "BLOCKCACHE layout");
static_assert(sizeof(BLOCKCACHE) == 0x318, "BLOCKCACHE is 0x318 bytes");

}

#endif
