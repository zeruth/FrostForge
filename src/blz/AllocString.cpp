#include "blz/AllocString.hpp"
#include <cstring>

namespace blz {

// Inline-constructed into an empty string (the constructors inline this everywhere).
void alloc_string::Init(allocator* allocator) {
    this->m_allocator = reinterpret_cast<uint64_t>(allocator) | 1;
    this->m_inline[0] = '\0';
    this->m_inline[23] = INLINE_CAPACITY;
}

// The destructor is inlined everywhere: an out-of-line string returns its text.
void alloc_string::Destroy() {
    if (!this->IsInline()) {
        this->Allocator()->deallocate(this->m_data);
    }
}

// Grows out of line to at least `capacity` characters (half again the old capacity, at least 8)
// and keeps the text.
// ref: FUN_142365640
void alloc_string::Reserve(uint32_t capacity) {
    char* text;
    uint32_t length;
    uint32_t current;
    if (!this->IsInline()) {
        text = this->m_data;
        length = this->m_size;
        current = this->m_capacity;
    } else {
        text = this->m_inline;
        current = INLINE_CAPACITY;
        length = INLINE_CAPACITY - static_cast<uint8_t>(this->m_inline[23]);
    }
    if (current >= capacity) {
        return;
    }
    current += current >> 1;
    if (current < capacity) {
        current = capacity;
    }
    if (current < 8) {
        current = 8;
    }
    auto data = static_cast<char*>(this->Allocator()->allocate(current + 1, 1));
    std::memcpy(data, text, length);
    if (!this->IsInline()) {
        this->Allocator()->deallocate(text);
    }
    this->m_allocator &= ~uint64_t(1);
    this->m_capacity = current;
    this->m_data = data;
    data[length] = '\0';
    this->m_size = length;
}

// ref: FUN_1423653f0
void alloc_string::Assign(const char* text, uint32_t length) {
    if (length == 0) {
        if (!this->IsInline()) {
            this->m_allocator &= ~uint64_t(1);
            this->m_data[0] = '\0';
            this->m_size = 0;
            return;
        }
        this->m_allocator |= 1;
        this->m_inline[0] = '\0';
        this->m_inline[23] = INLINE_CAPACITY;
        return;
    }
    if (length > INLINE_CAPACITY) {
        this->Reserve(length);
        std::memcpy(this->m_data, text, length);
        this->m_allocator &= ~uint64_t(1);
        this->m_data[length] = '\0';
        this->m_size = length;
        return;
    }
    // Short text always goes inline, freeing an out-of-line buffer first.
    if (!this->IsInline()) {
        this->Allocator()->deallocate(this->m_data);
    }
    std::memcpy(this->m_inline, text, length);
    this->m_allocator |= 1;
    this->m_inline[length] = '\0';
    this->m_inline[23] = static_cast<char>(INLINE_CAPACITY - length);
}

// ref: FUN_142365500
void alloc_string::Insert(uint32_t position, const char* text, uint32_t length) {
    char* data;
    uint32_t size;
    uint32_t capacity;
    if (!this->IsInline()) {
        data = this->m_data;
        size = this->m_size;
        capacity = this->m_capacity;
    } else {
        data = this->m_inline;
        capacity = INLINE_CAPACITY;
        size = INLINE_CAPACITY - static_cast<uint8_t>(this->m_inline[23]);
    }
    uint32_t total = size + length;
    if (capacity < total) {
        this->Reserve(total);
        data = this->m_data;
    }
    if (size - position != 0) {
        std::memmove(data + position + length, data + position, size - position);
    }
    std::memcpy(data + position, text, length);
    if (!this->IsInline()) {
        this->m_allocator &= ~uint64_t(1);
        this->m_data[total] = '\0';
        this->m_size = total;
    } else {
        this->m_allocator |= 1;
        this->m_inline[total] = '\0';
        this->m_inline[23] = static_cast<char>(INLINE_CAPACITY - total);
    }
}

// Constructs this from `source` with the source's allocator: the buffer is stolen when the
// allocators are the same, copied otherwise; the source is left empty and inline.
// ref: FUN_1420de980
void alloc_string::MoveConstruct(alloc_string* source) {
    uint64_t allocator = source->m_allocator;
    this->m_inline[0] = '\0';
    this->m_inline[23] = INLINE_CAPACITY;
    this->m_allocator = allocator | 1;
    if (source == this) {
        return;
    }
    if (((source->m_allocator ^ (allocator | 1)) & ~uint64_t(1)) != 0) {
        if (source->IsInline()) {
            this->Assign(source->m_inline, INLINE_CAPACITY - static_cast<uint8_t>(source->m_inline[23]));
        } else {
            this->Assign(source->m_data, source->m_size);
        }
        return;
    }
    std::memcpy(this, source, sizeof(*this));
    source->m_allocator |= 1;
    source->m_inline[0] = '\0';
    source->m_inline[23] = INLINE_CAPACITY;
}

// Sets the length (the text past the old length is left as it was), terminated.
// ref: FUN_1423655e0
void alloc_string::Resize(uint32_t length) {
    if (this->IsInline() && length < 0x18) {
        this->m_allocator |= 1;
        this->m_inline[length] = 0;
        this->m_inline[23] = static_cast<char>(INLINE_CAPACITY - length);
        return;
    }
    this->Reserve(length);
    this->m_allocator &= ~uint64_t(1);
    this->m_data[length] = 0;
    this->m_size = length;
}

}
