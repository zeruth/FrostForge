#include "tact/Common.hpp"
#include "blz/Hex.hpp"
#include "os/Os.hpp"

namespace tact {

const char g_logCategory[] = "tact";

namespace {

// The package allocators' function: everything goes to the process allocator's reallocate
// (the flag, tag and line are not used).
// ref: FUN_1420dad40
void* PackageRealloc(void* context, void* ptr, uint64_t size, uint64_t alignment, bool flag, const char* tag,
    int32_t line) {
    (void)context;
    (void)flag;
    (void)tag;
    (void)line;
    return blz::default_allocator()->reallocate(ptr, size, alignment);
}

int32_t Lower(char c) {
    int32_t value = c;
    return static_cast<uint32_t>(value - 'A') <= 0x19 ? value + 0x20 : value;
}

// Whether the first `length` characters of text match `word` case-insensitively, with
// text ending (or running out) where the word ends - or running out first.
bool MatchesWord(const char* text, uint64_t length, const char* word) {
    for (;;) {
        if (length == 0) {
            return true;
        }
        int32_t a = Lower(*text++);
        length--;
        int32_t b = Lower(*word);
        if (a != b) {
            return false;
        }
        if (a == 0) {
            return true;
        }
        word++;
    }
}

const char* const s_trueWords[8] = { "1", "on", "enable", "enabled", "yes", "y", "true", "t" };      // 0x1428e5cc0
const char* const s_falseWords[8] = { "0", "off", "disable", "disabled", "no", "n", "false", "f" };  // 0x1428e5d00

}

// ref: FUN_1420dae80
blz::allocator* GetAllocator() {
    static blz::function_allocator s_allocator(&PackageRealloc, nullptr, true);
    return &s_allocator;
}

// ref: FUN_1420daf10
blz::allocator* GetBufferAllocator() {
    static blz::function_allocator s_allocator(&PackageRealloc, nullptr, false);
    return &s_allocator;
}

void* PackageAlloc(blz::allocator* allocator, uint64_t size, uint64_t alignment, bool flag, const char* tag,
    int32_t line) {
    auto function = static_cast<blz::function_allocator*>(allocator);
    return function->m_function(function->m_context, nullptr, size, alignment, flag, tag, line);
}

void PackageFree(blz::allocator* allocator, void* ptr) {
    auto function = static_cast<blz::function_allocator*>(allocator);
    function->m_function(function->m_context, ptr, 0, 0, false, nullptr, 0);
}

// The packages' hex encoding (blz::hex_encode).
uint64_t HexEncode(char* out, uint64_t outSize, const uint8_t* data, uint64_t length) {
    return blz::hex_encode(out, outSize, data, length);
}

// ref: FUN_1420d8f40
uint64_t KeyToString(char* out, uint64_t outSize, const uint8_t key[16]) {
    return HexEncode(out, outSize, key, 16);
}

namespace {

const float s_fullRate = 1.0f;  // DAT_142618d64

// ref: FUN_1420faf50
int64_t FormatCounter(char* out, uint64_t size, const char* name, int64_t value, float rate) {
    if (rate < s_fullRate) {
        return OsSnprintf(out, size, "%s:%lld|c|@%0.2f\n", name, value, static_cast<double>(rate));
    }
    return OsSnprintf(out, size, "%s:%lld|c\n", name, value);
}

// ref: FUN_1420fafb0
int64_t FormatGauge(char* out, uint64_t size, const char* name, int64_t value) {
    return OsSnprintf(out, size, "%s:%lld|g\n", name, value);
}

// ref: FUN_1420fafd0
int64_t FormatSet(char* out, uint64_t size, const char* name, int64_t value) {
    return OsSnprintf(out, size, "%s:%lld|s\n", name, value);
}

// ref: FUN_1420faff0
int64_t FormatTimer(char* out, uint64_t size, const char* name, int64_t value, float rate) {
    if (rate < s_fullRate) {
        return OsSnprintf(out, size, "%s:%lld|ms|@%0.2f\n", name, value, static_cast<double>(rate));
    }
    return OsSnprintf(out, size, "%s:%lld|ms\n", name, value);
}

}

// ref: FUN_1420dc510
void StatsReport(void* sink, int32_t kind, const char* name, int64_t value, float rate) {
    auto block = static_cast<void**>(sink);
    auto report = reinterpret_cast<void (*)(void*, void*, const char*)>(block[1]);
    if (!report) {
        return;
    }
    char line[0x200];
    std::memset(line + 1, 0, 0x1ff);
    std::memcpy(line, "tact.", 5);
    if (kind == 0) {
        FormatCounter(line + 5, 0x1fb, name, value, rate);
    } else if (kind == 1) {
        FormatGauge(line + 5, 0x1fb, name, value);
    } else if (kind == 2) {
        FormatSet(line + 5, 0x1fb, name, value);
    } else if (kind == 3) {
        FormatTimer(line + 5, 0x1fb, name, value, rate);
    }
    report(block[2], block[0], line);
}

// ref: 0x1420d9040 (reached through the thunk 0x1402cbf30)
bool ParseString(const char* text, uint64_t length, void* out) {
    (void)length;
    *static_cast<const char**>(out) = text;
    return true;
}

// The packages' hex decoding (blz::hex_decode).
uint64_t HexDecode(uint8_t* out, uint64_t outSize, const char* text, uint64_t length) {
    return blz::hex_decode(out, outSize, text, length);
}

// ref: FUN_1420d8ed0
uint64_t ParseKey16(const char* text, uint64_t length, uint8_t key[16], bool* error) {
    std::memset(key, 0, 16);
    *error = false;
    const char* p = text;
    while (p != text + length && (*p == ' ' || static_cast<uint8_t>(*p - 9) < 5)) {
        p++;
    }
    uint64_t skipped = static_cast<uint64_t>(p - text);
    if (HexDecode(key, 16, p, length - skipped) != 16) {
        return 0;
    }
    return skipped + 0x20;
}

// ref: FUN_1402cbf40
bool ParseHexKey(const char* text, uint64_t length, void* out) {
    bool error = false;
    uint64_t consumed = ParseKey16(text, length, static_cast<uint8_t*>(out), &error);
    return !error && consumed == length;
}

// ref: FUN_1420d9050 (reached through the thunk 0x1420da210)
bool ParseBool(const char* text, uint64_t length, void* out) {
    for (const char* word : s_trueWords) {
        if (MatchesWord(text, length, word)) {
            *static_cast<uint8_t*>(out) = 1;
            return true;
        }
    }
    for (const char* word : s_falseWords) {
        if (MatchesWord(text, length, word)) {
            *static_cast<uint8_t*>(out) = 0;
            return true;
        }
    }
    return false;
}

// ref: FUN_142363670
uint64_t ParseUInt64(const char* text, uint64_t length, uint64_t* value, bool* overflow) {
    *value = 0;
    *overflow = false;
    if (length == 0) {
        return 0;
    }
    uint64_t i = 0;
    char c;
    while ((c = text[i]) == ' ' || static_cast<uint8_t>(c - 9) < 5) {
        if (++i >= length) {
            return 0;
        }
    }
    uint64_t start = i + (((c - 0x2B) & 0xFD) == 0 ? 1 : 0);
    const char* digits = text + start;
    const char* p = digits;
    uint64_t result = 0;
    bool over = false;
    while (p < text + length && static_cast<uint64_t>(static_cast<int64_t>(*p) - '0') <= 9) {
        uint64_t next = static_cast<uint64_t>(static_cast<int64_t>(*p) - '0') + result * 10;
        if (next < result) {
            over = true;
        }
        result = next;
        p++;
    }
    uint64_t count = static_cast<uint64_t>(p - digits);
    if (count == 0) {
        return 0;
    }
    if (c == '-') {
        if (over || result != 0) {
            *overflow = true;
        }
        *value = 0;
        return start + count;
    }
    if (over) {
        result = ~uint64_t(0);
        *overflow = true;
    }
    *value = result;
    return start + count;
}

}
