#ifndef CASC_SEGMENT_HEADER_HPP
#define CASC_SEGMENT_HEADER_HPP

// The 0x1e-byte headers in front of every file in a data.### archive (the first 16 of an
// archive describe the archive's own reconstruction headers): the key byte-reversed, the
// size, a flag, a lookup3 hash of the first 0x16 bytes, and a check word tied to the offset
// the header sits at.

#include "casc/KeyMappingTable.hpp"
#include <cstdint>

namespace casc {

struct SEGMENTHEADER {
    uint8_t m_key[16];      // 0x00
    SPAN m_span;            // 0x10: where the header sits, and the size it records
    uint8_t m_flag;         // 0x20
};

bool SegmentHeaderChecksum(const uint8_t* data, uint32_t offset);
void SegmentHeaderSeal(uint8_t* data, uint32_t offset);
bool SegmentHeaderParse(const uint8_t* data, uint32_t size, uint64_t offset, SEGMENTHEADER* header);
bool SegmentHeaderSerialize(uint8_t* data, uint32_t size, const uint8_t key[16], const SPAN* span, uint8_t flag);

}

#endif
