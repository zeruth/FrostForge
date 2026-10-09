#ifndef BLZ_HEX_HPP
#define BLZ_HEX_HPP

#include <cstdint>

namespace blz {

// Lower-case hex of `length` bytes into out (at most outSize - 1 characters, terminated);
// returns the full length the text would have (length * 2).
uint64_t hex_encode(char* out, uint64_t outSize, const uint8_t* data, uint64_t length);

// Decodes hex pairs into out (at most outSize bytes, stopping at the first non-hex pair or
// when fewer than two characters remain); returns the pairs decoded.
uint64_t hex_decode(uint8_t* out, uint64_t outSize, const char* text, uint64_t length);

}

#endif
