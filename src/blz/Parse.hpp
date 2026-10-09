#ifndef BLZ_PARSE_HPP
#define BLZ_PARSE_HPP

#include <cstdint>

namespace blz {

// Parses a decimal number after optional whitespace and sign; returns the characters
// consumed (0 for none). A negative number or an overflow sets *error.
uint64_t parse_uint32(const char* text, uint64_t length, uint32_t* value, bool* error);

}

#endif
