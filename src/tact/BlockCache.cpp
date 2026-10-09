#include "tact/BlockCache.hpp"
#include "tact/Common.hpp"
#include <cstring>

namespace tact {

namespace {

const char s_noTag[] = "";  // DAT_1426178ba

}

// ref: FUN_1420e5b90
void BLOCKCACHE::Init() {
    this->m_enabled = 1;
    for (auto& slot : this->m_slots) {
        slot.m_offset = 0;
        slot.m_size = 0;
        slot.m_data = nullptr;
        slot.m_capacity = 0;
        slot.m_lastUse = 0;
        std::memset(slot.m_ekey, 0, 16);
    }
    this->m_clock = 1;
    InitializeSRWLock(&this->m_lock);
}

// ref: FUN_1420e5bf0
void BLOCKCACHE::Destroy() {
    for (auto& slot : this->m_slots) {
        if (slot.m_data) {
            PackageFree(GetBufferAllocator(), slot.m_data);
        }
    }
}

// Copies what the cache holds of a file's decoded span; returns the bytes copied.
// ref: FUN_1420e5d80
uint32_t BLOCKCACHE::Read(const uint8_t ekey[16], void* dest, uint32_t offset, uint32_t size) {
    if (!this->m_enabled) {
        return 0;
    }
    AcquireSRWLockExclusive(&this->m_lock);
    uint32_t copied = 0;
    for (auto& slot : this->m_slots) {
        if (std::memcmp(slot.m_ekey, ekey, 16) != 0) {
            continue;
        }
        if (slot.m_offset <= offset && offset < slot.m_size + slot.m_offset) {
            uint32_t available = slot.m_size - (offset - slot.m_offset);
            copied = size < available ? size : available;
            if (copied != 0) {
                std::memcpy(dest, slot.m_data + (offset - slot.m_offset), copied);
            }
            slot.m_lastUse = this->m_clock++;
        }
        break;
    }
    ReleaseSRWLockExclusive(&this->m_lock);
    return copied;
}

// ref: FUN_1420e5e70
void BLOCKCACHE::Fill(BLOCKCACHESLOT* slot, const uint8_t ekey[16], const void* data, uint32_t offset, uint32_t size) {
    std::memcpy(slot->m_ekey, ekey, 16);
    slot->m_offset = offset;
    slot->m_size = size;
    if (slot->m_capacity < size) {
        if (slot->m_data) {
            PackageFree(GetBufferAllocator(), slot->m_data);
        }
        slot->m_data = static_cast<uint8_t*>(PackageAlloc(GetBufferAllocator(), size, 0x10, false, s_noTag, 0));
        slot->m_capacity = size;
    }
    std::memcpy(slot->m_data, data, size);
    slot->m_lastUse = this->m_clock++;
}

// The file's slot when it has one (touched if it already holds this span), else the first
// empty slot, else the least recently used.
// ref: FUN_1420e5c70
void BLOCKCACHE::Store(const uint8_t ekey[16], const void* data, uint32_t offset, uint32_t size) {
    if (!this->m_enabled) {
        return;
    }
    AcquireSRWLockExclusive(&this->m_lock);
    uint32_t empty = 0xFFFFFFFF;
    uint32_t oldest = 0xFFFFFFFF;
    uint64_t oldestUse = ~uint64_t(0);
    uint32_t target = 0xFFFFFFFF;
    for (uint32_t i = 0; i < 16; i++) {
        BLOCKCACHESLOT& slot = this->m_slots[i];
        if (std::memcmp(slot.m_ekey, ekey, 16) == 0) {
            if (slot.m_offset == offset && slot.m_size == size) {
                slot.m_lastUse++;
                ReleaseSRWLockExclusive(&this->m_lock);
                return;
            }
            target = i;
            break;
        }
        if (!slot.m_data) {
            if (empty == 0xFFFFFFFF) {
                empty = i;
            }
        } else if (slot.m_lastUse < oldestUse) {
            oldestUse = slot.m_lastUse;
            oldest = i;
        }
    }
    if (target == 0xFFFFFFFF) {
        target = empty != 0xFFFFFFFF ? empty : oldest;
    }
    this->Fill(&this->m_slots[target], ekey, data, offset, size);
    ReleaseSRWLockExclusive(&this->m_lock);
}

}
