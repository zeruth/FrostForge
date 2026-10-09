#ifndef UTIL_FNV_HPP
#define UTIL_FNV_HPP

#include <cstdint>

namespace t2util {

constexpr uint64_t FNV64_BASIS = 0xCBF29CE484222325ull;
constexpr uint64_t FNV64_PRIME = 0x100000001B3ull;

// One step of the case-insensitive 64-bit FNV-1a: ASCII upper case folds to lower case, and
// the character is sign-extended to 32 bits, then zero-extended into the hash.
inline uint64_t FnvStepI(char c, uint64_t hash) {
    int32_t value = static_cast<int32_t>(c);
    if (static_cast<uint8_t>(c - 'A') <= 0x19) {
        value += 0x20;
    }
    return (static_cast<uint64_t>(static_cast<uint32_t>(value)) ^ hash) * FNV64_PRIME;
}

// Continues a case-insensitive FNV-1a hash over the rest of a string.
uint64_t FnvHashStringI(const char* string, uint64_t hash);

// The key hash of t2util::string_hash<ConstFnvHashStringI>. Call sites inline the first
// character and call FnvHashStringI for the rest.
inline uint64_t ConstFnvHashStringI(const char* string) {
    if (*string == '\0') {
        return FNV64_BASIS;
    }
    return FnvHashStringI(string + 1, FnvStepI(*string, FNV64_BASIS));
}

}

#endif
