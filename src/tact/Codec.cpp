#include "tact/Codec.hpp"
#include "blz/Hex.hpp"
#include "blz/Log.hpp"
#include "blz/Md5.hpp"
#include "tact/Common.hpp"
#include "tact/U32Set.hpp"
#include "os/Os.hpp"
#include <cstring>
#include <windows.h>
#include <zlib.h>

namespace tact {

namespace {

const char s_decodingFile[] = "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\codec\\decoding.cpp";
const char s_eSpecFile[] = "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\codec\\e_spec.cpp";
const char s_headerFile[] = "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\codec\\encoding_header.cpp";
const char s_uncompressedFile[] =
    "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\codec\\codec_uncompressed.cpp";
const char s_zlibFile[] = "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\codec\\codec_zlib.cpp";
const char s_lz4hcFile[] = "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\codec\\codec_lz4hc.cpp";
const char s_bcpackFile[] = "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\codec\\codec_bcpack.cpp";
const char s_encryptionFile[] =
    "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\codec\\codec_encryption.cpp";

const uint32_t s_blteMagic = 0x45544C42;  // "BLTE" (DAT_1428eea80)
const char s_noTag[] = "";                // DAT_1426178ba

bool IsSpace(char c) {
    return c == ' ' || static_cast<uint8_t>(c - 9) < 5;
}

int32_t Lower(char c) {
    return static_cast<uint8_t>(c + 0xBF) <= 0x19 ? c + 0x20 : c;
}

uint32_t ReadBE32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// strtoul over 32 bits: base 0 picks 8, 10 or 16 from the prefix; an overflow gives
// 0xffffffff and sets *overflow.
// ref: FUN_14026ae10
uint32_t ParseUnsigned(const char* text, const char** end, uint32_t base, char* overflow) {
    char first = *text;
    const char* p = text + 1;
    while (first == ' ' || static_cast<uint8_t>(first - 9) < 5) {
        text = p;
        first = *p;
        p = text + 1;
    }
    char c = first;
    if (((first - 0x2B) & 0xFD) == 0) {
        c = *p;
        p = text + 2;
    }
    bool skipPrefix = false;
    if (base == 0) {
        if (c != '0') {
            base = 10;
        } else if (((*p + 0xA8) & 0xDF) != 0) {
            base = 8;
        } else {
            base = 0x10;
            skipPrefix = true;
        }
    } else if (base == 0x10 && c == '0' && ((*p + 0xA8) & 0xDF) == 0) {
        skipPrefix = true;
    }
    if (skipPrefix) {
        c = p[1];
        p = p + 2;
    }
    char over = 0;
    uint32_t value = 0;
    for (;;) {
        uint32_t digit;
        if (static_cast<uint8_t>(c - '0') < 10) {
            digit = static_cast<uint32_t>(c - '0');
        } else if (static_cast<uint8_t>(c + 0x9F) < 0x1A) {
            digit = static_cast<uint32_t>(c - 0x57);
        } else if (static_cast<uint8_t>(c + 0xBF) < 0x1A) {
            digit = static_cast<uint32_t>(c - 0x37);
        } else {
            digit = 0xFFFFFFFF;
        }
        if (base <= digit) {
            break;
        }
        uint32_t limit = static_cast<uint32_t>(0xFFFFFFFFull / base);
        if (value < limit || (value == limit && digit <= static_cast<uint32_t>(0xFFFFFFFFull % base))) {
            c = *p;
            value = value * base + digit;
            p++;
        } else {
            c = *p;
            over = 1;
            p++;
        }
    }
    if (end) {
        *end = p - 1;
    }
    if (over) {
        value = 0xFFFFFFFF;
    }
    if (overflow) {
        *overflow = over;
    }
    if (first == '-' && !over) {
        value = 0u - value;
    }
    return value;
}

// --- zlib allocation hooks -------------------------------------------------------------------

// ref: FUN_14212d540
voidpf ZlibAlloc(voidpf opaque, uInt items, uInt size) {
    (void)opaque;
    return PackageAlloc(GetBufferAllocator(), uint64_t(items * size), 0x10, false, s_noTag, 0);
}

// ref: FUN_14212d590
void ZlibFree(voidpf opaque, voidpf address) {
    (void)opaque;
    if (address) {
        PackageFree(GetBufferAllocator(), address);
    }
}

// ref: FUN_14212dcf0
uint8_t ZlibInit(z_stream* stream) {
    std::memset(stream, 0, sizeof(*stream));
    stream->zalloc = &ZlibAlloc;
    stream->zfree = &ZlibFree;
    int result = inflateInit_(stream, "1.2.11", static_cast<int>(sizeof(z_stream)));
    if (result == Z_OK) {
        stream->reserved |= 1;
        return 0;
    }
    blz::log(g_logCategory, 4, s_zlibFile, 0x6c, "Zlib inflateInit failed. Error (%d): %s", result,
        stream->msg ? stream->msg : "error");
    return result == Z_STREAM_ERROR ? 3 : 1;
}

// --- Salsa20 ---------------------------------------------------------------------------------

// The cipher state as the reference lays it out for its SSE rounds ("expand 16-byte k" at
// 0x1428f0410, then the key, IV and counter words interleaved).
struct SALSA20 {
    uint32_t m_words[16];
};

// ref: FUN_1421334d0
void Salsa20Setup(SALSA20* state, const uint32_t key[4], const uint32_t iv[2], uint64_t counter) {
    static const char s_sigma[] = "expand 16-byte k";
    std::memcpy(&state->m_words[0], s_sigma, 16);
    state->m_words[4] = key[1];
    state->m_words[5] = key[0];
    state->m_words[6] = iv[0];
    state->m_words[7] = key[0];
    state->m_words[8] = static_cast<uint32_t>(counter);
    state->m_words[9] = key[2];
    state->m_words[10] = key[1];
    state->m_words[11] = iv[1];
    state->m_words[12] = key[3];
    state->m_words[13] = static_cast<uint32_t>(counter >> 32);
    state->m_words[14] = key[3];
    state->m_words[15] = key[2];
}

uint32_t Rotl(uint32_t v, int c) {
    return v << c | v >> (32 - c);
}

// One 64-byte keystream block of Salsa20/20 from the state above, in the standard word order
// (constants on the diagonal), and the counter advanced.
void Salsa20Block(SALSA20* state, uint8_t out[64]) {
    const uint32_t* w = state->m_words;
    uint32_t c0;
    uint32_t c1;
    uint32_t c2;
    uint32_t c3;
    std::memcpy(&c0, reinterpret_cast<const uint8_t*>(w) + 0, 4);
    std::memcpy(&c1, reinterpret_cast<const uint8_t*>(w) + 4, 4);
    std::memcpy(&c2, reinterpret_cast<const uint8_t*>(w) + 8, 4);
    std::memcpy(&c3, reinterpret_cast<const uint8_t*>(w) + 12, 4);
    uint32_t k0 = w[5];
    uint32_t k1 = w[4];
    uint32_t k2 = w[15];
    uint32_t k3 = w[14];
    uint64_t counter = uint64_t(w[13]) << 32 | w[8];
    uint32_t x[16] = {
        c0, k0, k1, k2,
        k3, c1, w[6], w[11],
        static_cast<uint32_t>(counter), static_cast<uint32_t>(counter >> 32), c2, k0,
        k1, k2, k3, c3,
    };
    uint32_t in[16];
    std::memcpy(in, x, sizeof(in));
    for (int i = 0; i < 20; i += 2) {
        x[4] ^= Rotl(x[0] + x[12], 7);
        x[8] ^= Rotl(x[4] + x[0], 9);
        x[12] ^= Rotl(x[8] + x[4], 13);
        x[0] ^= Rotl(x[12] + x[8], 18);
        x[9] ^= Rotl(x[5] + x[1], 7);
        x[13] ^= Rotl(x[9] + x[5], 9);
        x[1] ^= Rotl(x[13] + x[9], 13);
        x[5] ^= Rotl(x[1] + x[13], 18);
        x[14] ^= Rotl(x[10] + x[6], 7);
        x[2] ^= Rotl(x[14] + x[10], 9);
        x[6] ^= Rotl(x[2] + x[14], 13);
        x[10] ^= Rotl(x[6] + x[2], 18);
        x[3] ^= Rotl(x[15] + x[11], 7);
        x[7] ^= Rotl(x[3] + x[15], 9);
        x[11] ^= Rotl(x[7] + x[3], 13);
        x[15] ^= Rotl(x[11] + x[7], 18);
        x[1] ^= Rotl(x[0] + x[3], 7);
        x[2] ^= Rotl(x[1] + x[0], 9);
        x[3] ^= Rotl(x[2] + x[1], 13);
        x[0] ^= Rotl(x[3] + x[2], 18);
        x[6] ^= Rotl(x[5] + x[4], 7);
        x[7] ^= Rotl(x[6] + x[5], 9);
        x[4] ^= Rotl(x[7] + x[6], 13);
        x[5] ^= Rotl(x[4] + x[7], 18);
        x[11] ^= Rotl(x[10] + x[9], 7);
        x[8] ^= Rotl(x[11] + x[10], 9);
        x[9] ^= Rotl(x[8] + x[11], 13);
        x[10] ^= Rotl(x[9] + x[8], 18);
        x[12] ^= Rotl(x[15] + x[14], 7);
        x[13] ^= Rotl(x[12] + x[15], 9);
        x[14] ^= Rotl(x[13] + x[12], 13);
        x[15] ^= Rotl(x[14] + x[13], 18);
    }
    for (int i = 0; i < 16; i++) {
        uint32_t v = x[i] + in[i];
        std::memcpy(out + i * 4, &v, 4);
    }
    counter++;
    state->m_words[8] = static_cast<uint32_t>(counter);
    state->m_words[13] = static_cast<uint32_t>(counter >> 32);
}

// XORs the keystream over data in place.
// ref: FUN_142133560 (with FUN_142133600 for runs of 256 bytes and FUN_1421340b0 for 64)
// DIVERGED: the reference runs the rounds on SSE registers over this state's interleaved word
// order; this computes the same keystream a block at a time in scalar code.
void Salsa20Process(SALSA20* state, uint8_t* data, uint32_t size) {
    uint8_t block[64];
    while (size >= 64) {
        Salsa20Block(state, block);
        for (int i = 0; i < 64; i++) {
            data[i] ^= block[i];
        }
        data += 64;
        size -= 64;
    }
    if (size != 0) {
        Salsa20Block(state, block);
        for (uint32_t i = 0; i < size; i++) {
            data[i] ^= block[i];
        }
    }
}

// The BCPack decoder lives in the GDK's XBTC.dll (from %GXDKLatest%\bin\Scarlett), which a
// PC install does not have.
struct BCPACKLIBRARY {
    HMODULE m_module;
    FARPROC m_compress;
    int32_t(__stdcall* m_decompress)(void* out, const void* data, uint32_t outSize, uint32_t outSize2, uint32_t size,
        uint32_t format);
};

// ref: FUN_14212bc00
void BcPackLoad(BCPACKLIBRARY* library) {
    library->m_module = nullptr;
    library->m_compress = nullptr;
    library->m_decompress = nullptr;
    wchar_t path[0x30C];
    DWORD length = GetEnvironmentVariableW(L"GXDKLatest", path, 0x30C);
    if (length - 1 < 0x2FE && path[0] != 0) {
        if (path[length - 1] != L'\\') {
            path[length++] = L'\\';
        }
        wcscpy_s(path + length, 0x30C - length, L"bin\\Scarlett");
        SetDllDirectoryW(path);
        library->m_module = LoadLibraryW(L"XBTC.dll");
        if (library->m_module) {
            library->m_compress = GetProcAddress(library->m_module, "XBTCCompressStream");
            library->m_decompress = reinterpret_cast<decltype(library->m_decompress)>(
                GetProcAddress(library->m_module, "XBTCDecompressStream"));
            if (!library->m_compress || !library->m_decompress) {
                FreeLibrary(library->m_module);
            }
        }
    }
}

// ref: FUN_14212c320
BCPACKLIBRARY* BcPackLibrary() {
    static BCPACKLIBRARY s_library = [] {
        BCPACKLIBRARY library;
        BcPackLoad(&library);
        return library;
    }();
    return &s_library;
}

// ref: FUN_14212bf80
uint8_t BcPackDecompress(BCPACKLIBRARY* library, void* out, uint32_t outSize, const void* data, uint32_t size,
    int32_t bcn) {
    if (!library->m_module) {
        return 5;
    }
    uint32_t format;
    switch (bcn) {
    case 1:
        format = 0x46;
        break;
    case 2:
        format = 0x49;
        break;
    case 3:
        format = 0x4c;
        break;
    case 4:
        format = 0x4f;
        break;
    case 5:
        format = 0x52;
        break;
    case 6:
        format = 0x5e;
        break;
    case 7:
        format = 0x61;
        break;
    default:
        blz::log(g_logCategory, 4, s_bcpackFile, 0x95,
            "Cannot decompress BCPack data. Invalid BCn encoding type specified: %u", bcn);
        return 3;
    }
    int32_t result = library->m_decompress(out, data, size, size, outSize, format);
    if (result == static_cast<int32_t>(0x80004005)) {
        return 8;
    }
    if (result >= 0) {
        return 0;
    }
    char text[0x100];
    OsErrorString(text, sizeof(text), static_cast<uint32_t>(result));  // FUN_140267930
    blz::log(g_logCategory, 4, s_bcpackFile, 0xb2, "Failed to decompress BCPack stream. Error (%d): %s", result, text);
    return 1;
}

// ref: FUN_14212ce50
void LogMissingKey(const uint8_t name[8]) {
    char text[0x21];
    blz::hex_encode(text, sizeof(text), name, 8);
    blz::log(g_logCategory, 4, s_encryptionFile, 0x3d, "Failed to decrypt data: encryption key id %s was not found",
        text);
}

}

// --- e-spec ----------------------------------------------------------------------------------

// The codec an e-spec names by its first letter: b blocks, c BCPack, e encrypted, g, n none,
// z zlib (lz4hc when its variant says so).
// ref: FUN_1420d8300
uint8_t ESpecType(const char* spec, const char* end) {
    if (!end) {
        end = spec + std::strlen(spec);
    }
    bool braced = false;
    for (;;) {
        if (end <= spec) {
            return ESPEC_INVALID;
        }
        if (!IsSpace(*spec)) {
            break;
        }
        spec++;
    }
    char letter = *spec;
    bool hasArguments = false;
    const char* at = spec;
    for (;;) {
        at++;
        if (end <= at) {
            break;
        }
        if (!IsSpace(*at)) {
            if (*at != ':') {
                return ESPEC_INVALID;
            }
            at++;
            for (;;) {
                hasArguments = true;
                if (end <= at) {
                    break;
                }
                if (!IsSpace(*at)) {
                    if (*at == '{') {
                        at++;
                        braced = true;
                        if (end <= at) {
                            return ESPEC_INVALID;
                        }
                        while (IsSpace(*at)) {
                            at++;
                            if (end <= at) {
                                return ESPEC_INVALID;
                            }
                        }
                    }
                    break;
                }
                at++;
            }
            break;
        }
    }
    switch (Lower(letter)) {
    case 'b':
        return ESPEC_BLOCKS;
    case 'c':
        return ESPEC_BCPACK;
    case 'e':
        return ESPEC_ENCRYPTED;
    case 'g':
        return ESPEC_G;
    case 'n':
        return ESPEC_NONE;
    case 'z': {
        if (!hasArguments) {
            return ESPEC_ZLIB;
        }
        ZLIBSPEC zlib;
        zlib.m_level = 9;
        zlib.m_windowBits = 0xf;
        zlib.m_mpq = 0;
        zlib.m_variant = 0;
        ESpecParseZlib(at, end, braced, &zlib);
        return static_cast<uint8_t>((zlib.m_variant == 1) + ESPEC_ZLIB);
    }
    default:
        return ESPEC_INVALID;
    }
}

// A decimal argument (optional) followed by ',' or '}' or the end.
// ref: FUN_1420d7aa0
bool ESpecNumber(const char** cursor, const char* end, bool* present, int32_t* value) {
    *present = false;
    *value = 0;
    while (*cursor < end && IsSpace(**cursor)) {
        (*cursor)++;
    }
    const char* start = *cursor;
    if (start >= end) {
        return true;
    }
    const char* p = start;
    while (p < end) {
        char c = *p;
        if (static_cast<uint8_t>(c - '0') > 9) {
            if (!IsSpace(c) && c != ',' && c != '}') {
                return false;
            }
            break;
        }
        p++;
        *cursor = p;
    }
    if (*cursor != start && *cursor - start > 0) {
        *present = true;
        *value = static_cast<int32_t>(ParseUnsigned(start, nullptr, 0, nullptr));
    }
    while (*cursor < end && IsSpace(**cursor)) {
        (*cursor)++;
    }
    if (*cursor < end) {
        char c = **cursor;
        if (c != ',') {
            return c == '}';
        }
    }
    return true;
}

// z:{level, variant (mpq, zlib, lz4hc or window bits), window bits}.
// ref: FUN_1420d72a0
uint8_t ESpecParseZlib(const char* cursor, const char* end, bool braced, ZLIBSPEC* spec) {
    bool present = false;
    int32_t value = 0;
    if (!ESpecNumber(&cursor, end, &present, &value)) {
        return 3;
    }
    if (present) {
        if (static_cast<uint32_t>(value - 1) > 8) {
            blz::log(g_logCategory, 4, s_eSpecFile, 0x197,
                "Invalid encoding specifier: invalid z-compression level '%d'. The valid range is [%u, %u]", value, 1, 9);
            return 3;
        }
        spec->m_level = value;
    }
    if (*cursor == ',') {
        cursor++;
    }
    bool bitsGiven = false;
    const char* word = cursor;
    while (word < end && IsSpace(*word)) {
        word++;
    }
    const char* wordEnd = word;
    cursor = word;
    if (word < end) {
        while (!IsSpace(*wordEnd) && *wordEnd != ',' && *wordEnd != '}') {
            wordEnd++;
            if (end <= wordEnd) {
                break;
            }
        }
        uint32_t length = wordEnd > word ? static_cast<uint32_t>(wordEnd - word) : 0;
        const char* after = wordEnd;
        while (after < end) {
            char c = *after;
            if (!IsSpace(c)) {
                if (c != ',' && c != '}') {
                    return 3;
                }
                break;
            }
            after++;
        }
        cursor = after;
        if (length != 0) {
            if (length == 3 && std::strncmp(word, "mpq", 3) == 0) {
                spec->m_mpq = 1;
            } else if (length == 4 && std::strncmp(word, "zlib", 4) == 0) {
                spec->m_variant = 0;
            } else if (length == 5 && std::strncmp(word, "lz4hc", 5) == 0) {
                spec->m_variant = 1;
            } else {
                const char* number = word;
                present = false;
                value = 0;
                if (!ESpecNumber(&number, word + length, &present, &value)) {
                    blz::log(g_logCategory, 4, s_eSpecFile, 0x1bc,
                        "Invalid encoding specifier: unknown encoding variant '%s'", number);
                    return 3;
                }
                if (present) {
                    if (static_cast<uint32_t>(value - 8) > 7) {
                        blz::log(g_logCategory, 4, s_eSpecFile, 0x1c5,
                            "Invalid encoding specifier: invalid zlib window bit count '%d'. The valid range is [%u, %u]",
                            value, 8, 0xf);
                        return 3;
                    }
                    bitsGiven = true;
                    spec->m_windowBits = value;
                }
            }
        }
    }
    if (*cursor == ',') {
        cursor++;
    }
    present = false;
    value = 0;
    if (!ESpecNumber(&cursor, end, &present, &value)) {
        return 3;
    }
    if (present) {
        if (static_cast<uint32_t>(value - 8) > 7) {
            blz::log(g_logCategory, 4, s_eSpecFile, 0x1dd,
                "Invalid encoding specifier: invalid zlib window bit count '%d'. The valid range is [%u, %u]", value, 8,
                0xf);
            return 3;
        }
        if (bitsGiven) {
            blz::log(g_logCategory, 3, s_eSpecFile, 0x1e3, "Zlib window bit count was specified multiple times.");
            if (spec->m_windowBits != value) {
                blz::log(g_logCategory, 4, s_eSpecFile, 0x1e6,
                    "Invalid encoding specifier: zlib window bit counts do not match");
                return 3;
            }
        }
        spec->m_windowBits = value;
    }
    while (cursor < end && IsSpace(*cursor)) {
        cursor++;
    }
    if (braced) {
        if (end <= cursor || *cursor != '}') {
            return 3;
        }
        do {
            cursor++;
        } while (cursor < end && IsSpace(*cursor));
    }
    return cursor == end ? 0 : 3;
}

// c:{n}: the BCn format, 1 to 7.
// ref: FUN_1420d7c10
uint8_t ESpecParseBcn(const char* spec, const char* end, int32_t* bcn) {
    *bcn = 1;
    if (!end) {
        end = spec + std::strlen(spec);
    }
    bool braced = false;
    for (;;) {
        if (end <= spec) {
            return 3;
        }
        if (!IsSpace(*spec)) {
            break;
        }
        spec++;
    }
    char letter = *spec;
    const char* cursor = spec;
    for (;;) {
        cursor++;
        if (end <= cursor) {
            break;
        }
        if (!IsSpace(*cursor)) {
            if (*cursor != ':') {
                return 3;
            }
            do {
                cursor++;
            } while (cursor < end && IsSpace(*cursor));
            if (cursor < end && *cursor == '{') {
                cursor++;
                braced = true;
                if (end <= cursor) {
                    return 3;
                }
                while (IsSpace(*cursor)) {
                    cursor++;
                    if (end <= cursor) {
                        return 3;
                    }
                }
            }
            break;
        }
    }
    if (Lower(letter) != 'c') {
        return 3;
    }
    bool present = false;
    int32_t value = 0;
    if (!ESpecNumber(&cursor, end, &present, &value)) {
        return 3;
    }
    if (present) {
        if (static_cast<uint32_t>(value - 1) > 6) {
            blz::log(g_logCategory, 4, s_eSpecFile, 0x236,
                "Invalid encoding specifier: invalid BCn '%d'. The valid range is [%u, %u]", value, 1, 7);
            return 3;
        }
        *bcn = value;
    }
    if (braced) {
        if (end <= cursor || *cursor != '}') {
            return 3;
        }
        do {
            cursor++;
        } while (cursor < end && IsSpace(*cursor));
    }
    return cursor == end ? 0 : 3;
}

// --- the encoding header ---------------------------------------------------------------------

// A single-block file: "BLTE" with a zero header size.
// ref: FUN_142129c50
bool CodecIsEncoded(const uint8_t* data, uint32_t size, uint8_t* error) {
    if (error) {
        *error = 0;
    }
    if (size < 8) {
        if (error) {
            *error = 7;
        }
        return false;
    }
    uint32_t magic;
    std::memcpy(&magic, data, 4);
    if (magic != s_blteMagic) {
        return false;
    }
    return ReadBE32(data + 4) == 0;
}

// 7 asks for more data (*headerSize says how much); the header's MD5 must be the e-key.
// ref: FUN_1420f92b0
uint8_t EncodingHeaderParse(const uint8_t* ekey, uint64_t ekeySize, const uint8_t* data, uint32_t size,
    ENCODINGHEADER** header, uint32_t* headerSize) {
    *headerSize = 0;
    if (size < 0x24) {
        return 7;
    }
    uint32_t magic;
    std::memcpy(&magic, data, 4);
    if (magic != s_blteMagic) {
        blz::log(g_logCategory, 4, s_headerFile, 0x31, "Failed to parse encoding header: invalid BLTE header bytes");
        return 8;
    }
    uint32_t total = ReadBE32(data + 4);
    if (total == 0) {
        return 8;
    }
    if (data[8] != 0xf) {
        blz::log(g_logCategory, 4, s_headerFile, 0x45,
            "Failed to parse encoding header: invalid encoding header format. Expected %02x, but got %02x", 0xf,
            data[8]);
        return 8;
    }
    uint32_t count = uint32_t(data[9]) << 16 | uint32_t(data[10]) << 8 | data[11];
    if (count == 0) {
        return 8;
    }
    uint32_t expected = count * 0x18 + 0xc;
    if (total != expected) {
        blz::log(g_logCategory, 4, s_headerFile, 0x56,
            "Failed to parse encoding header: invalid header size. Expected %u, but got %u", expected, total);
        return 8;
    }
    if (size < total) {
        *headerSize = total;
        return 7;
    }
    blz::md5_context md5;
    blz::md5_init(&md5);
    blz::md5_update(&md5, data, total);
    uint8_t hash[16];
    blz::md5_final(&md5, hash);
    if (std::memcmp(hash, ekey, ekeySize & 0xFF) != 0) {
        char text[0x21];
        blz::hex_encode(text, sizeof(text), ekey, 16);  // FUN_1420d8f40
        blz::log(g_logCategory, 4, s_headerFile, 0x6c,
            "Failed to parse encoding header: hash verification failed using e-key %s", text);
        return 8;
    }
    auto parsed = static_cast<ENCODINGHEADER*>(PackageAlloc(GetBufferAllocator(), 0x20, 8, false, s_noTag, 0));
    if (parsed) {
        std::memset(parsed, 0, sizeof(*parsed));
    }
    *header = parsed;
    auto block = static_cast<uint8_t*>(
        PackageAlloc(GetBufferAllocator(), uint64_t(count) * sizeof(ENCODINGBLOCK) + 8, 4, false, s_noTag, 0));
    ENCODINGBLOCK* blocks = nullptr;
    if (block) {
        uint64_t stored = count;
        std::memcpy(block, &stored, 8);
        blocks = reinterpret_cast<ENCODINGBLOCK*>(block + 8);
        for (uint32_t i = 0; i < count; i++) {
            std::memset(&blocks[i], 0, sizeof(ENCODINGBLOCK));
        }
    }
    (*header)->m_blocks = blocks;
    (*header)->m_blockCount = count;
    std::memcpy((*header)->m_hash, hash, 16);
    *headerSize = total;
    const uint8_t* entry = data + 0xc;
    for (uint32_t i = 0; i < count; i++) {
        ENCODINGBLOCK* out = &(*header)->m_blocks[i];
        out->m_encodedSize = ReadBE32(entry);
        out->m_decodedSize = ReadBE32(entry + 4);
        std::memcpy(out->m_hash, entry + 8, 16);
        entry += 0x18;
    }
    return 0;
}

// ref: FUN_1420f97a0
void EncodingHeaderFree(ENCODINGHEADER* header) {
    if (!header) {
        return;
    }
    if (header->m_blocks) {
        PackageFree(GetBufferAllocator(), reinterpret_cast<uint8_t*>(header->m_blocks) - 8);
    }
    PackageFree(GetBufferAllocator(), header);
}

// ref: FUN_1420f96e0
ENCODINGHEADER* EncodingHeaderCreate(const uint8_t* ekey, uint64_t ekeySize, const uint8_t* data, uint32_t size,
    uint32_t* headerSize, uint8_t* error) {
    uint8_t local = 0;
    uint8_t* result = error ? error : &local;
    ENCODINGHEADER* header = nullptr;
    *result = EncodingHeaderParse(ekey, ekeySize, data, size, &header, headerSize);
    if (*result == 0) {
        return header;
    }
    EncodingHeaderFree(header);
    if (*result != 7) {
        *headerSize = 0;
    }
    return nullptr;
}

// ref: FUN_1420f9a00
uint32_t EncodingHeaderSize(uint32_t blockCount) {
    return blockCount * 0x18 + 0xc;
}

// ref: FUN_1420f9a10
uint8_t EncodingHeaderMapSpan(const ENCODINGHEADER* header, const uint32_t cSpan[2], uint32_t eSpan[2],
    uint32_t* aligned) {
    uint32_t local[2] = { 0, 0 };
    eSpan[0] = 0;
    eSpan[1] = 0;
    uint32_t* out = aligned ? aligned : local;
    if (!header) {
        return 3;
    }
    if (cSpan[1] == 0) {
        return 0;
    }
    bool found = false;
    uint32_t encoded = 0;
    uint32_t decoded = 0;
    for (uint32_t i = 0; i < header->m_blockCount; i++) {
        const ENCODINGBLOCK* block = &header->m_blocks[i];
        if (!found && cSpan[0] < block->m_decodedSize + decoded) {
            eSpan[0] = encoded;
            found = true;
            out[0] = decoded;
        }
        if (found && cSpan[0] + cSpan[1] <= block->m_decodedSize + decoded) {
            eSpan[1] = (block->m_encodedSize - eSpan[0]) + encoded;
            out[1] = (block->m_decodedSize - out[0]) + decoded;
            eSpan[0] += EncodingHeaderSize(header->m_blockCount);
            return 0;
        }
        encoded += block->m_encodedSize;
        decoded += block->m_decodedSize;
    }
    return 3;
}

// ref: FUN_1420f97f0
uint8_t EncodingHeaderBlockRange(const ENCODINGHEADER* header, const uint32_t eSpan[2], uint32_t* first,
    uint32_t* end, uint32_t cSpan[2]) {
    uint32_t start = eSpan[0];
    uint32_t size = eSpan[1];
    bool found = false;
    uint32_t position = EncodingHeaderSize(header->m_blockCount);
    uint32_t decoded = 0;
    for (uint32_t i = 0; i < header->m_blockCount; i++) {
        const ENCODINGBLOCK* block = &header->m_blocks[i];
        if (!found && position == start) {
            *first = i;
            found = true;
            cSpan[0] = decoded;
        }
        if (found && block->m_encodedSize + position == size + start) {
            *end = i + 1;
            cSpan[1] = (block->m_decodedSize - cSpan[0]) + decoded;
            return 0;
        }
        position += block->m_encodedSize;
        decoded += block->m_decodedSize;
    }
    return 3;
}

// --- decoding --------------------------------------------------------------------------------

// ref: FUN_1420fa800
bool CodecVerifyHash(const uint8_t* hash, uint64_t hashSize, const void* data, uint32_t size) {
    blz::md5_context md5;
    blz::md5_init(&md5);
    blz::md5_update(&md5, data, size);
    uint8_t computed[16];
    blz::md5_final(&md5, computed);
    return std::memcmp(hash, computed, hashSize & 0xFF) == 0;
}

// ref: FUN_14212d4c0
uint8_t CodecUncompressed(void* out, uint32_t outSize, const uint8_t* data, uint32_t size, uint8_t codecByte) {
    if (codecByte <= size) {
        if (outSize < size - codecByte) {
            return 4;
        }
        if (codecByte == 0 || data[0] == 'N') {
            std::memcpy(out, data + codecByte, size - codecByte);
            return 0;
        }
        blz::log(g_logCategory, 4, s_uncompressedFile, 0x32,
            "Failed decode uncompressed: invalid uncompressed codec '%c'. Expected '%c'", data[0], 'N');
    }
    return 8;
}

// A streamed inflate goes through a 4 KB staging buffer.
// ref: FUN_14212ddc0
uint8_t CodecZlib(void* out, uint32_t outSize, const uint8_t* data, uint32_t size, uint8_t codecByte, uint8_t streamed,
    DECODERHOOK* hook) {
    if (codecByte && (size < 2 || data[0] != 'Z')) {
        blz::log(g_logCategory, 4, s_zlibFile, 300, "Failed decode zlib: invalid zlib codec '%c'. Expected '%c'",
            data[0], 'Z');
        return 8;
    }
    if (hook && !hook->Decode(out, outSize, data, size, &codecByte)) {
        return 0;
    }
    z_stream stream;
    uint8_t initialized = ZlibInit(&stream);
    if (initialized != 0) {
        return initialized;
    }
    size -= codecByte;
    int status = Z_OK;
    uint8_t result;
    bool done = false;
    if (!streamed) {
        stream.next_in = const_cast<Bytef*>(data + codecByte);
        stream.avail_in = size;
        stream.next_out = static_cast<Bytef*>(out);
        stream.avail_out = outSize;
        status = inflate(&stream, Z_FINISH);
        done = status == Z_STREAM_END;
    } else {
        uint8_t staging[0x1000];
        uint32_t written = 0;
        uint32_t consumed = 0;
        for (;;) {
            if ((outSize <= written && size <= consumed) || status != Z_OK) {
                break;
            }
            uint32_t chunk = outSize - written;
            if (chunk > 0x1000) {
                chunk = 0x1000;
            }
            stream.next_in = const_cast<Bytef*>(data + codecByte + consumed);
            stream.avail_in = size - consumed;
            stream.next_out = staging;
            stream.avail_out = chunk;
            int step = inflate(&stream, Z_FINISH);
            if (step != Z_BUF_ERROR) {
                status = step;
            }
            consumed = size - stream.avail_in;
            uint32_t got = chunk - stream.avail_out;
            if (got != 0) {
                std::memcpy(static_cast<uint8_t*>(out) + written, staging, got);
            }
            written += got;
            if (status == Z_STREAM_END) {
                done = true;
                break;
            }
            if (got == 0) {
                break;
            }
        }
    }
    if (done) {
        result = 0;
    } else {
        if (status == Z_STREAM_ERROR) {
            blz::log(g_logCategory, 3, s_zlibFile, 0x183, "Potential memory stomp detected during decoding. Error (%d): %s",
                Z_STREAM_ERROR, stream.msg ? stream.msg : "error");
        }
        blz::log(g_logCategory, 4, s_zlibFile, 0x188, "Zlib inflate failed. Error (%d): %s", status,
            stream.msg ? stream.msg : "error");
        if (status == Z_BUF_ERROR) {
            result = 4;
        } else if (status == Z_MEM_ERROR || status != Z_OK) {
            result = 1;
        } else {
            result = 3;
        }
    }
    inflateEnd(&stream);
    return result;
}

// ref: FUN_14212d2d0
uint8_t CodecLz4hc(void* out, uint32_t outSize, const uint8_t* data, uint32_t size, uint8_t codecByte, uint8_t a6,
    SCRATCHBUFFER* scratch) {
    (void)out;
    (void)outSize;
    (void)data;
    (void)size;
    (void)codecByte;
    (void)a6;
    (void)scratch;
    blz::log(g_logCategory, 1, s_lz4hcFile, 0x32b, "Attempted to use lz4hc encoding without enabling.");
    return 5;
}

// ref: FUN_14212c3a0
uint8_t CodecBcPack(void* out, uint32_t outSize, const uint8_t* data, uint32_t size, int32_t bcn, DECODERHOOK* hook) {
    (void)hook;
    return BcPackDecompress(BcPackLibrary(), out, outSize, data, size, bcn);
}

// 'E', key name length (8), key name, IV length, IV, algorithm, then the encrypted block.
// ref: FUN_14212ceb0
uint8_t EncryptionHeaderSize(const uint8_t* data, uint32_t size, uint32_t* headerSize) {
    if (size < 0xc) {
        blz::log(g_logCategory, 4, s_encryptionFile, 0xb1,
            "Failed to read encryption header size: buffer too small to contain header. Expected at least %u bytes, but got %u",
            0xc, size);
        return 8;
    }
    if (data[0] != 'E') {
        blz::log(g_logCategory, 4, s_encryptionFile, 0xba,
            "Failed to read encryption header size: invalid encryption codec '%c'. Expected '%c'", data[0], 'E');
        return 8;
    }
    uint32_t ivLength = data[10];
    if (ivLength < 9) {
        *headerSize = ivLength + 0xc;
        return 0;
    }
    blz::log(g_logCategory, 4, s_encryptionFile, 0xc4,
        "Failed to read encryption header size: IV length is invalid. Expected <= %u bytes, but got %u", 8, ivLength);
    return 8;
}

// Decrypts an 'E' block's payload in place (Salsa20, the IV's first word XORed with the block
// index).
// ref: FUN_14212cf60
uint8_t CodecDecrypt(uint8_t* data, uint32_t size, uint32_t payloadSize, uint64_t blockIndex, KEYGETTER keyGetter,
    void* keyContext) {
    if (size < 0xc) {
        return 4;
    }
    if (!keyGetter) {
        blz::log(g_logCategory, 4, s_encryptionFile, 0x111, "Failed to decrypt data: no key getter was specified");
        return 0xd;
    }
    const char* message;
    int32_t line;
    uint32_t expected;
    uint32_t got;
    if (data[0] != 'E') {
        message = "Failed to decrypt data: invalid encryption codec '%c'. Expected '%c'";
        line = 0x11b;
        expected = data[0];
        got = 'E';
    } else if (data[1] != 8) {
        message = "Failed to decrypt data: invalid encryption key length. Expected %u, but got %u";
        line = 0x123;
        expected = 8;
        got = data[1];
    } else {
        uint8_t name[8];
        std::memcpy(name, data + 2, 8);
        uint8_t key[16];
        if (!keyGetter(keyContext, name, key)) {
            LogMissingKey(name);
            return 0xd;
        }
        uint32_t ivLength = data[10];
        if (ivLength < 9) {
            if (payloadSize + 0xc + ivLength > size) {
                return 4;
            }
            uint8_t iv[8];
            std::memcpy(iv, data + 0xb, ivLength);
            if (ivLength < 8) {
                std::memset(iv + ivLength, 0, 8 - ivLength);
            }
            if (data[ivLength + 0xb] != 'S') {
                blz::log(g_logCategory, 4, s_encryptionFile, 0x147,
                    "Failed to decrypt data: unknown encryption algorithm specifier '%c'", data[ivLength + 0xb]);
                return 8;
            }
            iv[0] ^= static_cast<uint8_t>(blockIndex);
            iv[1] ^= static_cast<uint8_t>(blockIndex >> 8);
            iv[2] ^= static_cast<uint8_t>(blockIndex >> 16);
            iv[3] ^= static_cast<uint8_t>(blockIndex >> 24);
            uint32_t keyWords[4];
            std::memcpy(keyWords, key, 16);
            uint32_t ivWords[2];
            std::memcpy(ivWords, iv, 8);
            SALSA20 state;
            Salsa20Setup(&state, keyWords, ivWords, 0);
            Salsa20Process(&state, data + ivLength + 0xc, payloadSize);
            return 0;
        }
        message = "Failed to decrypt data: invalid encryption IV length. Expected <= %u, but got %u";
        line = 0x136;
        expected = 8;
        got = ivLength;
    }
    blz::log(g_logCategory, 4, s_encryptionFile, line, message, expected, got);
    return 8;
}

// One block: verified against its hash, then decoded by its codec byte.
// ref: FUN_1420f9ef0
uint8_t CodecDecodeEncoded(void* out, uint32_t outSize, const uint8_t* data, uint32_t size, uint32_t blockIndex,
    const uint8_t* blockHash, uint8_t hashSize, uint8_t a8, KEYGETTER keyGetter, void* keyContext, uint8_t verify,
    SCRATCHBUFFER* scratch, DECODERHOOK* hook) {
    if (verify && !CodecVerifyHash(blockHash, hashSize, data, size)) {
        char text[0x21];
        blz::hex_encode(text, sizeof(text), blockHash, 16);  // FUN_1420d8f40
        blz::log(g_logCategory, 4, s_decodingFile, 0x1e7,
            "Failed decode block (%u): hash verification failed using e-key %s", blockIndex, text);
        return 8;
    }
    switch (data[0]) {
    case '4':
        return CodecLz4hc(out, outSize, data, size, 1, a8, scratch);
    case 'E': {
        uint32_t headerSize = 0;
        uint8_t result = EncryptionHeaderSize(data, size, &headerSize);
        if (result != 0) {
            return result;
        }
        // The block is decrypted where it lies.
        result = CodecDecrypt(const_cast<uint8_t*>(data), size, size - headerSize, blockIndex, keyGetter, keyContext);
        if (result != 0) {
            return result;
        }
        uint32_t inner = static_cast<uint32_t>(data[headerSize] - 0x34);
        if (static_cast<uint8_t>(inner) < 0x27 && ((0x4004000001ull >> (inner & 0x3F)) & 1) != 0) {
            return CodecDecodeEncoded(out, outSize, data + headerSize, size - headerSize, blockIndex, blockHash,
                hashSize, a8, keyGetter, keyContext, 0, scratch, hook);
        }
        blz::log(g_logCategory, 4, s_decodingFile, 0x211,
            "Failed decode block (%u): invalid codec '%c' in decrypted block. Expected one of: '%c', '%c', or '%c'",
            blockIndex, data[headerSize], 'N', 'Z', '4');
        return 8;
    }
    case 'F':
        blz::log(g_logCategory, 4, s_decodingFile, 0x229,
            "Failed decode block (%u): recursive block encoding is no longer supported", blockIndex);
        return 5;
    case 'N':
        return CodecUncompressed(out, outSize, data, size, 1);
    case 'Z':
        return CodecZlib(out, outSize, data, size, 1, a8, hook);
    default:
        blz::log(g_logCategory, 4, s_decodingFile, 0x230, "Failed decode block (%u): unknown block codec '%c'",
            blockIndex, data[0]);
        return 8;
    }
}

// Decodes a run of blocks; the indices of corrupt ones (and of all after a short input) are
// collected.
// ref: FUN_1420f9d00
uint8_t DecodeBlocks(void* out, uint32_t outSize, const uint8_t* data, uint32_t size, ENCODINGHEADER* header,
    uint32_t firstBlock, const ENCODINGBLOCK* begin, const ENCODINGBLOCK* end, U32SET* corrupt, uint8_t a10,
    KEYGETTER keyGetter, void* keyContext, uint8_t verify, SCRATCHBUFFER* scratch, DECODERHOOK* hook) {
    (void)header;
    uint32_t consumed = 0;
    uint32_t written = 0;
    bool failed = false;
    uint32_t index = firstBlock;
    const ENCODINGBLOCK* block = begin;
    while (block < end) {
        if (size <= consumed || size - consumed < block->m_encodedSize) {
            failed = true;
            break;
        }
        if (outSize <= written || outSize - written < block->m_decodedSize) {
            return 4;
        }
        uint8_t result = CodecDecodeEncoded(static_cast<uint8_t*>(out) + written, block->m_decodedSize, data + consumed,
            block->m_encodedSize, index, block->m_hash, 0x10, a10, keyGetter, keyContext, verify, scratch, hook);
        if (result == 0xd) {
            return 0xd;
        }
        if (result != 0) {
            failed = true;
            if (corrupt) {
                corrupt->Insert(index, nullptr);
            }
        }
        consumed += block->m_encodedSize;
        index++;
        written += block->m_decodedSize;
        block++;
    }
    if (corrupt && block < end) {
        for (; block < end; block++) {
            corrupt->Insert(index, nullptr);
            index++;
        }
    }
    return failed ? 8 : 0;
}

// ref: FUN_1420fa290
uint8_t CodecDecode(void* out, uint32_t outSize, const uint8_t* data, uint32_t size, const uint8_t* ekey,
    const char* eSpec, uint8_t hashSize, uint8_t a8, KEYGETTER keyGetter, void* keyContext, uint8_t verify,
    SCRATCHBUFFER* scratch, DECODERHOOK* hook, U32SET* corrupt) {
    if (!out || !data || size == 0) {
        return 3;
    }
    uint8_t type = ESPEC_BLOCKS;
    if (eSpec && *eSpec) {
        type = ESpecType(eSpec, eSpec + std::strlen(eSpec));
        if (type != ESPEC_BLOCKS && verify && !CodecVerifyHash(ekey, hashSize, data, size)) {
            char text[0x21];
            blz::hex_encode(text, sizeof(text), ekey, 16);  // FUN_1420d8f40
            blz::log(g_logCategory, 4, s_decodingFile, 0x145, "Failed decode file: hash verification failed using e-key %s",
                text);
            return 8;
        }
    }
    switch (type) {
    case ESPEC_NONE:
        return CodecUncompressed(out, outSize, data, size, 0);
    case ESPEC_ZLIB:
        return CodecZlib(out, outSize, data, size, 0, a8, hook);
    case ESPEC_LZ4HC:
        return CodecLz4hc(out, outSize, data, size, 0, a8, scratch);
    case ESPEC_BCPACK: {
        int32_t bcn = 1;
        if (ESpecParseBcn(eSpec, nullptr, &bcn) == 0) {
            return CodecBcPack(out, outSize, data, size, bcn, hook);
        }
        return 3;
    }
    case ESPEC_BLOCKS: {
        uint32_t headerSize = 0;
        uint8_t error = 0;
        ENCODINGHEADER* header = EncodingHeaderCreate(ekey, hashSize, data, size, &headerSize, &error);
        if (error != 0) {
            return error;
        }
        uint8_t result = DecodeBlocks(out, outSize, data + headerSize, size - headerSize, header, 0, header->m_blocks,
            header->m_blocks + header->m_blockCount, corrupt, a8, keyGetter, keyContext, verify, scratch, hook);
        EncodingHeaderFree(header);
        return result;
    }
    default:
        return 3;
    }
}

// ref: FUN_1420fa5d0
uint8_t CodecDecodeSpan(void* out, const uint32_t cSpan[2], const uint8_t* data, const uint32_t eSpan[2],
    const ENCODINGHEADER* header, U32SET* corrupt, uint8_t a7, KEYGETTER keyGetter, void* keyContext, uint8_t verify,
    SCRATCHBUFFER* scratch, DECODERHOOK* hook) {
    uint32_t position = EncodingHeaderSize(header->m_blockCount);
    uint32_t end = eSpan[1] + eSpan[0];
    if (header->m_blockCount == 0) {
        return 3;
    }
    bool found = false;
    uint32_t first = 0;
    uint32_t decoded = 0;
    uint32_t start = 0;
    uint32_t i = 0;
    while (position <= end) {
        const ENCODINGBLOCK* block = &header->m_blocks[i];
        if (!found && position == eSpan[0]) {
            found = true;
            first = i;
            start = decoded;
        }
        if (found && block->m_encodedSize + position == end) {
            uint32_t want = cSpan[1];
            decoded += block->m_decodedSize - start;
            if (decoded < want || cSpan[0] < start || (want == decoded && cSpan[0] != start)) {
                return 3;
            }
            uint8_t* target = static_cast<uint8_t*>(out);
            uint8_t* staging = nullptr;
            if (want < decoded) {
                staging = static_cast<uint8_t*>(PackageAlloc(GetBufferAllocator(), decoded, 0x10, false, s_noTag, 0));
                a7 = 0;
                target = staging;
            }
            uint8_t result = DecodeBlocks(target, decoded, data, eSpan[1], const_cast<ENCODINGHEADER*>(header), first,
                &header->m_blocks[first], &header->m_blocks[i + 1], corrupt, a7, keyGetter, keyContext, verify, scratch,
                hook);
            if (result == 0) {
                if (want < decoded) {
                    std::memcpy(out, target + (cSpan[0] - start), want);
                }
                result = 0;
            }
            if (staging) {
                PackageFree(GetBufferAllocator(), staging);
            }
            return result;
        }
        decoded += block->m_decodedSize;
        i++;
        position += block->m_encodedSize;
        if (header->m_blockCount <= i) {
            return 3;
        }
    }
    return 3;
}

// ref: FUN_1420fa210
uint8_t CodecDecodeRaw(void* out, uint32_t outSize, const uint8_t* data, uint32_t size, const uint8_t* ekey,
    const char* eSpec, uint8_t hashSize, uint8_t a8, KEYGETTER keyGetter, void* keyContext, uint8_t verify) {
    return CodecDecode(out, outSize, data, size, ekey, eSpec, hashSize, a8, keyGetter, keyContext, verify, nullptr,
        nullptr, nullptr);
}

}

