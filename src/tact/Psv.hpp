#ifndef TACT_PSV_HPP
#define TACT_PSV_HPP

// pkg/tact/3.6.39/src/psv: PSV ("pipe-separated values") files - .build.info, .flavor.info.
// A header line "Name!TYPE:width|..." (TYPE DEC, HEX or STRING), "## key = value" metadata
// lines, "#" comments, then one row per line.

#include "tact/Common.hpp"

namespace tact {

struct PSVHEADER {
    const char* m_name;
    uint8_t m_type;        // 1 DEC, 2 HEX, 3 STRING
    uint32_t m_width;
    uint32_t m_field;      // the manifest field this column fills, or 0xFFFFFFFF
};
static_assert(sizeof(PSVHEADER) == 0x18, "PSVHEADER is 0x18 bytes");

struct PSVMETADATA {
    const char* m_key;
    const char* m_value;
};

// The reader: parses into its own copy of the text, calling the hooks for the header and
// for each row and field.
class PsvReader {
    public:
    blz::allocator* m_allocator;        // 0x08
    Array<PSVHEADER> m_headers;         // 0x10
    Array<PSVMETADATA> m_metadata;      // 0x28
    Array<char> m_text;                 // 0x40
    uint32_t m_line;                    // 0x58

    explicit PsvReader(blz::allocator* allocator);
    virtual ~PsvReader();
    virtual bool WantsHeader();
    virtual bool OnHeader() = 0;
    virtual bool OnRow() = 0;
    virtual bool OnField(char* text, uint32_t length, PSVHEADER* header) = 0;
    virtual bool OnComplete() = 0;

    bool Parse(const char* text, uint32_t length);
    bool ParseHeader(char* line, uint32_t length);
    bool ParseRow(char* line, int32_t length);
    bool ParseMetadata(char* line, uint64_t length);
};

// A manifest column: where and how a named column is stored in a row.
#define PSVFIELD_OPTIONAL 0x1
#define PSVFIELD_LIST     0x2
struct PSVFIELD {
    const char* m_name;
    uint64_t m_size;
    uint64_t m_alignment;
    uint64_t m_offset;
    uint64_t m_countOffset;  // lists: where the element count goes
    bool (*m_parse)(const char* text, uint64_t length, void* out);
    void* m_write;           // the writer side (not ported)
    uint32_t m_flags;
    uint8_t m_type;          // as PSVHEADER::m_type
    uint64_t m_hexLength;
};

// What a parsed manifest is: the headers, metadata and rows, and the reader that owns them.
struct PSVMANIFEST {
    PSVHEADER* m_headers;
    uint32_t m_headerCount;
    PSVMETADATA* m_metadata;
    uint32_t m_metadataCount;
    void* m_entries;
    uint32_t m_entryCount;
    PsvReader* m_reader;
};

// A reader that fills rows of Entry from a fixed field table (psv_manifest_reader.h).
template <class Entry, uint32_t FieldCount>
class PsvManifestReader : public PsvReader {
    public:
    Array<Entry> m_entries;      // 0x60
    Array<void*> m_lists;        // 0x78: list allocations, freed with the reader
    PSVMANIFEST m_result;        // 0x90
    const PSVFIELD* m_fields;

    PsvManifestReader(blz::allocator* allocator, const PSVFIELD* fields) : PsvReader(allocator), m_fields(fields) {
        this->m_entries.Init(allocator);
        this->m_lists.Init(allocator);
        std::memset(&this->m_result, 0, sizeof(this->m_result));
    }

    ~PsvManifestReader() override {
        for (uint32_t i = 0; i < this->m_lists.m_count; i++) {
            this->m_allocator->deallocate(this->m_lists.m_data[i]);
        }
        this->m_lists.Free();
        this->m_entries.Free();
    }

    // Maps the columns to fields; a required field without a column fails.
    bool OnHeader() override {
        bool found[FieldCount] = {};
        for (uint32_t i = 0; i < this->m_headers.m_count; i++) {
            PSVHEADER* header = &this->m_headers.m_data[i];
            header->m_field = 0xFFFFFFFF;
            for (uint32_t f = 0; f < FieldCount; f++) {
                if (std::strcmp(header->m_name, this->m_fields[f].m_name) == 0) {
                    found[f] = true;
                    header->m_field = f;
                    break;
                }
            }
        }
        for (uint32_t f = 0; f < FieldCount; f++) {
            if (!found[f] && !(this->m_fields[f].m_flags & PSVFIELD_OPTIONAL)) {
                blz::log(g_logCategory, 4, "psv_manifest_reader.h", 0x62, "Required column is missing: %s",
                    this->m_fields[f].m_name);
                return false;
            }
        }
        return true;
    }

