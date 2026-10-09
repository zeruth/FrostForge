#ifndef BLZ_MD5_HPP
#define BLZ_MD5_HPP

#include <cstdint>

namespace blz {

// MD5 (0x5c bytes): bit count, state, the partial block.
struct md5_context {
    uint64_t m_bits;        // 0x00
    uint32_t m_state[4];    // 0x08
    uint32_t m_used;        // 0x18
    uint8_t m_block[64];    // 0x1c
};
static_assert(sizeof(md5_context) == 0x60, "md5_context is 0x5c bytes plus padding");

void md5_init(md5_context* context);
void md5_update(md5_context* context, const void* data, uint64_t size);
void md5_final(md5_context* context, uint8_t digest[16]);
void md5_transform(md5_context* context, const uint8_t block[64]);

}

#endif
