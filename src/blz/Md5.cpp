#include "blz/Md5.hpp"
#include <cstring>

namespace blz {

namespace {

// The per-step constants (the reference's table at 0x14292c118).
const uint32_t s_sines[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

const uint8_t s_shifts[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

uint32_t Rotate(uint32_t value, uint32_t shift) {
    return (value << shift) | (value >> (32 - shift));
}

}

// ref: FUN_142366090
void md5_init(md5_context* context) {
    context->m_state[0] = 0x67452301;
    context->m_bits = 0;
    context->m_state[1] = 0xefcdab89;
    context->m_state[2] = 0x98badcfe;
    context->m_state[3] = 0x10325476;
    context->m_used = 0;
}

// The standard MD5 block function.
// ref: FUN_142365720
void md5_transform(md5_context* context, const uint8_t block[64]) {
    uint32_t words[16];
    std::memcpy(words, block, sizeof(words));
    uint32_t a = context->m_state[0];
    uint32_t b = context->m_state[1];
    uint32_t c = context->m_state[2];
    uint32_t d = context->m_state[3];
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t f;
        uint32_t g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        uint32_t next = d;
        d = c;
        c = b;
        b = b + Rotate(a + f + s_sines[i] + words[g], s_shifts[i]);
        a = next;
    }
    context->m_state[0] += a;
    context->m_state[1] += b;
    context->m_state[2] += c;
    context->m_state[3] += d;
}

// ref: FUN_142365f00
void md5_update(md5_context* context, const void* data, uint64_t size) {
    auto bytes = static_cast<const uint8_t*>(data);
    while (size != 0) {
        uint32_t used = context->m_used;
        if (used == 0 && size > 0x3F) {
            md5_transform(context, bytes);
            context->m_bits += 0x200;
            bytes += 0x40;
            size -= 0x40;
        } else {
            uint32_t take = static_cast<uint32_t>(size);
            if (0x40 - used <= size) {
                take = 0x40 - used;
            }
            std::memcpy(context->m_block + used, bytes, take);
            bytes += take;
            size -= take;
            context->m_used += take;
            if (context->m_used == 0x40) {
                md5_transform(context, context->m_block);
                context->m_bits += 0x200;
                context->m_used = 0;
            }
        }
    }
}

// ref: FUN_142365fd0
void md5_final(md5_context* context, uint8_t digest[16]) {
    context->m_bits += uint64_t(context->m_used) * 8;
    context->m_block[context->m_used++] = 0x80;
    if (context->m_used > 0x38) {
        while (context->m_used < 0x40) {
            context->m_block[context->m_used++] = 0;
        }
        md5_transform(context, context->m_block);
        context->m_used = 0;
    }
    while (context->m_used < 0x38) {
        context->m_block[context->m_used++] = 0;
    }
    std::memcpy(context->m_block + 0x38, &context->m_bits, 8);
    md5_transform(context, context->m_block);
    std::memcpy(digest, context->m_state, 16);
}

}