    bool OnRow() override {
        if (this->m_entries.m_count == this->m_entries.m_capacity) {
            this->m_entries.Reserve(this->m_entries.m_count + 1);
        }
        Entry::Init(&this->m_entries.m_data[this->m_entries.m_count]);
        this->m_entries.m_count++;
        return true;
    }

    bool OnField(char* text, uint32_t length, PSVHEADER* header) override {
        if (header->m_field == 0xFFFFFFFF) {
            return true;
        }
        const PSVFIELD* field = &this->m_fields[header->m_field];
        if (length == 0 && field->m_type != 3) {
            if (!(field->m_flags & PSVFIELD_OPTIONAL)) {
                blz::log(g_logCategory, 4, "psv_manifest_reader.h", 0x88,
                    "Missing required field data for field '%s' on line %u", field->m_name, this->m_line);
                return false;
            }
            return true;
        }
        if (field->m_flags & PSVFIELD_LIST) {
            return this->ParseList(text, field);
        }
        uint8_t* entry = reinterpret_cast<uint8_t*>(&this->m_entries.m_data[this->m_entries.m_count - 1]);
        if (!field->m_parse(text, length, entry + field->m_offset)) {
            blz::log(g_logCategory, 4, "psv_manifest_reader.h", 0xB5,
                "Failed to parse field data '%s' for field '%s' on line %u", text, field->m_name, this->m_line);
            return false;
        }
        return true;
    }

    // A whitespace-separated list into a new array, its pointer and count stored in the row.
    bool ParseList(char* text, const PSVFIELD* field) {
        uint32_t count = 0;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(text);
        auto space = [](uint8_t c) { return c == ' ' || static_cast<uint8_t>(c - 9) < 5; };
        if (*p != 0) {
            for (;;) {
                count++;
                while (*p != 0 && !space(*p)) {
                    p++;
                }
                if (*p == 0) {
                    break;
                }
                while (*p != 0 && space(*p)) {
                    p++;
                }
                if (*p == 0) {
                    break;
                }
            }
        }
        if (count == 0) {
            return true;
        }

        auto list = static_cast<uint8_t*>(this->m_allocator->allocate(count * field->m_size, field->m_alignment));
        char* token = text;
        for (uint32_t i = 0; *token != '\0';) {
            char* end = token;
            while (*end != '\0' && !space(static_cast<uint8_t>(*end))) {
                end++;
            }
            *end = '\0';
            if (token < end && !field->m_parse(token, static_cast<uint64_t>(end - token), list + i * field->m_size)) {
                blz::log(g_logCategory, 4, "psv_manifest_reader.h", 0xE6,
                    "Failed to parse array field data '%s' for field '%s' (index %u) on line %u", token, field->m_name, i,
                    this->m_line);
                this->m_allocator->deallocate(list);
                return false;
            }
            if (++i == count) {
                break;
            }
            token = end + 1;
            while (*token != '\0' && space(static_cast<uint8_t>(*token))) {
                token++;
            }
        }

        if (this->m_lists.m_count == this->m_lists.m_capacity) {
            this->m_lists.Reserve(this->m_lists.m_count + 1);
        }
        this->m_lists.m_data[this->m_lists.m_count++] = list;
        uint8_t* entry = reinterpret_cast<uint8_t*>(&this->m_entries.m_data[this->m_entries.m_count - 1]);
        *reinterpret_cast<void**>(entry + field->m_offset) = list;
        *reinterpret_cast<uint32_t*>(entry + field->m_countOffset) = count;
        return true;
    }

    // ref: FUN_1402d3120
    bool OnComplete() override {
        this->m_result.m_headers = this->m_headers.m_data;
        this->m_result.m_headerCount = this->m_headers.m_count;
        this->m_result.m_metadata = this->m_metadata.m_data;
        this->m_result.m_metadataCount = this->m_metadata.m_count;
        this->m_result.m_entries = this->m_entries.m_data;
        this->m_result.m_entryCount = this->m_entries.m_count;
        this->m_result.m_reader = this;
        return true;
    }
};

// Frees a parsed manifest (its reader owns everything).
void PsvManifestFree(PSVMANIFEST* manifest);

}

#endif
