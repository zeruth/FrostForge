#include "casc/KeyMappingTable.hpp"
#include "casc/Dynamic.hpp"
#include "casc/Hash.hpp"
#include "blz/Log.hpp"
#include <algorithm>
#include <cstring>
#include <windows.h>

namespace casc {

namespace {

const char s_kmtFile[] = "D:\\BuildServer\\A\\work\\package_cache\\casc\\1.4.41\\src\\dynamic\\key_mapping_table.cpp";
const char s_keyStateFile[] = "D:\\BuildServer\\A\\work\\package_cache\\casc\\1.4.41\\src\\dynamic\\key_state.cpp";
const float s_fullRate = 1.0f;

uint64_t ReadBE40(const uint8_t* p) {
    return uint64_t(p[0]) << 32 | uint64_t(p[1]) << 24 | uint64_t(p[2]) << 16 | uint64_t(p[3]) << 8 | p[4];
}

void WriteBE40(uint8_t* p, uint64_t value) {
    p[0] = static_cast<uint8_t>(value >> 32);
    p[1] = static_cast<uint8_t>(value >> 24);
    p[2] = static_cast<uint8_t>(value >> 16);
    p[3] = static_cast<uint8_t>(value >> 8);
    p[4] = static_cast<uint8_t>(value);
}

uint32_t ReadU32(const uint8_t* p) {
    uint32_t value;
    std::memcpy(&value, p, 4);
    return value;
}

SPAN SpanOf(const uint8_t* offsetBytes) {
    SPAN span;
    span.m_offset = ReadBE40(offsetBytes);
    span.m_size = ReadU32(offsetBytes + 5);
    span.m_pad = 0;
    return span;
}

bool SameKey(const uint8_t* a, const uint8_t* b) {
    return std::memcmp(a, b, 9) == 0;
}

// The reads of the mapped tables run under a structured exception handler that retries in-page
// errors; this reports how a retry went (only the retries are counted).
// ref: FUN_1421680b0
void ReportInPageRetry(int32_t retry, STATSINK* stats) {
    const char* name;
    if (retry == 1) {
        name = "dynamic_container.structured_exception.in_page_error.retry_1_succeeded";
    } else if (retry == 2) {
        name = "dynamic_container.structured_exception.in_page_error.retry_2_succeeded";
    } else {
        return;
    }
    StatsReport(stats, 0, name, 1, s_fullRate);
}

// ref: FUN_14215cea0
void LogWriteError(const char* what, uint32_t error) {
    char text[0x100];
    OsErrorString(text, sizeof(text), error);
    blz::log(g_logCategory, 4, s_kmtFile, 0x62, "Failed to write '%s' to table file. Error (%u): %s", what, error, text);
}

// ref: FUN_14215cde0
void LogReadError(uint32_t error) {
    char text[0x100];
    OsErrorString(text, sizeof(text), error);
    blz::log(g_logCategory, 4, s_kmtFile, 0x70, "Failed to read KMT file data. Error (%u): %s", error, text);
}

// ref: FUN_14215cd80
void LogMapError(uint32_t error) {
    char text[0x100];
    OsErrorString(text, sizeof(text), error);
    blz::log(g_logCategory, 4, s_kmtFile, 0x78, "Failed to bind KMT shared memory. Error (%u): %s", error, text);
}

// ref: FUN_14215ce40
void LogSyncError(uint32_t error) {
    char text[0x100];
    OsErrorString(text, sizeof(text), error);
    blz::log(g_logCategory, 4, s_kmtFile, 0x69, "Failed to synchronize KMT data. Error (%u): %s", error, text);
}

// A write that must complete: a failure is logged, and only an error that maps to a casc
// error stops the caller.
int32_t WriteAll(OSFILE* file, const void* data, uint32_t size, const char* what) {
    uint32_t written = 0;
    uint32_t error = OsFileWrite(file, data, size, &written);
    if (error != 0 || written != size) {
        LogWriteError(what, error);
        return ErrorFromOs(error);
    }
    return 0;
}

// ref: FUN_14215ccc0
int32_t ZeroFill(OSFILE* file, uint64_t size) {
    uint8_t zeros[0x1000];
    std::memset(zeros, 0, sizeof(zeros));
    while (size != 0) {
        uint32_t chunk = size < 0x1000 ? static_cast<uint32_t>(size) : 0x1000;
        int32_t error = WriteAll(file, zeros, chunk, "zero fill");
        if (error != 0) {
            return error;
        }
        size -= chunk;
    }
    return 0;
}

// The section and file headers of an empty table, then an empty sorted section, padded to
// 64 KB for the update pages.
// ref: FUN_14215c830
int32_t CreateTableFile(KEYMAPPINGTABLE* table, OSFILE* file, uint64_t maxSize) {
    uint8_t header[0x10];
    header[0] = 7;
    header[1] = 0;
    header[2] = static_cast<uint8_t>(table->m_bucket);
    header[3] = 0;
    header[4] = 4;
    header[5] = 5;
    header[6] = 9;
    header[7] = 0x1E;
    std::memcpy(header + 8, &maxSize, 8);
    uint32_t pc = 0;
    uint32_t pb = 0;
    HashLittle2(header, 0x10, &pc, &pb);
    uint32_t section[2] = { 0x10, pc };
    int32_t error = WriteAll(file, section, 8, "section header");
    if (error != 0) {
        return error;
    }
    error = WriteAll(file, header, 0x10, "file header");
    if (error != 0) {
        return error;
    }
    uint64_t position = OsFileTell(file);
    error = ZeroFill(file, ((position + 0xF) & ~uint64_t(0xF)) - position);
    if (error != 0) {
        return error;
    }
    uint32_t empty[2] = { 0, 0 };
    error = WriteAll(file, empty, 8, "section header");
    if (error != 0) {
        return error;
    }
    position = OsFileTell(file);
    return ZeroFill(file, ((position + 0x17FFF) & ~uint64_t(0xFFFF)) - position);
}

// The records a flush writes, buffered 10000 at a time (FUN_14215ae80 empties the buffer).
struct WRITEBUFFER {
    blz::allocator* m_allocator;
    OSFILE* m_file;
    uint8_t* m_buffer;
    uint8_t* m_cursor;
    uint32_t m_count;
};

// ref: FUN_14215ae80
int32_t FlushBuffer(WRITEBUFFER* buffer) {
    if (buffer->m_count == 0) {
        return 0;
    }
    int32_t error = WriteAll(buffer->m_file, buffer->m_buffer, buffer->m_count * SORTED_ENTRY, "buffered records");
    if (error != 0) {
        return error;
    }
    buffer->m_cursor = buffer->m_buffer;
    buffer->m_count = 0;
    return 0;
}

struct WRITER {
    KEYMAPPINGTABLE* m_table;
    uint32_t* m_pc;
    uint32_t* m_pb;
    WRITEBUFFER* m_buffer;
};

int32_t AppendRecord(WRITER* writer, const uint8_t record[SORTED_ENTRY]) {
    HashLittle2(record, SORTED_ENTRY, writer->m_pc, writer->m_pb);
    if (writer->m_buffer->m_count == 10000) {
        int32_t error = FlushBuffer(writer->m_buffer);
        if (error != 0) {
            return error;
        }
    }
    std::memcpy(writer->m_buffer->m_cursor, record, SORTED_ENTRY);
    writer->m_buffer->m_cursor += SORTED_ENTRY;
    writer->m_buffer->m_count++;
    return 0;
}

// Writes one key's records to the new sorted section: unchanged sorted runs go through as
// they are; a key with updates is written as its resolved span, followed by its resident
// ranges when only part of it is resident (or an empty span when none of it is).
// ref: FUN_14215a2b0 (reached through FUN_1421599d0)
int32_t WriteVisitor(void* context, const uint8_t* key, const uint8_t* sorted, const uint8_t* sortedEnd,
    uint8_t** updates, uint8_t** updatesEnd, const uint8_t* records, const uint8_t* recordsEnd) {
    auto writer = static_cast<WRITER*>(context);
    if (updatesEnd <= updates && recordsEnd <= records) {
        uint32_t count = static_cast<uint32_t>((sortedEnd - sorted) / SORTED_ENTRY);
        for (uint32_t i = 0; i < count; i++) {
            HashLittle2(sorted + i * SORTED_ENTRY, SORTED_ENTRY, writer->m_pc, writer->m_pb);
        }
        WRITEBUFFER* buffer = writer->m_buffer;
        int32_t error = 0;
        if (buffer->m_count + count < 0x2711 || (error = FlushBuffer(buffer)) == 0) {
            uint32_t bytes = count * SORTED_ENTRY;
            if (count > 10000) {
                uint32_t written = 0;
                uint32_t result = OsFileWrite(buffer->m_file, sorted, bytes, &written);
                if (result == 0 && written == bytes) {
                    return 0;
                }
                LogWriteError("buffered records", result);
                return ErrorFromOs(result);
            }
            std::memcpy(buffer->m_cursor, sorted, bytes);
            buffer->m_cursor += bytes;
            buffer->m_count += count;
        }
        return error;
    }
    KEYSTATE state;
    state.Init(writer->m_table->m_allocator, writer->m_table->m_stats);
    state.Build(sorted, sortedEnd, updates, updatesEnd, records, recordsEnd);
    int32_t error = 0;
    if (state.m_present) {
        uint8_t record[SORTED_ENTRY];
        std::memcpy(record, key, 9);
        WriteBE40(record + 9, state.m_span.m_offset);
        std::memcpy(record + 14, &state.m_span.m_size, 4);
        error = AppendRecord(writer, record);
        if (error == 0 && !state.Covers(&state.m_span, true)) {
            if (!state.Covers(&state.m_span, false)) {
                for (uint32_t i = 0; i < state.RangeCount() && error == 0; i++) {
                    uint64_t start = 0;
                    uint64_t end = 0;
                    state.Range(i, &start, &end);
                    WriteBE40(record + 9, start);
                    uint32_t size = static_cast<uint32_t>(end) - static_cast<uint32_t>(start);
                    std::memcpy(record + 14, &size, 4);
                    error = AppendRecord(writer, record);
                }
            } else {
                std::memset(record + 9, 0, 9);
                error = AppendRecord(writer, record);
            }
        }
    }
    state.Destroy();
    return error;
}

struct ENUMERATOR {
    KEYMAPPINGTABLE* m_table;
    KMTSPANVISITOR m_visitor;
    void* m_context;
    uint32_t m_skip;
};

// Resolves each key and reports its span with how much of it is resident; `skip` bytes at
// the front of each span are left out (a span no longer than that is not reported).
// ref: FUN_1421597b0
int32_t EnumerateVisitor(void* context, const uint8_t* key, const uint8_t* sorted, const uint8_t* sortedEnd,
    uint8_t** updates, uint8_t** updatesEnd, const uint8_t* records, const uint8_t* recordsEnd) {
    auto enumerator = static_cast<ENUMERATOR*>(context);
    KEYSTATE state;
    state.Init(enumerator->m_table->m_allocator, enumerator->m_table->m_stats);
    state.Build(sorted, sortedEnd, updates, updatesEnd, records, recordsEnd);
    if (state.m_present) {
        uint32_t skip = enumerator->m_skip;
        SPAN span = state.m_span;
        if (skip != 0) {
            if (state.m_span.m_size <= skip) {
                state.Destroy();
                return 0;
            }
            span.m_offset = state.m_span.m_offset + skip;
            span.m_size = state.m_span.m_size - skip;
        }
        int32_t kind;
        if (state.Covers(&state.m_span, true)) {
            kind = 3;
        } else if (state.Covers(&state.m_span, false)) {
            kind = 1;
        } else if (enumerator->m_skip == 0) {
            kind = 2;
        } else if (state.Covers(&span, true)) {
            kind = 3;
        } else {
            kind = 0;
            for (uint32_t i = 0; i < state.RangeCount(); i++) {
                uint64_t start = 0;
                uint64_t end = 0;
                state.Range(i, &start, &end);
                if (span.m_offset < end) {
                    kind = 1;
                    break;
                }
            }
            kind++;
        }
        if (!enumerator->m_visitor(enumerator->m_context, key, &span, kind)) {
            state.Destroy();
            return ERR_CANCELED;
        }
    }
    state.Destroy();
    return 0;
}

}

// --- KEYSTATE ------------------------------------------------------------------------------

// ref: FUN_1421676e0
void KEYSTATE::Init(blz::allocator* allocator, STATSINK* stats) {
    this->m_stats = stats;
    this->m_allocator = allocator;
    this->m_present = 0;
    this->m_span.m_offset = 0;
    this->m_span.m_size = 0;
    this->m_span.m_pad = 0;
    for (auto& transition : this->m_inline) {
        transition.m_position = 0;
        transition.m_delta = 0;
    }
    this->m_transitions = this->m_inline;
    this->m_count = 0;
    this->m_capacity = 0x10;
}

// ref: FUN_142167730
void KEYSTATE::Destroy() {
    if (this->m_transitions != this->m_inline) {
        this->m_allocator->deallocate(this->m_transitions);
    }
}

// Adds a span as a +1/-1 (resident) or -0x40000000/+0x40000000 (not resident) pair of
// transitions; a non-resident span is folded in immediately.
// ref: FUN_1421678f0
void KEYSTATE::AddSpan(const SPAN* span, bool resident) {
    if (this->m_capacity <= this->m_count + 1) {
        StatsReport(this->m_stats, 0, "dynamic_container.key_state.grew_update_buffer", 1, s_fullRate);
        uint32_t capacity = this->m_capacity * 2;
        auto grown = static_cast<TRANSITION*>(this->m_allocator->allocate(uint64_t(capacity) * sizeof(TRANSITION), 8));
        if (grown) {
            std::memset(grown, 0, uint64_t(capacity) * sizeof(TRANSITION));
        }
        std::memcpy(grown, this->m_transitions, uint64_t(this->m_count) * sizeof(TRANSITION));
        if (this->m_transitions != this->m_inline) {
            this->m_allocator->deallocate(this->m_transitions);
        }
        this->m_transitions = grown;
        this->m_capacity = capacity;
    }
    int32_t delta = resident ? 1 : -0x40000000;
    TRANSITION* start = &this->m_transitions[this->m_count++];
    start->m_position = span->m_offset;
    start->m_delta = delta;
    TRANSITION* end = &this->m_transitions[this->m_count++];
    end->m_position = span->m_offset + span->m_size;
    end->m_delta = -delta;
    if (resident) {
        return;
    }
    this->Normalize();
}

// Sorts the transitions and keeps only where the running sum crosses zero, as +1 (resident
// from here) and -1 (no longer).
// ref: FUN_142167ac0
void KEYSTATE::Normalize() {
    TRANSITION* begin = this->m_transitions;
    TRANSITION* end = begin + this->m_count;
    // FUN_142167170 is std::sort over the 16-byte transitions by position.
    std::sort(begin, end, [](const TRANSITION& a, const TRANSITION& b) {
        return a.m_position < b.m_position;
    });
    TRANSITION* out = begin;
    int32_t previous = 0;
    TRANSITION* p = begin;
    while (p != end) {
        TRANSITION* q = p + 1;
        int32_t sum = p->m_delta + previous;
        while (q != end && q->m_position == p->m_position) {
            sum += q->m_delta;
            q++;
        }
        if (previous > 0) {
            if (sum < 1) {
                out->m_position = p->m_position;
                out->m_delta = -1;
                out++;
            }
        } else if (sum >= 1) {
            out->m_position = p->m_position;
            out->m_delta = 1;
            out++;
        }
        previous = sum;
        p = q;
    }
    this->m_count = static_cast<uint32_t>(out - begin);
}

// ref: FUN_142167750
void KEYSTATE::Apply(uint8_t type, const SPAN* span) {
    switch (type) {
        case 1:
            this->m_present = 1;
            this->m_count = 0;
            this->m_span.m_offset = span->m_offset;
            this->m_span.m_size = span->m_size;
            this->m_span.m_pad = span->m_pad;
            this->AddSpan(span, true);
            return;
        case 2:
            this->m_present = 1;
            this->m_count = 0;
            this->m_span = *span;
            return;
        case 3:
            this->m_present = 0;
            this->m_span.m_offset = 0;
            this->m_span.m_size = 0;
            this->m_count = 0;
            return;
        case 4:
            StatsReport(this->m_stats, 0, "dynamic_container.key_state.mark_fully_resident", 1, s_fullRate);
            this->m_count = 0;
            this->AddSpan(&this->m_span, true);
            return;
        case 5:
            StatsReport(this->m_stats, 0, "dynamic_container.key_state.mark_fully_nonresident", 1, s_fullRate);
            this->m_count = 0;
            return;
        case 6:
            this->AddSpan(span, true);
            return;
        case 7:
            this->AddSpan(span, false);
            return;
        default:
            blz::log(g_logCategory, 4, s_keyStateFile, 299, "Unexpected key update type '%u'", type);
            return;
    }
}

// The state of a key from its sorted entries (the first is its span, the rest resident
// ranges), its updates, and any extra records, in that order.
// ref: FUN_142167b80
void KEYSTATE::Build(const uint8_t* sorted, const uint8_t* sortedEnd, uint8_t** updates, uint8_t** updatesEnd,
    const uint8_t* records, const uint8_t* recordsEnd) {
    this->m_count = 0;
    if (sorted && sorted < sortedEnd) {
        SPAN span = SpanOf(sorted + 9);
        if (sorted + SORTED_ENTRY == sortedEnd) {
            this->Apply(1, &span);
        } else {
            this->Apply(2, &span);
            for (const uint8_t* entry = sorted + SORTED_ENTRY; entry != sortedEnd; entry += SORTED_ENTRY) {
                span = SpanOf(entry + 9);
                this->Apply(6, &span);
            }
        }
    }
    if (updates && updates < updatesEnd) {
        for (; updates != updatesEnd; updates++) {
            const uint8_t* update = *updates;
            SPAN span = SpanOf(update + 9);
            this->Apply(update[0x12], &span);
        }
    }
    if (records && records < recordsEnd) {
        for (const uint8_t* record = records; record != recordsEnd; record += 0x13) {
            SPAN span = SpanOf(record + 9);
            this->Apply(record[0x12], &span);
        }
    }
    this->Normalize();
}

// Whether the span is wholly resident (or wholly not).
// ref: FUN_142167dc0
bool KEYSTATE::Covers(const SPAN* span, bool resident) {
    int32_t sum = 0;
    TRANSITION* p = this->m_transitions;
    TRANSITION* end = p + this->m_count;
    for (;;) {
        if (p == end) {
            return !resident;
        }
        if (span->m_offset < p->m_position) {
            break;
        }
        sum += p->m_delta;
        p++;
    }
    if (sum < 1) {
        if (resident) {
            return false;
        }
    } else if (!resident) {
        return false;
    }
    return uint64_t(span->m_size) + span->m_offset <= p->m_position;
}

// Whether an update of this type applies: adding a key that exists is FileExists; removing or
// marking one that does not is NotReserved; a span outside the key's is InvalidSpan; marking a
// span that already is in that state is a no-op.
// ref: FUN_142167e20
int32_t KEYSTATE::Validate(uint8_t type, const SPAN* span, bool* noop) {
    *noop = false;
    uint64_t offset;
    uint64_t end;
    TRANSITION* p;
    TRANSITION* last;
    int32_t sum = 0;
    uint64_t position;
    switch (type) {
        default:
            return ERR_UNSPECIFIED;
        case 1:
        case 2:
            return this->m_present ? ERR_FILE_EXISTS : ERR_SUCCESS;
        case 3:
            return this->m_present ? ERR_SUCCESS : ERR_NOT_RESERVED;
        case 6:
            if (!this->m_present) {
                return ERR_NOT_RESERVED;
            }
            offset = span->m_offset;
            if (offset < this->m_span.m_offset ||
                uint64_t(this->m_span.m_size) + this->m_span.m_offset < uint64_t(span->m_size) + offset) {
                return ERR_INVALID_SPAN;
            }
            p = this->m_transitions;
            end = uint64_t(span->m_size) + offset;
            last = p + this->m_count;
            if (p == last) {
                return ERR_SUCCESS;
            }
            while ((position = p->m_position) <= offset) {
                sum += p->m_delta;
                p++;
                if (p == last) {
                    return ERR_SUCCESS;
                }
            }
            if (sum < 1) {
                return ERR_SUCCESS;
            }
            break;
        case 7:
            if (!this->m_present) {
                return ERR_NOT_RESERVED;
            }
            offset = span->m_offset;
            if (offset < this->m_span.m_offset) {
                return ERR_INVALID_SPAN;
            }
            if (uint64_t(this->m_span.m_size) + this->m_span.m_offset < uint64_t(span->m_size) + offset) {
                return ERR_INVALID_SPAN;
            }
            p = this->m_transitions;
            end = uint64_t(span->m_size) + offset;
            last = p + this->m_count;
            if (p == last) {
                *noop = true;
                return ERR_SUCCESS;
            }
            while ((position = p->m_position) <= offset) {
                sum += p->m_delta;
                p++;
                if (p == last) {
                    *noop = true;
                    return ERR_SUCCESS;
                }
            }
            if (sum > 0) {
                return ERR_SUCCESS;
            }
            break;
    }
    if (position < end) {
        return ERR_SUCCESS;
    }
    *noop = true;
    return ERR_SUCCESS;
}

// ref: FUN_1421678e0
uint32_t KEYSTATE::RangeCount() {
    return this->m_count >> 1;
}

// ref: FUN_1421678c0
void KEYSTATE::Range(uint32_t index, uint64_t* start, uint64_t* end) {
    TRANSITION* pair = &this->m_transitions[index * 2];
    *start = pair[0].m_position;
    *end = pair[1].m_position;
}

// --- KEYMAPPINGTABLE ---------------------------------------------------------------------

// ref: FUN_14215a240
void KEYMAPPINGTABLE::Init() {
    this->m_mode = 2;
    this->m_bucket = 0;
    this->m_allocator = nullptr;
    this->m_stats = nullptr;
    OsMappingInit(&this->m_mapping);
    this->m_maxSize = 0;
    this->m_data = nullptr;
    this->m_size = 0;
    this->m_sorted = nullptr;
    this->m_sortedCount = 0;
    this->m_updates = nullptr;
    this->m_updatePages = 0;
}

// ref: FUN_14215c820
void KEYMAPPINGTABLE::Setup(blz::allocator* allocator, STATSINK* stats) {
    this->m_allocator = allocator;
    this->m_stats = stats;
}

// ref: FUN_14215c9a0
void KEYMAPPINGTABLE::Unbind() {
    if (this->m_mode == 2) {
        this->m_allocator->deallocate(this->m_data);
    } else {
        OsMappingClose(&this->m_mapping);
    }
    this->m_maxSize = 0;
    this->m_data = nullptr;
    this->m_size = 0;
    this->m_sorted = nullptr;
    this->m_sortedCount = 0;
    this->m_updates = nullptr;
    this->m_updatePages = 0;
}

// ref: FUN_14215a290 (with FUN_14215c9f0)
void KEYMAPPINGTABLE::Destroy() {
    this->Unbind();
    ReportInPageRetry(0, this->m_stats);
    OsMappingClose(&this->m_mapping);
}

// Reads (read-only mode) or maps (otherwise) a table file and checks its headers, the hash of
// its sorted section and of every update entry. An empty file gets a fresh table first.
// ref: FUN_14215a790 (reached through FUN_14215a700)
int32_t KEYMAPPINGTABLE::Bind(OSFILE* file, int32_t mode, uint32_t bucket, uint64_t maxSize) {
    if (this->m_mode == 2) {
        this->m_allocator->deallocate(this->m_data);
    } else {
        OsMappingClose(&this->m_mapping);
    }
    this->m_mode = mode;
    this->m_bucket = bucket;
    this->m_maxSize = 0;
    this->m_data = nullptr;
    this->m_size = 0;
    this->m_sorted = nullptr;
    this->m_sortedCount = 0;
    this->m_updates = nullptr;
    this->m_updatePages = 0;
    int32_t result;
    if (OsFileGetSize(file) == 0) {
        if (this->m_mode == 2) {
            return ERR_NEEDS_REPAIR;
        }
        result = CreateTableFile(this, file, maxSize);
        if (result != 0) {
            return result;
        }
    }
    uint64_t fileSize = OsFileGetSize(file);
    const char* message = nullptr;
    int32_t line = 0;
    if (fileSize - 1 > 0xFFFFFFFEull) {
        blz::log(g_logCategory, 4, s_kmtFile, 0x4d1, "Invalid KMT file size '%llu'.", fileSize);
        result = ERR_NEEDS_REPAIR;
        goto fail;
    }
    if (this->m_mode == 2) {
        this->m_size = static_cast<uint32_t>(fileSize);
        this->m_data = static_cast<uint8_t*>(this->m_allocator->allocate(fileSize & 0xFFFFFFFF, 0x10));
        uint32_t read = 0;
        uint32_t error = OsFileReadAt(file, this->m_data, this->m_size, 0, &read);
        if (error != 0 || read != this->m_size) {
            LogReadError(error);
            result = ErrorFromOs(error);
            if (result != 0) {
                goto fail;
            }
        }
    } else {
        uint32_t error = OsMappingOpen(&this->m_mapping, file, true, 0, 0);
        if (error != 0) {
            LogMapError(error);
            result = ErrorFromOs(error);
            if (result == 0) {
                return result;
            }
            goto fail;
        }
        this->m_data = static_cast<uint8_t*>(this->m_mapping.m_view);
        this->m_size = static_cast<uint32_t>(this->m_mapping.m_size);
    }

    result = ERR_SUCCESS;
    {
        uint32_t size = this->m_size;
        if (size < 8) {
            line = 0x507;
            message = "Truncated KMT file detected.";
            goto corrupt;
        }
        auto section = reinterpret_cast<uint32_t*>(this->m_data);
        if (size < uint64_t(section[0]) + 8) {
            line = 0x50e;
            message = "Truncated KMT file detected.";
            goto corrupt;
        }
        uint32_t pc = 0;
        uint32_t pb = 0;
        HashLittle2(section + 2, section[0], &pc, &pb);
        if (pc != section[1]) {
            line = 0x51c;
            message = "Corruption detected in KMT file. (invalid hash)";
            goto corrupt;
        }
        if (section[0] < 0x10) {
            blz::log(g_logCategory, 4, s_kmtFile, 0x522, "Unsupported KMT format detected.");
            result = ERR_UNSUPPORTED;
            goto done;
        }
        const uint8_t* header = this->m_data;
        if (header[10] != this->m_bucket) {
            line = 0x52a;
            message = "Corruption detected in KMT file. (invalid bucket index)";
            goto corrupt;
        }
        uint16_t revision;
        std::memcpy(&revision, header + 8, 2);
        if (revision < 7) {
            blz::log(g_logCategory, 4, s_kmtFile, 0x531, "Unsupported KMT revision '%u' detected.", revision);
            result = ERR_UNSUPPORTED;
            goto done;
        }
        if (header[0xc] != 4 || header[0xd] != 5 || header[0xe] != 9) {
            blz::log(g_logCategory, 4, s_kmtFile, 0x53b, "Unable to support KMT revision '%u'.", revision);
            result = ERR_UNSUPPORTED;
            goto done;
        }
        if (header[0xb] != 0) {
            blz::log(g_logCategory, 2, s_kmtFile, 0x540, "Unknown KMT flags '%u', ignoring.", header[0xb]);
        }
        std::memcpy(&this->m_maxSize, header + 0x10, 8);
        uint64_t sortedOffset = (uint64_t(section[0]) + 0x17) & ~uint64_t(0xF);
        if (sortedOffset >= this->m_size) {
            line = 0x54a;
            message = "Truncated sorted section in KMT file detected.";
            goto corrupt;
        }
        uint64_t remaining = this->m_size - sortedOffset;
        if (remaining < 8) {
            line = 0x56a;
            message = "Truncated KMT sorted section detected.";
            goto corrupt;
        }
        auto sortedSection = reinterpret_cast<uint32_t*>(this->m_data + sortedOffset);
        if (remaining < uint64_t(sortedSection[0]) + 8) {
            line = 0x571;
            message = "Truncated KMT sorted section detected.";
            goto corrupt;
        }
        this->m_sorted = reinterpret_cast<uint8_t*>(sortedSection + 2);
        this->m_sortedCount = sortedSection[0] / SORTED_ENTRY;
        uint32_t sortedPc = 0;
        uint32_t sortedPb = 0;
        for (uint32_t i = 0; i < this->m_sortedCount; i++) {
            HashLittle2(this->m_sorted + i * SORTED_ENTRY, SORTED_ENTRY, &sortedPc, &sortedPb);
        }
        if (sortedPc != sortedSection[1]) {
            line = 0x585;
            message = "Corruption detected in KMT file. (invalid sorted hash)";
            goto corrupt;
        }
        uint64_t updateOffset = (uint64_t(sortedSection[0]) + 0x1007 + sortedOffset) & ~uint64_t(0xFFF);
        if (updateOffset >= this->m_size) {
            line = 0x559;
            message = "Truncated updated section in KMT file detected.";
            goto corrupt;
        }
        remaining = this->m_size - updateOffset;
        if (remaining <= 0x77FF) {
            line = 0x595;
            message = "Truncated KMT update section detected.";
            goto corrupt;
        }
        this->m_updates = this->m_data + updateOffset;
        this->m_updatePages = static_cast<uint32_t>(remaining >> 9);
        for (uint32_t page = 0; page < this->m_updatePages; page++) {
            for (uint32_t i = 0; i < UPDATES_PER_PAGE; i++) {
                const uint8_t* entry = this->m_updates + page * UPDATE_PAGE + i * UPDATE_ENTRY;
                uint32_t hash = ReadU32(entry);
                if (hash == 0) {
                    goto done;
                }
                if ((HashLittle(entry + 4, 0x13, 0) | 0x80000000) != hash) {
                    line = 0x5b3;
                    message = "Corruption detected in KMT file. (invalid update hash)";
                    goto corrupt;
                }
            }
        }
        goto done;
    }
corrupt:
    blz::log(g_logCategory, 4, s_kmtFile, line, message);
    result = ERR_NEEDS_REPAIR;
done:
    if (result == 0) {
        return result;
    }
fail:
    if (this->m_mode == 2) {
        this->m_allocator->deallocate(this->m_data);
    } else {
        OsMappingClose(&this->m_mapping);
    }
    this->m_maxSize = 0;
    this->m_data = nullptr;
    this->m_size = 0;
    this->m_sorted = nullptr;
    this->m_sortedCount = 0;
    this->m_updates = nullptr;
    this->m_updatePages = 0;
    return result;
}

// ref: FUN_14215c7c0
bool KEYMAPPINGTABLE::HasUpdates() {
    bool dirty = this->m_updates != nullptr && ReadU32(this->m_updates) != 0;
    ReportInPageRetry(0, this->m_stats);
    return dirty;
}

// Distinct keys in the sorted section, plus adds and less removes since.
// ref: FUN_14215bca0 (reached through FUN_14215bc50)
uint32_t KEYMAPPINGTABLE::CountKeys() {
    int32_t count = 0;
    if (this->m_sortedCount != 0) {
        const uint8_t* previous = this->m_sorted;
        count = 1;
        for (uint32_t i = 1; i < this->m_sortedCount; i++) {
            const uint8_t* entry = this->m_sorted + i * SORTED_ENTRY;
            if (!SameKey(entry, previous)) {
                count++;
                previous = entry;
            }
        }
    }
    for (uint32_t page = 0; page < this->m_updatePages; page++) {
        for (uint32_t i = 0; i < UPDATES_PER_PAGE; i++) {
            const uint8_t* entry = this->m_updates + page * UPDATE_PAGE + i * UPDATE_ENTRY;
            if (ReadU32(entry) == 0) {
                goto done;
            }
            uint8_t type = entry[0x16];
            if (static_cast<uint8_t>(type - 1) < 2) {
                count++;
            } else if (type == 3) {
                count--;
            }
        }
    }
done:
    ReportInPageRetry(0, this->m_stats);
    return static_cast<uint32_t>(count);
}

// The key's sorted entries and updates resolved into `state`; `page` and `index` get where
// the next update goes (index 0x15 when the table is full).
// ref: FUN_14215be40
void KEYMAPPINGTABLE::Find(const uint8_t key[9], KEYSTATE* state, uint32_t* page, uint32_t* index) {
    uint32_t count = this->m_sortedCount;
    const uint8_t* entries = this->m_sorted;
    uint32_t lo = 0;
    uint32_t hi = count;
    uint32_t first = 0;
    uint32_t last = count;
    bool found = false;
    while (lo != hi) {
        uint32_t mid = (hi + lo) >> 1;
        int32_t compare = std::memcmp(key, entries + mid * SORTED_ENTRY, 9);
        if (compare == 0) {
            first = mid;
            last = mid + 1;
            found = true;
            break;
        }
        if (compare > 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (found) {
        while (first != 0 && SameKey(entries + (first - 1) * SORTED_ENTRY, key)) {
            first--;
        }
        while (last < count && SameKey(entries + last * SORTED_ENTRY, key)) {
            last++;
        }
    } else {
        first = lo;
        last = hi;
    }

    // Matching updates, in page order (a small vector with 32 inline slots in the reference).
    uint8_t* inlineUpdates[32];
    uint8_t** matches = inlineUpdates;
    uint32_t matchCount = 0;
    uint32_t matchCapacity = 32;
    bool onHeap = false;
    uint32_t pageIndex = 0;
    uint32_t entryIndex = 0;
    for (pageIndex = 0; pageIndex < this->m_updatePages; pageIndex++) {
        for (entryIndex = 0; entryIndex < UPDATES_PER_PAGE; entryIndex++) {
            uint8_t* entry = this->m_updates + pageIndex * UPDATE_PAGE + entryIndex * UPDATE_ENTRY;
            if (ReadU32(entry) == 0) {
                goto scanned;
            }
            if (SameKey(entry + 4, key)) {
                if (matchCount == matchCapacity) {
                    uint32_t grown = matchCapacity + (matchCapacity >> 1);
                    if (grown < matchCount + 1) {
                        grown = matchCount + 1;
                    }
                    if (grown < 8) {
                        grown = 8;
                    }
                    auto block = static_cast<uint8_t**>(this->m_allocator->allocate(uint64_t(grown) * 8, 8));
                    std::memcpy(block, matches, uint64_t(matchCount) * 8);
                    if (onHeap) {
                        this->m_allocator->deallocate(matches);
                    }
                    matches = block;
                    matchCapacity = grown;
                    onHeap = true;
                }
                matches[matchCount++] = entry + 4;
            }
        }
    }
scanned:
    state->Build(entries + first * SORTED_ENTRY, entries + last * SORTED_ENTRY, matches, matches + matchCount, nullptr,
        nullptr);
    if (page) {
        *page = pageIndex;
    }
    if (index) {
        *index = entryIndex;
    }
    if (onHeap) {
        this->m_allocator->deallocate(matches);
    }
}

// ref: FUN_14215bdb0 (reached through FUN_14215bd50)
int32_t KEYMAPPINGTABLE::Lookup(const uint8_t key[9], SPAN* span) {
    KEYSTATE state;
    state.Init(this->m_allocator, this->m_stats);
    int32_t result = ERR_SUCCESS;
    this->Find(key, &state, nullptr, nullptr);
    if (!state.m_present) {
        result = ERR_NOT_RESERVED;
    } else {
        *span = state.m_span;
    }
    state.Destroy();
    ReportInPageRetry(0, this->m_stats);
    return result;
}

// Resolves part of a key's span (an offset into it and a size, -1 for the rest) to container
// offsets, and checks that part is resident: 2 when the key is absent, 4 when the request runs
// past the span, 3 when it is not all resident.
// ref: FUN_14215c220
int32_t KEYMAPPINGTABLE::QuerySpan(const uint8_t key[9], const SPAN* request, SPAN* out, SPAN* stored) {
    KEYSTATE state;
    state.Init(this->m_allocator, this->m_stats);
    int32_t result = ERR_SUCCESS;
    this->Find(key, &state, nullptr, nullptr);
    if (!state.m_present) {
        result = ERR_NOT_RESERVED;
    } else {
        uint32_t spanSize = state.m_span.m_size;
        uint64_t start = request->m_offset;
        result = ERR_INVALID_SPAN;
        if (start <= spanSize) {
            uint32_t size = request->m_size;
            if (size == 0xFFFFFFFF) {
                size = spanSize - static_cast<uint32_t>(start);
            }
            if (size + start <= spanSize) {
                out->m_size = size;
                out->m_offset = start + state.m_span.m_offset;
                if (size == 0 || state.Covers(out, true)) {
                    if (stored) {
                        *stored = state.m_span;
                    }
                    result = ERR_SUCCESS;
                } else {
                    result = ERR_NOT_RESIDENT;
                }
            }
        }
    }
    state.Destroy();
    return result;
}

// ref: FUN_14215ad50
int32_t KEYMAPPINGTABLE::GetSpan(const uint8_t key[9], const SPAN* request) {
    SPAN resolved;
    int32_t result = this->QuerySpan(key, request, &resolved, nullptr);
    ReportInPageRetry(0, this->m_stats);
    return result;
}

// ref: FUN_14215c190
int32_t KEYMAPPINGTABLE::ResolveSpan(const uint8_t key[9], const SPAN* request, SPAN* out, SPAN* stored) {
    int32_t result = this->QuerySpan(key, request, out, stored);
    ReportInPageRetry(0, this->m_stats);
    return result;
}

// Appends an update for a key; every eighth page completed is flushed to disk.
// ref: FUN_14215cad0 (reached through FUN_14215ca40)
int32_t KEYMAPPINGTABLE::Update(const uint8_t key[9], uint8_t type, const SPAN* span, SPAN* previous) {
    if (type != 3 && !span) {
        return ERR_INVALID_PARAM;
    }
    KEYSTATE state;
    state.Init(this->m_allocator, this->m_stats);
    uint32_t page = 0;
    uint32_t index = 0;
    this->Find(key, &state, &page, &index);
    int32_t result;
    if (index == UPDATES_PER_PAGE) {
        result = ERR_CANCELED;
    } else {
        if (previous) {
            *previous = state.m_span;
        }
        // DIVERGED: the reference leaves the target uninitialized for a removal (type 3, no
        // span) and writes it into the update entry; it is zeroed here.
        SPAN target = {};
        if (span) {
            target.m_offset = span->m_offset + state.m_span.m_offset;
            target.m_size = span->m_size;
            target.m_pad = 0;
        }
        bool noop = false;
        result = state.Validate(type, &target, &noop);
        if (result == 0 && !noop) {
            uint8_t* entry = this->m_updates + page * UPDATE_PAGE + index * UPDATE_ENTRY;
            std::memcpy(entry + 4, key, 9);
            entry[0x16] = type;
            WriteBE40(entry + 0xd, target.m_offset);
            std::memcpy(entry + 0x12, &target.m_size, 4);
            uint32_t hash = HashLittle(entry + 4, 0x13, 0) | 0x80000000;
            std::memcpy(entry, &hash, 4);
            if (index == 0x14 && (page & 7) == 7) {
                uint64_t offset = uint64_t(page - 7) * UPDATE_PAGE - reinterpret_cast<uint64_t>(this->m_data) +
                    reinterpret_cast<uint64_t>(this->m_updates);
                // FUN_1423685a0: flush part of a mapped view.
                OSMAPPING* mapping = &this->m_mapping;
                uint32_t error;
                if (!mapping->m_view || mapping->m_size < 0x1000 || mapping->m_size - 0x1000 < offset) {
                    error = OsErrorFromResult(2);
                } else if (!FlushViewOfFile(static_cast<uint8_t*>(mapping->m_view) + offset, 0x1000) ||
                    !FlushFileBuffers(mapping->m_file)) {
                    error = GetLastError();
                } else {
                    error = 0;
                }
                if (error != 0) {
                    LogSyncError(error);
                }
            }
        }
    }
    state.Destroy();
    return result;
}

// Visits every key of the sorted section, the updates and the extra records in key order,
// with the run of each source that has the key.
// ref: FUN_14215b7b0
int32_t KEYMAPPINGTABLE::Merge(KMTVISITOR visitor, void* context, KMTRECORD* records, uint32_t count) {
    const uint8_t* sorted = this->m_sorted;
    const uint8_t* sortedEnd = sorted + this->m_sortedCount * SORTED_ENTRY;
    uint64_t updateSlots = uint64_t(this->m_updatePages) * UPDATES_PER_PAGE;
    auto updates = static_cast<uint8_t**>(this->m_allocator->allocate(updateSlots * 8, 8));
    if (updates) {
        std::memset(updates, 0, updateSlots * 8);
    }
    uint8_t** updatesEnd = updates;
    for (uint32_t page = 0; page < this->m_updatePages; page++) {
        for (uint32_t i = 0; i < UPDATES_PER_PAGE; i++) {
            uint8_t* entry = this->m_updates + page * UPDATE_PAGE + i * UPDATE_ENTRY;
            if (ReadU32(entry) == 0) {
                goto collected;
            }
            *updatesEnd++ = entry + 4;
        }
    }
collected:
    uint8_t* extra = nullptr;
    uint8_t* extraEnd = nullptr;
    if (count != 0) {
        extra = static_cast<uint8_t*>(this->m_allocator->allocate(uint64_t(count) * 0x13, 1));
        if (extra) {
            std::memset(extra, 0, uint64_t(count) * 0x13);
        }
        uint8_t* out = extra;
        for (uint32_t i = 0; i < count; i++, out += 0x13) {
            std::memcpy(out, records[i].m_key, 9);
            out[0x12] = 2;
            WriteBE40(out + 9, records[i].m_offset);
            std::memcpy(out + 0xe, &records[i].m_size, 4);
        }
        extraEnd = out;
    }
    // FUN_142159a30: std::sort of the 19-byte records by key.
    {
        struct RECORD19 {
            uint8_t m_bytes[0x13];
        };
        auto begin = reinterpret_cast<RECORD19*>(extra);
        auto end = reinterpret_cast<RECORD19*>(extraEnd);
        if (begin) {
            std::sort(begin, end, [](const RECORD19& a, const RECORD19& b) {
                return std::memcmp(a.m_bytes, b.m_bytes, 9) < 0;
            });
        }
    }
    // An insertion sort keeps the updates of a key in the order they were made.
    for (uint8_t** i = updates + 1; i < updatesEnd; i++) {
        uint8_t* value = *i;
        uint8_t** j = i;
        while (j != updates && std::memcmp(value, j[-1], 9) < 0) {
            *j = j[-1];
            j--;
        }
        *j = value;
    }

    int32_t result = 0;
    const uint8_t* a = sorted;
    uint8_t** b = updates;
    const uint8_t* c = extra;
    for (;;) {
        bool hasA = a != sortedEnd;
        bool hasB = b != updatesEnd;
        bool hasC = c != extraEnd;
        if (!hasA && !hasB && !hasC) {
            result = 0;
            break;
        }
        const uint8_t* key = nullptr;
        if (hasA) {
            key = a;
        }
        if (hasB && (!key || std::memcmp(*b, key, 9) < 0)) {
            key = *b;
        }
        if (hasC && (!key || std::memcmp(c, key, 9) < 0)) {
            key = c;
        }
        const uint8_t* aEnd = a;
        while (aEnd != sortedEnd && SameKey(aEnd, key)) {
            aEnd += SORTED_ENTRY;
        }
        uint8_t** bEnd = b;
        while (bEnd != updatesEnd && SameKey(*bEnd, key)) {
            bEnd++;
        }
        const uint8_t* cEnd = c;
        while (cEnd != extraEnd && SameKey(cEnd, key)) {
            cEnd += 0x13;
        }
        result = visitor(context, key, a, aEnd, b, bEnd, c, cEnd);
        a = aEnd;
        b = bEnd;
        c = cEnd;
        if (result != 0) {
            break;
        }
    }
    this->m_allocator->deallocate(extra);
    this->m_allocator->deallocate(updates);
    return result;
}

// ref: FUN_14215ae30 (reached through FUN_14215adc0)
int32_t KEYMAPPINGTABLE::Enumerate(KMTSPANVISITOR visitor, void* context, uint32_t skip) {
    if (!visitor) {
        return ERR_INVALID_PARAM;
    }
    ENUMERATOR enumerator = { this, visitor, context, skip };
    int32_t result = this->Merge(&EnumerateVisitor, &enumerator, nullptr, 0);
    ReportInPageRetry(0, this->m_stats);
    return result;
}

// Writes a new table file: the headers, then every key resolved into the sorted section
// (with its hash), padded for the update pages.
// ref: FUN_14215af80 (reached through FUN_14215af10/FUN_14215b400 and FUN_14215b470)
int32_t KEYMAPPINGTABLE::Write(OSFILE* file, KMTRECORD* records, uint32_t count) {
    uint8_t header[0x10];
    std::memcpy(header, this->m_data + 8, 0x10);
    header[0] = 7;
    header[1] = 0;
    uint32_t pc = 0;
    uint32_t pb = 0;
    HashLittle2(header, 0x10, &pc, &pb);
    uint32_t section[2] = { 0x10, pc };
    int32_t error = WriteAll(file, section, 8, "section header");
    if (error != 0) {
        return error;
    }
    error = WriteAll(file, header, 0x10, "file header");
    if (error != 0) {
        return error;
    }
    uint64_t position = OsFileTell(file);
    uint64_t sortedStart = (position + 0xF) & ~uint64_t(0xF);
    uint64_t dataStart = sortedStart + 8;
    error = ZeroFill(file, dataStart - position);
    if (error != 0) {
        return error;
    }
    uint32_t sortedPc = 0;
    uint32_t sortedPb = 0;
    uint64_t recordStart = OsFileTell(file);
    WRITEBUFFER buffer;
    buffer.m_allocator = this->m_allocator;
    buffer.m_file = file;
    buffer.m_count = 0;
    buffer.m_buffer = static_cast<uint8_t*>(this->m_allocator->allocate(180000, 0x10));
    buffer.m_cursor = buffer.m_buffer;
    WRITER writer = { this, &sortedPc, &sortedPb, &buffer };
    int32_t result = this->Merge(&WriteVisitor, &writer, records, count);
    if (result != 0) {
        if (buffer.m_buffer) {
            this->m_allocator->deallocate(buffer.m_buffer);
        }
        ReportInPageRetry(0, this->m_stats);
        return result;
    }
    if (buffer.m_count != 0) {
        uint32_t bytes = buffer.m_count * SORTED_ENTRY;
        uint32_t written = 0;
        uint32_t osError = OsFileWrite(file, buffer.m_buffer, bytes, &written);
        if (osError != 0 || written != bytes) {
            LogWriteError("buffered records", osError);
            result = ErrorFromOs(osError);
            if (result != 0) {
                if (buffer.m_buffer) {
                    this->m_allocator->deallocate(buffer.m_buffer);
                }
                ReportInPageRetry(0, this->m_stats);
                return result;
            }
        }
        buffer.m_cursor = buffer.m_buffer;
        buffer.m_count = 0;
    }
    uint64_t length = OsFileTell(file) - recordStart;
    if (length >= 0x100000000ull) {
        blz::log(g_logCategory, 4, s_kmtFile, 0x7ae, "Sorted KMT section exceeds addressable limits.");
        if (buffer.m_buffer) {
            this->m_allocator->deallocate(buffer.m_buffer);
        }
        ReportInPageRetry(0, this->m_stats);
        return ERR_UNSPECIFIED;
    }
    if (buffer.m_buffer) {
        this->m_allocator->deallocate(buffer.m_buffer);
    }
    uint32_t sortedSection[2] = { static_cast<uint32_t>(length), sortedPc };
    uint64_t end = dataStart + length;
    uint32_t written = 0;
    uint32_t osError = OsFileWriteAt(file, sortedSection, 8, sortedStart, &written);
    if (osError != 0 || written != 8) {
        LogWriteError("section header", osError);
        result = ErrorFromOs(osError);
        if (result != 0) {
            ReportInPageRetry(0, this->m_stats);
            return result;
        }
    }
    uint8_t zeros[0x1000];
    std::memset(zeros, 0, sizeof(zeros));
    for (uint64_t remaining = ((end + 0x17FFF) & ~uint64_t(0xFFFF)) - end; remaining != 0;) {
        uint32_t chunk = remaining < 0x1000 ? static_cast<uint32_t>(remaining) : 0x1000;
        written = 0;
        osError = OsFileWriteAt(file, zeros, chunk, end, &written);
        if (osError != 0 || written != chunk) {
            LogWriteError("zero fill", osError);
            result = ErrorFromOs(osError);
            if (result != 0) {
                ReportInPageRetry(0, this->m_stats);
                return result;
            }
        }
        end += chunk;
        remaining -= chunk;
    }
    ReportInPageRetry(0, this->m_stats);
    return 0;
}

}
