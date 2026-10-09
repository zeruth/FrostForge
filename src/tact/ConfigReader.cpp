#include "tact/ConfigReader.hpp"
#include "blz/Parse.hpp"
#include "tact/Common.hpp"
#include "tact/Instance.hpp"
#include "blz/AllocString.hpp"
#include "blz/Log.hpp"
#include <cstring>

namespace tact {

namespace {

const char s_readerFile[] = "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\config\\config_reader.cpp";

bool IsSpace(char c) {
    return c == ' ' || static_cast<uint8_t>(c - 9) < 5;
}

// The key comparisons compare at most the key's length, stopping at the literal's end: a key
// that is a prefix of the literal matches it, as in the reference.
bool KeyIs(const char* key, uint32_t length, const char* literal) {
    for (uint32_t i = 0; i < length; i++) {
        if (key[i] != literal[i]) {
            return false;
        }
        if (literal[i] == '\0') {
            return true;
        }
    }
    return true;
}

void SkipSpace(const char** text, const char* end) {
    while (*text < end && IsSpace(**text)) {
        (*text)++;
    }
}

// "<ckey> <ekey>" into a record.
// ref: FUN_142128ec0
bool ParseRecordKeys(const char* name, const char** text, const char* end, CONFIGRECORD* record) {
    SkipSpace(text, end);
    const char* message = "Invalid build config. Missing or invalid C-Key value for manfiest key '%s'.";
    int32_t line = 0x180;
    if (*text < end) {
        bool error = false;
        uint64_t used = ParseKey16(*text, static_cast<uint64_t>(end - *text), record->m_ckey, &error);
        if (!error) {
            *text += used;
            // FUN_142127850
            bool ok = false;
            SkipSpace(text, end);
            if (*text < end) {
                bool keyError = false;
                uint64_t keyUsed = ParseKey16(*text, static_cast<uint64_t>(end - *text), record->m_ekey, &keyError);
                if (!keyError) {
                    *text += keyUsed;
                    ok = true;
                }
            }
            if (ok) {
                return true;
            }
            message = "Invalid build config. Missing or invalid E-Key value for manfiest key '%s'.";
            line = 0x186;
        }
    }
    blz::log(g_logCategory, 4, s_readerFile, line, message, name);
    return false;
}

// FUN_1421277e0: whitespace, then a 32-bit decimal.
bool ParseNextUInt32(const char** text, const char* end, uint32_t* value) {
    SkipSpace(text, end);
    if (*text < end) {
        bool error = false;
        uint64_t used = ParseUInt32(*text, static_cast<uint64_t>(end - *text), value, &error);
        if (!error) {
            *text += used;
            return true;
        }
    }
    return false;
}

// "<csize> <esize>" into a record.
// ref: FUN_142128fb0
bool ParseRecordSizes(const char* name, const char** text, const char* end, CONFIGRECORD* record) {
    SkipSpace(text, end);
    const char* message = "Invalid build config. Missing or invalid C-Size value for manfiest key '%s'.";
    int32_t line = 0x191;
    if (*text < end) {
        bool error = false;
        uint64_t used = ParseUInt32(*text, static_cast<uint64_t>(end - *text), &record->m_csize, &error);
        if (!error) {
            *text += used;
            if (ParseNextUInt32(text, end, &record->m_esize)) {
                return true;
            }
            message = "Invalid build config. Missing or invalid E-Size value for manfiest key '%s'.";
            line = 0x197;
        }
    }
    blz::log(g_logCategory, 4, s_readerFile, line, message, name);
    return false;
}

// ref: FUN_142128e40
bool ParseRecordESpec(const char* name, const char** text, const char* end, CONFIGRECORD* record) {
    SkipSpace(text, end);
    if (*text < end) {
        record->m_eSpec.Assign(*text, static_cast<uint32_t>(end - *text));
        return true;
    }
    blz::log(g_logCategory, 4, s_readerFile, 0x1a4,
        "Invalid build config. Missing or invalid E-Spec value for manifest key '%s'.", name);
    return false;
}

// ref: FUN_1421275a0
bool ParseSingleKey(const char* name, const char** text, const char* end, uint8_t key[16]) {
    SkipSpace(text, end);
    if (*text < end) {
        bool error = false;
        uint64_t used = ParseKey16(*text, static_cast<uint64_t>(end - *text), key, &error);
        if (!error) {
            *text += used;
            return true;
        }
    }
    blz::log(g_logCategory, 4, s_readerFile, 0x159, "Invalid build config. Missing or invalid value for key '%s'.", name);
    return false;
}

// "key:size key:size ..."
// ref: FUN_1421290a0
bool ParsePartialPriority(const char* name, const char** text, const char* end, Array<PARTIALPRIORITY>* list) {
    (void)name;
    const char* at = *text;
    if (at >= end) {
        return true;
    }
    for (;;) {
        while (IsSpace(*at)) {
            at++;
            *text = at;
            if (at >= end) {
                break;
            }
        }
        if (*text >= end) {
            return true;
        }
        list->Reserve(list->m_count + 1);
        PARTIALPRIORITY* entry = &list->m_data[list->m_count];
        std::memset(entry, 0, sizeof(*entry));
        list->m_count++;
        at = *text;
        const char* colon = at;
        for (;;) {
            if (*colon == '\0') {
                blz::log(g_logCategory, 4, s_readerFile, 0x1bb, "Invalid build config. Malformed partial priority entry.");
                return false;
            }
            if (*colon == ':') {
                break;
            }
            colon++;
        }
        while (at < colon && IsSpace(*at)) {
            at++;
            *text = at;
        }
        at = *text;
        if (colon <= at) {
            blz::log(g_logCategory, 4, s_readerFile, 0x1c1, "Invalid build config. Bad partial priority key.");
            return false;
        }
        bool error = false;
        uint64_t used = ParseKey16(at, static_cast<uint64_t>(colon - at), entry->m_key, &error);
        if (error) {
            blz::log(g_logCategory, 4, s_readerFile, 0x1c1, "Invalid build config. Bad partial priority key.");
            return false;
        }
        *text += used + 1;
        at = *text;
        const char* digits = colon;
        do {
            digits++;
        } while (static_cast<uint8_t>(*digits - '0') < 10);
        while (at < digits && IsSpace(*at)) {
            at++;
            *text = at;
        }
        at = *text;
        if (digits <= at) {
            blz::log(g_logCategory, 4, s_readerFile, 0x1cb, "Invalid build config. Bad partial priority size.");
            return false;
        }
        bool sizeError = false;
        used = ParseUInt32(at, static_cast<uint64_t>(digits - at), &entry->m_size, &sizeError);
        if (sizeError) {
            blz::log(g_logCategory, 4, s_readerFile, 0x1cb, "Invalid build config. Bad partial priority size.");
            return false;
        }
        *text += used;
        at = *text;
        if (at >= end) {
            return true;
        }
    }
}

// Byte array append (FUN_1421096c0): inserts at the end, growing with FUN_1420d9bc0.
void AppendBytes(Array<char>* array, const char* data, uint32_t length) {
    uint32_t at = array->m_count;
    if (array->m_capacity < at + length) {
        array->Reserve(at + length);
    }
    array->m_count += length;
    std::memcpy(array->m_data + at, data, length);
}

// Grows the vfs record list to `count` (FUN_142129850): new records are initialized, records
// past it destroyed.
// ref: FUN_142129850
void ResizeVfs(Array<CONFIGRECORD>* list, uint32_t count) {
    if (count < list->m_count) {
        for (uint32_t i = count; i < list->m_count; i++) {
            list->m_data[i].m_eSpec.Destroy();
        }
    } else {
        // FUN_1420dece0 moves the records into the new block; for strings that share the
        // allocator that is the memcpy Array::Reserve does.
        list->Reserve(count);
        for (uint32_t i = list->m_count; i < count; i++) {
            list->m_data[i].Init();
        }
    }
    list->m_count = count;
}

}

// The packages' decimal parse (blz::parse_uint32).
uint64_t ParseUInt32(const char* text, uint64_t length, uint32_t* value, bool* error) {
    return blz::parse_uint32(text, length, value, error);
}

// ref: FUN_142363210
uint64_t ParseUInt8(const char* text, uint64_t length, uint8_t* value, bool* error) {
    *value = 0;
    *error = false;
    if (length == 0) {
        return 0;
    }
    uint64_t at = 0;
    char first;
    while ((first = text[at]), IsSpace(first)) {
        at++;
        if (length <= at) {
            return 0;
        }
    }
    if (length <= at) {
        return 0;
    }
    uint64_t start = at + (((first - '+') & 0xFD) == 0 ? 1 : 0);
    const char* digits = text + start;
    const char* p = digits;
    uint8_t result = 0;
    bool overflow = false;
    while (p < text + length && static_cast<uint8_t>(*p - '0') < 10) {
        uint8_t next = static_cast<uint8_t>((*p - '0') + result * 10);
        if (next < result) {
            overflow = true;
        }
        result = next;
        p++;
    }
    uint64_t count = static_cast<uint64_t>(p - digits);
    if (count == 0) {
        return 0;
    }
    if (first != '-') {
        if (overflow) {
            result = 0xFF;
            *error = true;
        }
        *value = result;
        return start + count;
    }
    if (overflow || result != 0) {
        *error = true;
    }
    *value = 0;
    return start + count;
}

// Lines are trimmed; empty lines and '#' comments are skipped; the key runs to the first
// whitespace or '=', the value starts after any whitespace and '='.
// ref: FUN_142127930
bool ConfigRead(ConfigVisitor* visitor, const char* text, uint32_t size) {
    const char* end = text + size;
    for (;;) {
        const char* line;
        const char* newline;
        for (;;) {
            line = text;
            if (end <= line) {
                return visitor->Finish();
            }
            newline = static_cast<const char*>(std::memchr(line, '\n', static_cast<uint64_t>(end - line)));
            if (!newline) {
                newline = end;
            }
            if (line != newline) {
                while (line < newline && IsSpace(*line)) {
                    line++;
                }
                text = newline + 1;
                if (*line != '#') {
                    break;
                }
            } else {
                text = line + 1;
            }
        }
        const char* lineEnd = newline;
        while (line < lineEnd && IsSpace(lineEnd[-1])) {
            lineEnd--;
        }
        const char* keyEnd = line;
        for (;;) {
            keyEnd++;
            if (lineEnd <= keyEnd) {
                break;
            }
            if (IsSpace(*keyEnd) || *keyEnd == '=') {
                break;
            }
        }
        const char* value = keyEnd;
        while (IsSpace(*value) || *value == '=') {
            value++;
            if (lineEnd <= value) {
                break;
            }
        }
        if (line < keyEnd && value < lineEnd && !visitor->OnKeyValue(line, keyEnd, value, lineEnd)) {
            return false;
        }
    }
}

// ref: FUN_142127a90
bool BuildConfigVisitor::OnKeyValue(const char* key, const char* keyEnd, const char* value, const char* valueEnd) {
    uint32_t length = static_cast<uint32_t>(keyEnd - key);
    const char* text = value;
    BUILDCONFIG* config = this->m_config;
    switch (key[0]) {
        case 'b': {
            if (KeyIs(key, length, "build-partial-priority")) {
                return ParsePartialPriority("build-partial-priority", &text, valueEnd, &config->m_partialPriority);
            }
            if (length < 7 || !KeyIs(key, 6, "build-")) {
                return true;
            }
            uint32_t valueLength = static_cast<uint32_t>(valueEnd - value);
            Array<char>& lines = config->m_buildLines;
            lines.Reserve(lines.m_count + 4 + valueLength + length);
            AppendBytes(&lines, key, length);
            AppendBytes(&lines, " = ", 3);
            AppendBytes(&lines, value, valueLength);
            lines.Reserve(lines.m_count + 1);
            lines.m_data[lines.m_count++] = '\0';
            return true;
        }
        case 'd':
            if (KeyIs(key, length, "download")) {
                return ParseRecordKeys("download", &text, valueEnd, &config->m_records[0]);
            }
            if (KeyIs(key, length, "download-size")) {
                return ParseRecordSizes("download-size", &text, valueEnd, &config->m_records[0]);
            }
            return true;
        case 'e':
            if (KeyIs(key, length, "encoding")) {
                return ParseRecordKeys("encoding", &text, valueEnd, &config->m_records[CONFIG_ENCODING]);
            }
            if (KeyIs(key, length, "encoding-size")) {
                return ParseRecordSizes("encoding-size", &text, valueEnd, &config->m_records[CONFIG_ENCODING]);
            }
            return true;
        case 'i':
            if (KeyIs(key, length, "install")) {
                return ParseRecordKeys("install", &text, valueEnd, &config->m_records[1]);
            }
            if (KeyIs(key, length, "install-size")) {
                return ParseRecordSizes("install-size", &text, valueEnd, &config->m_records[1]);
            }
            return true;
        case 'k': {
            if (KeyIs(key, length, "key-layout-index-bits")) {
                SkipSpace(&text, valueEnd);
                if (text < valueEnd) {
                    bool error = false;
                    ParseUInt8(text, static_cast<uint64_t>(valueEnd - text), &config->m_keyLayoutIndexBits, &error);
                    if (!error) {
                        return true;
                    }
                }
                blz::log(g_logCategory, 4, s_readerFile, 0x159,
                    "Invalid build config. Missing or invalid value for key '%s'.", "key-layout-index-bits");
                return false;
            }
            if (length < 0xC || std::memcmp(key, "key-layout", 10) != 0 || key[10] != '-') {
                return true;
            }
            const char* number = key + 0xB;
            const char* digitsEnd = number;
            while (digitsEnd < keyEnd && static_cast<uint8_t>(*digitsEnd - '0') < 10) {
                digitsEnd++;
            }
            uint32_t index = 0;
            for (;;) {
                if (digitsEnd <= number) {
                    blz::log(g_logCategory, 4, s_readerFile, 200,
                        "Invalid build config. Key layout has malformed identifier, expected 'key-layout-<number>'.");
                    return false;
                }
                if (!IsSpace(*number)) {
                    break;
                }
                number++;
            }
            bool error = false;
            uint64_t used = ParseUInt32(number, static_cast<uint64_t>(digitsEnd - number), &index, &error);
            if (error || number + used != keyEnd) {
                blz::log(g_logCategory, 4, s_readerFile, 200,
                    "Invalid build config. Key layout has malformed identifier, expected 'key-layout-<number>'.");
                return false;
            }
            blz::alloc_string name;
            name.Init(GetBufferAllocator());
            name.Insert(0, key, length);
            Array<KEYLAYOUT>& layouts = config->m_keyLayouts;
            uint32_t count = index + 1;
            if (layouts.m_count <= count) {
                layouts.Reserve(count);
                for (uint32_t i = layouts.m_count; i < count; i++) {
                    std::memset(&layouts.m_data[i], 0, sizeof(KEYLAYOUT));
                }
            }
            layouts.m_count = count;
            KEYLAYOUT* layout = &config->m_keyLayouts.m_data[index];
            const char* message;
            int32_t line;
            SkipSpace(&text, valueEnd);
            bool fail = true;
            if (text >= valueEnd ||
                (text += ParseUInt8(text, static_cast<uint64_t>(valueEnd - text), &layout->m_chunkBits, &error), error)) {
                message = "Invalid build config. Missing or invalid Chunk Bits value for static layout key '%s'.";
                line = 0x163;
            } else if ((SkipSpace(&text, valueEnd), text >= valueEnd) ||
                (text += ParseUInt8(text, static_cast<uint64_t>(valueEnd - text), &layout->m_archiveBits, &error), error)) {
                message = "Invalid build config. Missing or invalid Archive Bits value for static layout key '%s'.";
                line = 0x169;
            } else if ((SkipSpace(&text, valueEnd), text >= valueEnd) ||
                (text += ParseUInt8(text, static_cast<uint64_t>(valueEnd - text), &layout->m_offsetBits, &error), error)) {
                message = "Invalid build config. Missing or invalid Offset Bits value for static layout key '%s'.";
                line = 0x16f;
            } else if (!ParseNextUInt32(&text, valueEnd, &layout->m_alignment)) {
                message = "Invalid build config. Missing or invalid Alignment value for static layout key '%s'.";
                line = 0x175;
            } else {
                fail = false;
                message = nullptr;
                line = 0;
            }
            if (fail) {
                blz::log(g_logCategory, 4, s_readerFile, line, message, name.c_str());
            }
            name.Destroy();
            return !fail;
        }
        case 'p':
            if (KeyIs(key, length, "patch")) {
                return ParseSingleKey("patch", &text, valueEnd, config->m_patchKey);
            }
            if (KeyIs(key, length, "patch-size")) {
                SkipSpace(&text, valueEnd);
                if (text < valueEnd) {
                    bool error = false;
                    ParseUInt32(text, static_cast<uint64_t>(valueEnd - text), &config->m_patchSize, &error);
                    if (!error) {
                        return true;
                    }
                }
                blz::log(g_logCategory, 4, s_readerFile, 0x159,
                    "Invalid build config. Missing or invalid value for key '%s'.", "patch-size");
                return false;
            }
            if (KeyIs(key, length, "patch-config")) {
                return ParseSingleKey("patch-config", &text, valueEnd, config->m_patchConfig);
            }
            if (KeyIs(key, length, "patch-index")) {
                return ParseRecordKeys("patch-index", &text, valueEnd, &config->m_records[CONFIG_PATCH_INDEX]);
            }
            if (KeyIs(key, length, "patch-index-size")) {
                return ParseRecordSizes("patch-index-size", &text, valueEnd, &config->m_records[CONFIG_PATCH_INDEX]);
            }
            return true;
        case 'r':
            if (KeyIs(key, length, "root")) {
                return ParseSingleKey("root", &text, valueEnd, config->m_rootKey);
            }
            return true;
        case 's':
            if (KeyIs(key, length, "size")) {
                return ParseRecordKeys("size", &text, valueEnd, &config->m_records[2]);
            }
            if (KeyIs(key, length, "size-size")) {
                return ParseRecordSizes("size-size", &text, valueEnd, &config->m_records[2]);
            }
            return true;
        case 'v': {
            if (KeyIs(key, length, "vfs-root")) {
                return ParseRecordKeys("vfs-root", &text, valueEnd, &config->m_records[CONFIG_ROOT]);
            }
            if (KeyIs(key, length, "vfs-root-size")) {
                return ParseRecordSizes("vfs-root-size", &text, valueEnd, &config->m_records[CONFIG_ROOT]);
            }
            if (KeyIs(key, length, "vfs-root-espec")) {
                return ParseRecordESpec("vfs-root-espec", &text, valueEnd, &config->m_records[CONFIG_ROOT]);
            }
            if (length < 5 || std::memcmp(key, "vfs-", 4) != 0) {
                return true;
            }
            const char* number = key + 4;
            const char* digitsEnd = number;
            while (digitsEnd < keyEnd && static_cast<uint8_t>(*digitsEnd - '0') < 10) {
                digitsEnd++;
            }
            uint32_t index = 0;
            for (;;) {
                if (digitsEnd <= number) {
                    blz::log(g_logCategory, 4, s_readerFile, 0xfe,
                        "Invalid build config. VFS manifest entry has malformed identifier, expected 'vfs-<number>'.");
                    return false;
                }
                if (!IsSpace(*number)) {
                    break;
                }
                number++;
            }
            bool error = false;
            uint64_t used = ParseUInt32(number, static_cast<uint64_t>(digitsEnd - number), &index, &error);
            if (error || (number += used, index == 0)) {
                blz::log(g_logCategory, 4, s_readerFile, 0xfe,
                    "Invalid build config. VFS manifest entry has malformed identifier, expected 'vfs-<number>'.");
                return false;
            }
            blz::alloc_string name;
            name.Init(GetBufferAllocator());
            name.Insert(0, key, length);
            bool result;
            uint32_t suffix = static_cast<uint32_t>(keyEnd - number);
            if (number == keyEnd) {
                ResizeVfs(&config->m_vfs, index);
                result = ParseRecordKeys(name.c_str(), &text, valueEnd, &config->m_vfs.m_data[index - 1]);
            } else if (KeyIs(number, suffix, "-size")) {
                ResizeVfs(&config->m_vfs, index);
                result = ParseRecordSizes(name.c_str(), &text, valueEnd, &config->m_vfs.m_data[index - 1]);
            } else if (KeyIs(number, suffix, "-espec")) {
                ResizeVfs(&config->m_vfs, index);
                result = ParseRecordESpec(name.c_str(), &text, valueEnd, &config->m_vfs.m_data[index - 1]);
            } else {
                name.Destroy();
                return true;
            }
            name.Destroy();
            return result;
        }
        default:
            return true;
    }
}

// Every manifest that has a size needs its key; key layouts need an index width; and there
// must be a root manifest or an encoding table.
// ref: FUN_142129910
bool BuildConfigVisitor::Finish() {
    BUILDCONFIG* config = this->m_config;
    static const uint8_t s_zero[16] = {};
    auto zero = [](const uint8_t* key) {
        return std::memcmp(key, s_zero, 16) == 0;
    };
    const char* message;
    int32_t line;
    CONFIGRECORD* download = &config->m_records[0];
    CONFIGRECORD* install = &config->m_records[1];
    CONFIGRECORD* size = &config->m_records[2];
    CONFIGRECORD* encoding = &config->m_records[CONFIG_ENCODING];
    CONFIGRECORD* root = &config->m_records[CONFIG_ROOT];
    if (download->m_csize != 0 && zero(download->m_ckey)) {
        message = "Invalid build config. Download manifest C-Key is missing, but C-Size was specified.";
        line = 0x11c;
    } else if (download->m_esize != 0 && zero(download->m_ekey)) {
        message = "Invalid build config. Download manifest E-Key is missing, but E-Size was specified.";
        line = 0x122;
    } else if (size->m_csize != 0 && zero(size->m_ckey)) {
        message = "Invalid build config. Size manifest C-Key is missing, but C-Size was specified.";
        line = 0x128;
    } else if (size->m_esize != 0 && zero(size->m_ekey)) {
        message = "Invalid build config. Size manifest E-Key is missing, but E-Size was specified.";
        line = 0x12e;
    } else if (install->m_csize != 0 && zero(install->m_ckey)) {
        message = "Invalid build config. Install manifest C-Key is missing, but C-Size was specified.";
        line = 0x134;
    } else if (install->m_esize != 0 && zero(install->m_ekey)) {
        message = "Invalid build config. Install manifest E-Key is missing, but E-Size was specified.";
        line = 0x13a;
    } else if (config->m_keyLayoutIndexBits != 0 && config->m_keyLayouts.m_count == 0) {
        message = "Invalid build config. Static key layouts are missing.";
        line = 0x140;
    } else {
        bool hasRoot = !zero(root->m_ckey) && !zero(root->m_ekey) && root->m_csize != 0 && root->m_esize != 0;
        bool hasEncoding = !zero(encoding->m_ckey) && !zero(encoding->m_ekey) && encoding->m_csize != 0 &&
            encoding->m_esize != 0;
        if (hasRoot || hasEncoding) {
            return true;
        }
        message = "Invalid build config. Root file manifest (vfs-root) and encoding table are missing.";
        line = 0x14d;
    }
    blz::log(g_logCategory, 4, s_readerFile, line, message);
    return false;
}

}
