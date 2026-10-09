#include "base/CharClass.hpp"

namespace {

#include "base/CharSpaceTables.inc"

}

// ref: FUN_142431160
bool CharIsKorean(uint32_t code) {
    uint16_t c = static_cast<uint16_t>(code);
    return static_cast<uint16_t>(c - 0x1100) < 0x100 || static_cast<uint16_t>(c - 0x3130) < 0x60
        || static_cast<uint16_t>(c - 0xac00) < 0x2ba4;
}

// ref: FUN_1424311a0
bool CharIsChinese(uint32_t code) {
    uint16_t c = static_cast<uint16_t>(code);
    if (static_cast<uint16_t>(c - 0x4e00) < 0x5200 || static_cast<uint16_t>(c - 0x3100) < 0x2d) {
        return true;
    }
    if (static_cast<uint16_t>(c - 0xf900) < 0x200) {
        switch (c - 0xf92c) {
        case 0: case 0x4d: case 0x69: case 0xbb: case 0xc5: case 0xe0: case 0xe1: case 0xe2: case 0xe3: case 0xe5:
        case 0xe7: case 0xe8: case 0xec: case 0xf3: case 0xf4: case 0xf5: case 0xf7: case 0xf8: case 0xfb: case 0xfc:
        case 0xfd:
            return true;
        }
    }
    return false;
}

// ref: FUN_142431320
bool CharIsCyrillic(uint32_t code) {
    return static_cast<uint16_t>(static_cast<uint16_t>(code) - 0x400) < 0x100;
}

// ref: FUN_1424313a0
bool CharIsLatin(uint32_t code) {
    uint16_t c = static_cast<uint16_t>(code);
    if (static_cast<uint16_t>(c - 0x41) < 0x1a || static_cast<uint16_t>(c - 0x61) < 0x1a) {
        return true;
    }
    if (static_cast<uint16_t>(c - 0xc0) < 0x1e && c != 0xd7) {
        return true;
    }
    if (c == 0xdf) {
        return true;
    }
    if (static_cast<uint16_t>(c - 0xe0) < 0x20) {
        return c != 0xf7 && c != 0xfe;
    }
    return c == 0x178;
}

// ref: FUN_142431460
bool CharIsSpace(uint32_t code) {
    if ((code >> 8) >= 0x31) {
        return false;
    }
    uint8_t row = s_spaceRows[s_spaceBlocks[code >> 8] * 0x10 + ((code & 0xff) >> 4)];
    return s_spaceFlags[row * 0x10 + (code & 0xf)] != 0;
}
