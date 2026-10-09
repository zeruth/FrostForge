#include "tact/Psv.hpp"

namespace tact {

namespace {

const char s_readerFile[] = "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\psv\\psv_reader.cpp";

bool IsSpace(char c) {
    return c == ' ' || static_cast<uint8_t>(c - 9) < 5;
}

int32_t Lower(char c) {
    int32_t value = c;
    return static_cast<uint32_t>(value - 'A') <= 0x19 ? value + 0x20 : value;
}

bool EqualsNoCase(const char* a, const char* b) {
    for (;;) {
        int32_t x = Lower(*a++);
        int32_t y = Lower(*b++);
        if (x != y) {
            return false;
        }
        if (x == 0) {
            return true;
        }
    }
}

}

// ref: FUN_1420d9200
PsvReader::PsvReader(blz::allocator* allocator) : m_allocator(allocator), m_line(0) {
    this->m_headers.Init(allocator);
    this->m_metadata.Init(allocator);
    this->m_text.Init(allocator);
}

// ref: FUN_1402cc620
PsvReader::~PsvReader() {
    this->m_text.Free();
    this->m_metadata.Free();
    this->m_headers.Free();
}

// ref: FUN_14026bea0
bool PsvReader::WantsHeader() {
    return true;
}

// Copies the text, then reads it line by line ("\r\n" or "\n"; a NUL also ends a line).
// ref: FUN_1420d9240
bool PsvReader::Parse(const char* text, uint32_t length) {
    uint32_t size = length + 1;
    if (this->m_text.m_count <= size && this->m_text.m_capacity < size) {
        this->m_text.Reserve(size);
    }
    this->m_text.m_count = size;
    std::memcpy(this->m_text.m_data, text, length);
    this->m_text.m_data[length] = '\0';

    char* line = this->m_text.m_data;
    char* end = line + this->m_text.m_count;
    for (char* p = line; p < end; p++) {
        if (*p != '\n' && *p != '\0') {
            continue;
        }
        if (line < p) {
            char* lineEnd = p;
            if (p[-1] == '\r') {
                lineEnd = p - 1;
            }
            *lineEnd = '\0';
            this->m_line++;
            while (*line != '\0' && IsSpace(*line) && line <= lineEnd) {
                line++;
            }
            int32_t lineLength = static_cast<int32_t>(lineEnd - line);
            if (lineLength != 0) {
                bool ok;
                if (*line == '#') {
                    if (line[1] != '#') {
                        line = p + 1;
                        continue;
                    }
                    ok = this->ParseMetadata(line + 2, static_cast<uint64_t>(lineLength - 2));
                } else if (this->m_headers.m_count == 0 && this->WantsHeader()) {
                    ok = this->ParseHeader(line, static_cast<uint32_t>(lineLength));
                } else {
                    ok = this->ParseRow(line, lineLength);
                }
                if (!ok) {
                    return false;
                }
            }
        }
        line = p + 1;
    }
    return this->OnComplete();
}

// ref: FUN_1420d9530
bool PsvReader::ParseHeader(char* line, uint32_t length) {
    uint32_t columns = 1;
    for (uint32_t i = 0; i < length; i++) {
        if (line[i] == '|') {
            columns++;
        }
    }
    if (this->m_headers.m_count <= columns) {
        if (this->m_headers.m_capacity < columns) {
            this->m_headers.Reserve(columns);
        }
        for (uint32_t i = this->m_headers.m_count; i < columns; i++) {
            PSVHEADER* header = &this->m_headers.m_data[i];
            header->m_name = "";
            header->m_type = 0;
            header->m_width = 0;
            header->m_field = 0xFFFFFFFF;
        }
    }
    this->m_headers.m_count = columns;

    char* p = line;
    for (uint32_t column = 0; *p != '\0' && column < columns; column++) {
        PSVHEADER* header = &this->m_headers.m_data[column];
        while (*p != '\0' && IsSpace(*p)) {
            p++;
        }
        header->m_name = p;
        char* bang = p;
        while (*bang != '\0' && *bang != '!') {
            bang++;
        }
        if (*bang == '\0') {
            blz::log(g_logCategory, 4, s_readerFile, 0xC0, "No header type separator ('!') found on line %u", this->m_line);
            return false;
        }
        char* nameEnd = bang;
        while (header->m_name < nameEnd && IsSpace(nameEnd[-1])) {
            *--nameEnd = '\0';
        }
        *bang = '\0';
        char* type = bang + 1;

        const char* name = header->m_name;
        uint32_t nameLength = static_cast<uint32_t>(nameEnd - name);
        bool valid = nameLength != 0 && !IsSpace(name[0]) && !IsSpace(name[nameLength - 1]);
        for (uint32_t i = 0; valid && i < nameLength; i++) {
            if (name[i] == '|' || name[i] == '!' || name[i] == ':') {
                valid = false;
            }
        }
        if (!valid) {
            blz::log(g_logCategory, 4, s_readerFile, 0xCF, "Header name '%s' is not valid on line %u", name, this->m_line);
            return false;
        }

        char* colon = type;
        while (*colon != '\0' && *colon != ':') {
            colon++;
        }
        if (*colon == '\0') {
            blz::log(g_logCategory, 4, s_readerFile, 0xD9, "No header width separator (':') found on line %u", this->m_line);
            return false;
        }
        char* typeEnd = colon;
        while (header->m_name < typeEnd && IsSpace(typeEnd[-1])) {
            *--typeEnd = '\0';
        }
        *colon = '\0';
        char* width = colon + 1;

        header->m_type = 0;
        if (EqualsNoCase(type, "DEC")) {
            header->m_type = 1;
        } else if (EqualsNoCase(type, "HEX")) {
            header->m_type = 2;
        } else if (EqualsNoCase(type, "STRING")) {
            header->m_type = 3;
        } else {
            blz::log(g_logCategory, 4, s_readerFile, 0xF0, "Encountered unknown header type specifier '%s' on line %u", type,
                this->m_line);
            return false;
        }

        uint64_t value = 0;
        bool overflow;
        uint64_t consumed = ParseUInt64(width, std::strlen(width), &value, &overflow);
        if (consumed == 0) {
            blz::log(g_logCategory, 4, s_readerFile, 0xF9, "Unable to parse width value '%s' on line %u", width, this->m_line);
            return false;
        }
        if (value >= 0x100000000ull) {
            blz::log(g_logCategory, 4, s_readerFile, 0xFF, "Column width value is too large, got %llu on line %u", value,
                this->m_line);
            return false;
        }
        header->m_width = static_cast<uint32_t>(value);
        p = width + consumed;
        while (*p != '\0' && IsSpace(*p)) {
            p++;
        }
        if (*p == '|') {
            *p++ = '\0';
        } else if (*p != '\0') {
            blz::log(g_logCategory, 4, s_readerFile, 0x110, "Encountered unexpected character at the end of header on line %u",
                this->m_line);
            return false;
        }
    }
    return this->OnHeader();
}

// ref: FUN_1420d93a0
bool PsvReader::ParseRow(char* line, int32_t length) {
    if (length == 0) {
        return true;
    }
    if (!this->OnRow()) {
        return false;
    }
    uint32_t column = 0;
    for (; *line == '\0' || column <= this->m_headers.m_count - 1; column++) {
        if (column >= this->m_headers.m_count) {
            blz::log(g_logCategory, 4, s_readerFile, 0x139, "Too many columns in PSV file on line %u", this->m_line);
            return false;
        }
        char* start = line;
        while (*start != '\0' && IsSpace(*start)) {
            start++;
        }
        bool last = false;
        char* end = start;
        while (*end != '\0' && *end != '|') {
            end++;
        }
        if (*end == '|') {
            *end = '\0';
            line = end + 1;
        } else {
            end = start + std::strlen(start);
            last = true;
            line = end;
        }
        while (start < end && IsSpace(end[-1])) {
            *--end = '\0';
        }
        if (!this->OnField(start, static_cast<uint32_t>(end - start), &this->m_headers.m_data[column])) {
            return false;
        }
        if (last) {
            break;
        }
    }
    if (column == this->m_headers.m_count - 1) {
        return true;
    }
    blz::log(g_logCategory, 4, s_readerFile, 0x163, "Missing columns in PSV file on line %u", this->m_line);
    return false;
}

// "## key = value"
// ref: FUN_1420d9a20
bool PsvReader::ParseMetadata(char* line, uint64_t length) {
    char* start = line;
    while (*start != '\0' && IsSpace(*start)) {
        start++;
    }
    if (*start == '\0') {
        return true;
    }
    if (this->m_metadata.m_count == this->m_metadata.m_capacity) {
        this->m_metadata.Reserve(this->m_metadata.m_count + 1);
    }
    PSVMETADATA* pair = &this->m_metadata.m_data[this->m_metadata.m_count];
    pair->m_key = "";
    pair->m_value = "";
    this->m_metadata.m_count++;
    pair->m_key = start;

    char* equals = start;
    while (*equals != '\0' && *equals != '=') {
        equals++;
    }
    if (*equals == '\0') {
        blz::log(g_logCategory, 4, s_readerFile, 0x86, "Missing equals ('=') for metadata value on line %u", this->m_line);
        return false;
    }
    char* keyEnd = equals;
    if (pair->m_key < keyEnd) {
        for (;;) {
            *keyEnd = '\0';
            if (keyEnd <= pair->m_key || !IsSpace(keyEnd[-1])) {
                break;
            }
            keyEnd--;
        }
    }
    char* value = equals + 1;
    *equals = '\0';
    while (*value != '\0' && IsSpace(*value)) {
        value++;
    }
    pair->m_value = value;
    char* end = line + static_cast<uint32_t>(length);
    while (pair->m_value < end && IsSpace(end[-1])) {
        *--end = '\0';
    }
    return true;
}

// ref: FUN_1420daa20
void PsvManifestFree(PSVMANIFEST* manifest) {
    if (manifest && manifest->m_reader) {
        PsvReader* reader = manifest->m_reader;
        blz::allocator* allocator = GetAllocator();
        reader->~PsvReader();
        allocator->deallocate(reader);
    }
}

}
