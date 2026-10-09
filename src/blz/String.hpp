#ifndef BLZ_STRING_HPP
#define BLZ_STRING_HPP

#include <cstdint>

namespace blz {

// What every blz string starts with, whatever the size of its inline buffer: the reference's
// reserve (FUN_1402135c0) and printf (FUN_140236510) work on this alone, so one copy of each
// serves blz::string (16 bytes inline) and inline_string<N>.
struct string_header {
    char* m_data;
    uint64_t m_size;
    uint64_t m_capacity;            // bit 63: m_data is the inline buffer
};

// Grows the buffer to hold at least `size` characters plus the terminator (see string::reserve).
void string_reserve(string_header* text, uint64_t size);

// Replaces the contents with `length` characters of `source` (FUN_1402142b0).
string_header* string_assign(string_header* text, const char* source, uint64_t length);

// Appends `length` characters of `source` (which may lie inside the string).
string_header* string_append(string_header* text, const char* source, uint64_t length);

// Replaces the contents with the formatted text; an empty string for a bad format.
void string_format(string_header* text, const char* format, ...);

// Appends the formatted text.
void string_append_format(string_header* text, const char* format, ...);

// Frees a heap buffer (the strings' destructor).
void string_free(string_header* text);

// blz::string: data, size, capacity, then a 16-byte inline buffer. Bit 63 of the capacity
// word says the data is the inline buffer (capacity 15); clear means a heap block tagged ".D".
class string {
    public:
    static constexpr uint64_t INLINE = 0x8000000000000000ull;

    char* m_data;
    uint64_t m_size;
    uint64_t m_capacity;
    char m_inline[16];

    string() : m_data(m_inline), m_size(0), m_capacity(INLINE | 15) {
        this->m_inline[0] = '\0';
    }
    ~string();

    string(const string&) = delete;
    string& operator=(const string&) = delete;

    // Move construction, as the containers inline it: an inline source is copied, a heap
    // source hands its block over and is left empty.
    string(string&& other) {
        if (other.IsInline()) {
            this->construct(other.m_data, other.m_size);
        } else {
            this->m_data = other.m_data;
            this->m_size = other.m_size;
            this->m_capacity = other.m_capacity & ~INLINE;
            other.m_data = other.m_inline;
            other.m_size = 0;
            other.m_capacity = INLINE | 15;
            other.m_inline[0] = '\0';
        }
    }

    bool IsInline() const {
        return (this->m_capacity & INLINE) != 0;
    }

    // Grows the buffer to hold at least `size` characters plus the terminator: half again
    // the current capacity, or `size` if that is more. The contents move over.
    void reserve(uint64_t size);

    // The constructor from a C string, as the reference inlines it: an empty inline string,
    // reserve(length), the copy, the terminator.
    void assign(const char* text);

    // Move assignment. An inline source is copied (allowing for a source inside this
    // string's own buffer); a heap source hands its block over.
    string& operator=(string&& other);

    // Constructs in place from a pointer and length (raw storage: nothing is freed first).
    void construct(const char* text, uint64_t length);

    // Exchanges the contents. Two heap strings trade blocks; otherwise the characters move,
    // and a side that cannot hold the other's text gets a new block.
    void swap(string& other);
};

static_assert(sizeof(string) == 0x28, "blz::string is 0x28 bytes");

// A blz string with an N-byte inline buffer (capacity N - 1 before it spills to the heap).
template <uint64_t N>
class inline_string {
    public:
    char* m_data;
    uint64_t m_size;
    uint64_t m_capacity;
    char m_inline[N];

    inline_string() : m_data(m_inline), m_size(0), m_capacity(string::INLINE | (N - 1)) {
        this->m_inline[0] = '\0';
    }
    ~inline_string() {
        string_free(this->header());
    }

    inline_string(const inline_string&) = delete;
    inline_string& operator=(const inline_string&) = delete;

    string_header* header() {
        return reinterpret_cast<string_header*>(this);
    }

    bool IsInline() const {
        return (this->m_capacity & string::INLINE) != 0;
    }
};

// blz::vector<blz::string>: data, size, capacity (bit 63 set: the storage is not owned).
class string_vector {
    public:
    string* m_data;
    uint64_t m_size;
    uint64_t m_capacity;

    uint64_t capacity() const {
        return this->m_capacity & ~string::INLINE;
    }

    // Grows to hold at least `count`: half again the capacity, or `count` if that is more.
    // Heap strings move over by pointer; inline ones are copied.
    void reserve(uint64_t count);

    // Frees the strings' blocks and the array (if owned); the size is left alone.
    void destroy();
};

}

#endif
