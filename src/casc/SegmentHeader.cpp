#include "casc/SegmentHeader.hpp"
#include "casc/Hash.hpp"
#include <cstring>

namespace casc {

namespace {

// DAT_1428f2f20
const uint32_t s_checkTable[16] = {
    0x049396B8, 0x72A82A9B, 0xEE626CCA, 0x9917754F, 0x15DE40B1, 0xF5A8A9B6, 0x421EAC7E, 0xA9D55C9A,
    0x317FD40C, 0x04FAF80D, 0x3D6BE971, 0x52933CFD, 0x27F64B7D, 0xC6F5C11B, 0xD5757E3A, 0x6C388745,
};

uint32_t CheckWord(uint32_t offset) {
    return (offset + 0x1e) ^ s_checkTable[(offset + 0x1e) & 0xF];
}

}

// Whether the 0x1e bytes, XORed together a byte at a time into a word whose lanes rotate with
// the offset, give the check word of that offset.
// ref: FUN_142150350
bool SegmentHeaderChecksum(const uint8_t* data, uint32_t offset) {
    uint8_t lanes[4] = { 0, 0, 0, 0 };
    for (uint32_t i = 0; i < 0x1e; i++) {
        lanes[(offset + i) & 3] ^= data[i];
    }
    uint32_t word;
    std::memcpy(&word, lanes, 4);
    return word == CheckWord(offset);
}

// Fills in the hash (0x16) and the check bytes (0x1a) of a header that sits at `offset`.
// ref: FUN_142150170
void SegmentHeaderSeal(uint8_t* data, uint32_t offset) {
    uint32_t hash = HashLittle(data, 0x16, 0x3d6be971);
    std::memcpy(data + 0x16, &hash, 4);
    uint32_t check = CheckWord(offset);
    uint8_t want[4];
    std::memcpy(want, &check, 4);
    uint8_t lanes[4] = { 0, 0, 0, 0 };
    for (uint32_t i = 0; i < 0x1a; i++) {
        lanes[(offset + i) & 3] ^= data[i];
    }
    for (uint32_t i = 0x1a; i < 0x1e; i++) {
        uint32_t lane = (offset + i) & 3;
        data[i] = lanes[lane] ^ want[lane];
    }
}

// ref: FUN_1421505c0
bool SegmentHeaderParse(const uint8_t* data, uint32_t size, uint64_t offset, SEGMENTHEADER* header) {
    if (size < 0x1e || !SegmentHeaderChecksum(data, static_cast<uint32_t>(offset))) {
        return false;
    }
    uint32_t hash = HashLittle(data, 0x16, 0x3d6be971);
    for (uint32_t i = 0; i < 4; i++) {
        if (data[0x16 + i] != static_cast<uint8_t>(hash)) {
            return false;
        }
        hash >>= 8;
    }
    if (data[0x14] >= 2) {
        return false;
    }
    for (uint32_t i = 0; i < 16; i++) {
        header->m_key[i] = data[15 - i];
    }
    uint32_t recorded;
    std::memcpy(&recorded, data + 0x10, 4);
    header->m_span.m_offset = offset;
    header->m_span.m_size = recorded;
    header->m_flag = data[0x14];
    return true;
}

// ref: FUN_1421506f0
bool SegmentHeaderSerialize(uint8_t* data, uint32_t size, const uint8_t key[16], const SPAN* span, uint8_t flag) {
    if (size < 0x1e) {
        return false;
    }
    std::memset(data, 0, 0x1e);
    for (uint32_t i = 0; i < 16; i++) {
        data[15 - i] = key[i];
    }
    std::memcpy(data + 0x10, &span->m_size, 4);
    data[0x14] = flag;
    data[0x15] = 0;
    SegmentHeaderSeal(data, static_cast<uint32_t>(span->m_offset));
    return true;
}

}