namespace tact {

namespace {

void SkipSpace(const char** cursor, const char* end) {
    while (*cursor < end && IsSpace(**cursor)) {
        (*cursor)++;
    }
}

ESPECBLOCK* AllocateBlocks(uint32_t count) {
    return static_cast<ESPECBLOCK*>(PackageAlloc(GetBufferAllocator(), uint64_t(count) * sizeof(ESPECBLOCK), 8, false, "", 0));
}

// FUN_1420d7700: doubles the block array.
uint32_t GrowBlocks(ESPECBLOCK** blocks, uint32_t count) {
    ESPECBLOCK* grown = AllocateBlocks(count * 2);
    std::memcpy(grown, *blocks, uint64_t(count) * sizeof(ESPECBLOCK));
    PackageFree(GetBufferAllocator(), *blocks);
    *blocks = grown;
    return count * 2;
}

}

// ref: FUN_1420d77a0
bool ESpecBlockToken(const char** cursor, const char* end, int32_t* size, int32_t* count, char* variable) {
    *size = 0;
    *count = 0;
    *variable = 0;
    SkipSpace(cursor, end);
    if (end <= *cursor) {
        return false;
    }
    if (static_cast<uint8_t>(**cursor - '0') < 10) {
        const char* next = nullptr;
        int32_t value = static_cast<int32_t>(ParseUnsigned(*cursor, &next, 10, nullptr));
        *size = value;
        if (value == 0) {
            blz::log(g_logCategory, 4, s_eSpecFile, 0x9e, "Invalid encoding specifier: block size must not be zero");
            return false;
        }
        *cursor = next;
        SkipSpace(cursor, end);
        if (end <= *cursor) {
            return false;
        }
        char c = **cursor;
        if (c == 'K' || c == 'k' || c == 'M' || c == 'm') {
            *size = value << 10;
            if (c == 'M' || c == 'm') {
                *size = value << 20;
            }
            (*cursor)++;
        }
    }
    SkipSpace(cursor, end);
    if (end <= *cursor) {
        return false;
    }
    if (**cursor == '=') {
        *count = 1;
        return true;
    }
    if (**cursor != '*') {
        return false;
    }
    (*cursor)++;
    SkipSpace(cursor, end);
    if (end <= *cursor) {
        return false;
    }
    if (*size == 0) {
        if (**cursor != '=') {
            return false;
        }
        *count = 1;
        *variable = 1;
        return true;
    }
    if (static_cast<uint8_t>(**cursor - '0') < 10) {
        const char* next = nullptr;
        *count = static_cast<int32_t>(ParseUnsigned(*cursor, &next, 10, nullptr));
        if (*count == 0) {
            blz::log(g_logCategory, 4, s_eSpecFile, 0xd4, "Invalid encoding specifier: block count must not be zero");
            return false;
        }
        *cursor = next;
    }
    if (*count == 0) {
        *variable = 1;
    }
    // FUN_1420d7bc0
    SkipSpace(cursor, end);
    return *cursor < end && **cursor == '=';
}

// ref: FUN_1420d7bf0
void ESpecFreeBlocks(ESPECBLOCK* blocks) {
    PackageFree(GetBufferAllocator(), blocks);
}

// ref: FUN_1420d7e30
uint8_t ESpecParseBlocks(uint32_t csize, const char* spec, const char* end, ESPECBLOCK** blocks, int32_t* count) {
    *blocks = nullptr;
    *count = 0;
    int32_t variableIndex = -1;
    if (end == nullptr) {
        end = spec + std::strlen(spec);
    }
    bool colon = false;
    bool braced = false;
    const char* cursor = spec;
    while (true) {
        if (end <= cursor) {
            return 3;
        }
        if (!IsSpace(*cursor)) {
            break;
        }
        cursor++;
    }
    char type = *cursor;
    do {
        cursor++;
    } while (cursor < end && IsSpace(*cursor));
    if (cursor < end) {
        if (*cursor != ':') {
            return 3;
        }
        colon = true;
        do {
            cursor++;
        } while (cursor < end && IsSpace(*cursor));
        if (cursor < end && *cursor == '{') {
            cursor++;
            braced = true;
            if (end <= cursor) {
                return 3;
            }
            while (IsSpace(*cursor)) {
                cursor++;
                if (end <= cursor) {
                    return 3;
                }
            }
        }
    }
    if (static_cast<uint8_t>(type - 'A') < 26) {
        type = static_cast<char>(type + 0x20);
    }
    if (type != 'b' || !colon) {
        return 3;
    }
    uint32_t capacity = 0x10;
    *blocks = AllocateBlocks(capacity);
    uint32_t total = 0;
    int32_t index = 0;
    bool failed = false;
    while (cursor < end) {
        int32_t size = 0;
        int32_t blockCount = 0;
        char variable = 0;
        if (!ESpecBlockToken(&cursor, end, &size, &blockCount, &variable)) {
            failed = true;
            break;
        }
        const char* blockSpec = cursor + 1;
        total += static_cast<uint32_t>(blockCount * size);
        cursor = blockSpec;
        if (variable != 0) {
            if (variableIndex != -1) {
                blz::log(g_logCategory, 4, s_eSpecFile, 0x2e7,
                    "Invalid encoding specifier: multiple variable blocks detected in e-spec '%s'", spec);
                failed = true;
                break;
            }
            variableIndex = index;
        }
        int32_t depth = 0;
        bool closed = false;
        while (cursor < end) {
            char c = *cursor;
            if (c == '{') {
                depth++;
            } else if (c == '}') {
                depth--;
                if (depth < 1) {
                    if (depth == 0) {
                        cursor++;
                    }
                    closed = true;
                    break;
                }
            } else if (c == ',' && depth == 0) {
                closed = true;
                break;
            }
            cursor++;
        }
        if (!closed && depth != 0) {
            failed = true;
            break;
        }
        ESPECBLOCK* block = &(*blocks)[index];
        block->m_size = static_cast<uint32_t>(size);
        block->m_count = static_cast<uint32_t>(blockCount);
        block->m_spec = blockSpec;
        block->m_specEnd = cursor;
        index++;
        if (static_cast<int32_t>(capacity) <= index) {
            capacity = GrowBlocks(blocks, capacity);
        }
        if (variable != 0 && blockCount == 0) {
            ESPECBLOCK* remainder = &(*blocks)[index];
            remainder->m_size = 0;
            remainder->m_count = 0;
            remainder->m_spec = nullptr;
            remainder->m_specEnd = nullptr;
            index++;
            if (static_cast<int32_t>(capacity) <= index) {
                capacity = GrowBlocks(blocks, capacity);
            }
        }
        if (*cursor == '}' || cursor == end) {
            break;
        }
        if (*cursor != ',') {
            failed = true;
            break;
        }
        cursor++;
    }
    if (!failed) {
        bool finish = braced ? *cursor == '}' : *cursor != '}';
        if (finish) {
            *count = index;
            if (variableIndex == -1) {
                if (csize == total) {
                    return 0;
                }
                blz::log(g_logCategory, 4, s_eSpecFile, 0x321,
                    "Invalid encoding specifier: block encoding doesn't match raw size for e-spec '%s'", spec);
            } else if (total <= csize) {
                uint32_t remaining = csize - total;
                ESPECBLOCK* block = &(*blocks)[variableIndex];
                uint32_t size = block->m_size;
                if (size != 0) {
                    ESPECBLOCK* next = &(*blocks)[variableIndex + 1];
                    block->m_count = remaining / size;
                    next->m_size = remaining % size;
                    next->m_count = 1;
                    next->m_spec = block->m_spec;
                    next->m_specEnd = block->m_specEnd;
                    return 0;
                }
                block->m_size = remaining;
                return 0;
            } else {
                blz::log(g_logCategory, 4, s_eSpecFile, 0x32a,
                    "Invalid encoding specifier: variable block encoding doesn't match raw size for e-spec '%s'", spec);
            }
        }
    }
    PackageFree(GetBufferAllocator(), *blocks);
    *blocks = nullptr;
    *count = 0;
    return 3;
}

// ref: FUN_1420f9950
uint8_t ESpecEncodingHeaderSize(const char* spec, uint32_t csize, uint32_t* headerSize) {
    ESPECBLOCK* blocks = nullptr;
    int32_t count = 0;
    uint8_t error = ESpecParseBlocks(csize, spec, spec + std::strlen(spec), &blocks, &count);
    if (error != 0) {
        return error;
    }
    uint32_t total = 0;
    for (int32_t i = 0; i < count; i++) {
        if (blocks[i].m_size != 0) {
            total += blocks[i].m_count;
        }
    }
    if (total > 0xFFFFFF) {
        error = 3;
    } else {
        error = 0;
        *headerSize = total * 0x18 + 0xc;
    }
    ESpecFreeBlocks(blocks);
    return error;
}

}
