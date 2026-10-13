#include "world/ParticulateVolumeManager.hpp"

#include "gx/Gx.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/Unimplemented.hpp"
#include <cstring>

const char s_particulateVolumePtrTag[] = ".PEAVCParticulateVolume@@";

namespace {

const char s_managerPtrTag[] = ".PEAVCParticulateVolumeManager@@";
const char s_blockPtrTag[] = ".PEAH";
const char s_blockTag[] = ".H";
const char s_nodeTag[] = ".?AU?$rb_node@U?$pair@QEBVCMapObjInst@@PEAVCParticulateVolume@@@blz@@@blz@@";

// DAT_1430ac038: every manager.
blz::vector<CParticulateVolumeManager*, s_managerPtrTag> s_managers;  // untied: FUN_141397d30 would walk the port's managers at the reference's offsets

struct VolumeNode {
    uint8_t m_00[8];
    VolumeNode* m_left;             // 0x08
    VolumeNode* m_right;            // 0x10
};

}  // namespace

// ref: FUN_141397750
void FreeNodes(PARTICULATEVOLUMEMAP* map, VolumeNode* node) {
    while (node != nullptr) {
        FreeNodes(map, node->m_left);
        VolumeNode* right = node->m_right;
        SMemFree(node, s_nodeTag, -6, 0);
        node = right;
    }
}


// ref: FUN_141397e00
uint32_t* PARTICULATEINDEXQUEUE::PushBack() {
    uint64_t first = this->m_first;
    if (((this->m_size + first) & 0xf) == 0 && this->m_blockCount <= (this->m_size + 0x10) >> 4) {
        uint64_t count = this->m_blockCount;
        if (count == 0) {
            count = 8;
        } else {
            uint64_t want = count + 1;
            while (count < want) {
                count *= 2;
            }
        }
        auto blocks = static_cast<uint32_t**>(
            SMemAlloc(count * 8, 0xb, s_blockPtrTag, SStrHashFnv1a(s_blockPtrTag), -6, 0));
        uint64_t start = (this->m_first >> 4) & (this->m_blockCount - 1);
        std::memcpy(blocks, this->m_blocks + start, (this->m_blockCount - start) * 8);
        std::memcpy(blocks + (this->m_blockCount - start), this->m_blocks, start * 8);
        std::memset(blocks + this->m_blockCount, 0, (count - this->m_blockCount) * 8);
        SMemFree(this->m_blocks, s_blockPtrTag, -6, 0);
        this->m_first &= 0xf;
        first = this->m_first;
        this->m_blocks = blocks;
        this->m_blockCount = count;
    }
    uint64_t index = this->m_size + first;
    this->m_size++;
    uint32_t*& block = this->m_blocks[(this->m_blockCount - 1) & (index >> 4)];
    if (block == nullptr) {
        block = static_cast<uint32_t*>(SMemAlloc(0x40, 0xb, s_blockTag, SStrHashFnv1a(s_blockTag), -6, 0));
    }
    return block + (index & 0xf);
}

// ref: FUN_141395330
void PARTICULATEINDEXQUEUE::Free() {
    for (uint64_t i = 0; i < this->m_blockCount; i++) {
        SMemFree(this->m_blocks[i], s_blockTag, -6, 0);
    }
    SMemFree(this->m_blocks, s_blockPtrTag, -6, 0);
}

// ref: FUN_1413953a0
void PARTICULATEVOLUMEMAP::Free() {
    auto node = static_cast<VolumeNode*>(this->m_root);
    while (node != nullptr) {
        FreeNodes(this, node->m_left);
        VolumeNode* right = node->m_right;
        SMemFree(node, s_nodeTag, -6, 0);
        node = right;
    }
}

// ref: FUN_141394e40
CParticulateVolumeManager::CParticulateVolumeManager() {
    this->m_18 = 1;
    this->m_20[0] = 0;
    this->m_20[1] = 0;
    this->m_20[2] = 0;
    this->m_free.m_blocks = nullptr;
    this->m_free.m_blockCount = 0;
    this->m_free.m_first = 0;
    this->m_free.m_size = 0;
    OsMutexInit(&this->m_lock);
    this->m_60 = 0;
    this->m_byInstance.m_root = nullptr;
    this->m_byInstance.m_leftmost = &this->m_byInstance.m_root;
    this->m_byInstance.m_rightmost = &this->m_byInstance.m_root;
    this->m_byInstance.m_18 = 0;
    this->m_byInstance.m_size = 0;
    this->m_buffer = nullptr;
    this->m_98 = 0;
    for (uint32_t i = 0; i < MAX_VOLUMES; i++) {
        *this->m_free.PushBack() = i;
    }
    s_managers.push_back(this);
}

// ref: FUN_141395400
CParticulateVolumeManager::~CParticulateVolumeManager() {
    uint64_t kept = 0;
    for (uint64_t i = 0; i < s_managers.size(); i++) {
        if (s_managers[i] != this) {
            s_managers[kept++] = s_managers[i];
        }
    }
    s_managers.erase(kept, s_managers.size());
    for (uint64_t i = 0; i < this->m_volumes.size(); i++) {
        CParticulateVolume* volume = this->m_volumes[i];
        if (volume != nullptr) {
            ParticulateVolumeDestroy(volume);
            operator delete(volume);
        }
        this->m_volumes[i] = nullptr;
    }
    if (this->m_buffer != nullptr) {
        GxDeviceRelease0f8(&this->m_buffer, this->m_buffer);
    }
    this->m_byInstance.Free();
    this->m_free.Free();
    // m_volumes: its storage, as blz::vector frees it.
}

// ref: FUN_141407490
void ParticulateVolumeDestroy(CParticulateVolume* volume) {
    (void)volume;
    FROST_UNIMPLEMENTED();
}
