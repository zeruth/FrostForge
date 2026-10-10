#ifndef UI_C_SIMPLE_EDIT_BOX_TEXT_HPP
#define UI_C_SIMPLE_EDIT_BOX_TEXT_HPP

// The edit box's walks over its m_textInfo entries, which the reference inlines at every use:
// a step is one character with the codes that travel with it. The cursor's steps also keep a
// hyperlink or a texture whole.

#include <cstdint>

namespace EditBoxText {

constexpr uint32_t INFO_IN_LINK = 0x80000000;
constexpr uint32_t INFO_IN_TEXTURE = 0x40000000;

inline uint32_t Length(uint32_t info) {
    return info & 0xffff;
}

inline uint8_t Kind(uint32_t info) {
    return static_cast<uint8_t>(info >> 16);
}

// The codes that open (and go with the character after them): 1, 5, 7, 9, 13.
inline bool IsOpen(uint8_t kind) {
    return (kind <= 9 && ((0x2a2u >> kind) & 1) != 0) || kind == 13;
}

// The codes that close (and go with the character before them): 2, 6, 8, 10, 12, 14.
inline bool IsClose(uint8_t kind) {
    return kind <= 14 && ((0x5544u >> kind) & 1) != 0;
}

// The letters the limits count: 0, 3 and 4.
inline bool IsLetter(uint8_t kind) {
    return kind == 0 || static_cast<uint8_t>(kind - 3) < 2;
}

// One character forward from *entry (not at the end); the bytes passed.
inline int32_t StepForward(const uint32_t*& entry, bool cursor) {
    int32_t bytes = 0;
    uint32_t info;
    uint8_t kind;
    do {
        info = *entry;
        kind = Kind(info);
        bytes += static_cast<int32_t>(Length(info));
        entry += Length(info);
    } while (IsOpen(kind)
        || (cursor && ((static_cast<int32_t>(info) < 0 && kind != 6) || ((info & INFO_IN_TEXTURE) != 0 && kind != 10))));
    while (IsClose(Kind(*entry))) {
        bytes += static_cast<int32_t>(Length(*entry));
        entry += Length(*entry);
    }
    return bytes;
}

// The entry before *entry (skipping a token's empty entries, not below `base`).
inline const uint32_t* Previous(const uint32_t* base, const uint32_t* entry) {
    do {
        entry--;
    } while (base < entry && *entry == 0);
    return entry;
}

// One character back from *entry (above `base`); the bytes passed.
inline int32_t StepBack(const uint32_t* base, const uint32_t*& entry, bool cursor) {
    int32_t bytes = 0;
    for (;;) {
        entry = Previous(base, entry);
        uint32_t info = *entry;
        uint8_t kind = Kind(info);
        bytes += static_cast<int32_t>(Length(info));
        if (!IsClose(kind)
            && !(cursor && ((static_cast<int32_t>(info) < 0 && kind != 5) || ((info & INFO_IN_TEXTURE) != 0 && kind != 9)))) {
            break;
        }
    }
    while (base < entry) {
        const uint32_t* previous = Previous(base, entry);
        if (!IsOpen(Kind(*previous))) {
            break;
        }
        entry = previous;
        bytes += static_cast<int32_t>(Length(*entry));
    }
    return bytes;
}

// The bytes `count` characters forward (or back, when negative) from `start`.
inline int32_t Walk(const uint32_t* base, int32_t start, int32_t count, bool cursor) {
    const uint32_t* entry = base + start;
    int32_t bytes = 0;
    if (0 < count) {
        do {
            count--;
            if (*entry == 0) {
                break;
            }
            bytes += StepForward(entry, cursor);
        } while (0 < count);
        return bytes;
    }
    count = -count;
    while (0 < count) {
        count--;
        if (entry <= base) {
            break;
        }
        bytes += StepBack(base, entry, cursor);
    }
    return bytes;
}

}

#endif
