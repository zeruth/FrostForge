#include "blz/String.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include <cstring>

namespace blz {

namespace {

const char s_tag[] = ".D";

}

string::~string() {
    if (!this->IsInline()) {
        SMemFree(this->m_data, s_tag, -6, 0);
    }
}

// ref: FUN_1402135c0
void string::reserve(uint64_t size) {
    uint64_t capacity = this->m_capacity;
    if ((capacity & ~INLINE) >= size) {
        return;
    }

    uint64_t grown = ((capacity >> 1) & 0x3FFFFFFFFFFFFFFFull) + (capacity & ~INLINE);
    if (grown < size) {
        grown = size;
    }
    this->m_capacity = ((capacity ^ grown) & INLINE) ^ grown;

    auto data = static_cast<char*>(SMemAlloc(grown + 1, 0xB, s_tag, SStrHashFnv1a(s_tag), -6, 0));
    if (this->m_size + 1 != 0) {
        std::memcpy(data, this->m_data, this->m_size + 1);
    }
    if (!this->IsInline()) {
        SMemFree(this->m_data, s_tag, -6, 0);
    }
    this->m_capacity &= ~INLINE;
    this->m_data = data;
}

void string::assign(const char* text) {
    uint64_t length = 0;
    if (*text != '\0') {
        length = std::strlen(text);
    }
    this->reserve(length);
    if (length != 0) {
        std::memcpy(this->m_data, text, length);
    }
    this->m_data[length] = '\0';
    this->m_size = length;
}

// ref: FUN_1402141a0
void string::construct(const char* text, uint64_t length) {
    this->m_size = 0;
    this->m_data = this->m_inline;
    this->m_capacity = INLINE | 15;
    this->m_inline[0] = '\0';
    this->reserve(length);
    if (length != 0) {
        std::memcpy(this->m_data, text, length);
    }
    this->m_data[length] = '\0';
    this->m_size = length;
}

namespace {

const char s_vectorTag[] = ".?AV?$basic_string@DU?$char_traits@D@blz@@V?$allocator@D@2@@blz@@";

}

// ref: FUN_140216ae0
void string_vector::reserve(uint64_t count) {
    uint64_t capacity = this->m_capacity;
    if ((capacity & ~string::INLINE) >= count) {
        return;
    }
    uint64_t grown = ((capacity >> 1) & 0x3FFFFFFFFFFFFFFFull) + (capacity & ~string::INLINE);
    if (grown < count) {
        grown = count;
    }
    this->m_capacity = ((capacity ^ grown) & string::INLINE) ^ grown;

    auto data = static_cast<string*>(SMemAlloc(grown * sizeof(string), 0xB, s_vectorTag, SStrHashFnv1a(s_vectorTag), -6, 0));
    string* dest = data;
    for (string* src = this->m_data; src != this->m_data + this->m_size; src++, dest++) {
        if (src->IsInline()) {
            dest->construct(src->m_data, src->m_size);
        } else {
            dest->m_capacity &= ~string::INLINE;
            dest->m_data = src->m_data;
            dest->m_size = src->m_size;
            dest->m_capacity = src->m_capacity & ~string::INLINE;
            src->m_data = src->m_inline;
            src->m_size = 0;
            src->m_capacity = string::INLINE | 15;
            src->m_inline[0] = '\0';
        }
    }
    for (string* old = this->m_data; old != this->m_data + this->m_size; old++) {
        if (!old->IsInline()) {
            SMemFree(old->m_data, s_tag, -6, 0);
        }
    }
    if (!(this->m_capacity & string::INLINE)) {
        SMemFree(this->m_data, s_vectorTag, -6, 0);
    }
    this->m_capacity &= ~string::INLINE;
    this->m_data = data;
}

// ref: FUN_140216200
void string_vector::destroy() {
    for (string* s = this->m_data; s != this->m_data + this->m_size; s++) {
        if (!s->IsInline()) {
            SMemFree(s->m_data, s_tag, -6, 0);
        }
    }
    if (!(this->m_capacity & string::INLINE)) {
        SMemFree(this->m_data, s_vectorTag, -6, 0);
        this->m_data = nullptr;
    }
}

string& string::operator=(string&& other) {
    uint64_t size = other.m_size;
    if (other.IsInline()) {
        uint64_t offset = static_cast<uint64_t>(other.m_data - this->m_data);
        this->reserve(size);
        if (size != 0) {
            if (offset < this->m_size) {
                std::memmove(this->m_data, this->m_data + offset, size);
            } else {
                std::memcpy(this->m_data, other.m_data, size);
            }
        }
        this->m_data[size] = '\0';
    } else {
        if (!this->IsInline()) {
            SMemFree(this->m_data, s_tag, -6, 0);
        }
        this->m_data = other.m_data;
        this->m_capacity = other.m_capacity & ~INLINE;
        other.m_data = other.m_inline;
        other.m_size = 0;
        other.m_capacity = INLINE | 15;
        other.m_inline[0] = '\0';
    }
    this->m_size = size;
    return *this;
}

// ref: FUN_140216cc0
void string::swap(string& other) {
    if (!this->IsInline() && !other.IsInline()) {
        char* data = this->m_data;
        this->m_data = other.m_data;
        other.m_data = data;
        uint64_t size = this->m_size;
        this->m_size = other.m_size;
        other.m_size = size;
        uint64_t capacity = this->m_capacity;
        this->m_capacity = ((other.m_capacity ^ capacity) & INLINE) ^ other.m_capacity;
        other.m_capacity = ((capacity ^ other.m_capacity) & INLINE) ^ capacity;
        return;
    }

    string* big = this;
    string* small = &other;
    if (this->m_size < other.m_size) {
        big = &other;
        small = this;
    }

    if ((other.m_capacity & ~INLINE) < this->m_size || (this->m_capacity & ~INLINE) < other.m_size) {
        if (big->IsInline()) {
            // FUN_140216a80
            auto data = static_cast<char*>(SMemAlloc(big->m_size + 1, 0xB, s_tag, SStrHashFnv1a(s_tag), -6, 0));
            if (big->m_size != 0) {
                std::memcpy(data, big->m_data, big->m_size);
            }
            if (small->m_size != 0) {
                std::memcpy(big->m_data, small->m_data, small->m_size);
            }
            if (!small->IsInline()) {
                SMemFree(small->m_data, s_tag, -6, 0);
            }
            small->m_capacity &= ~INLINE;
            small->m_data = data;
            small->m_capacity = big->m_size & ~INLINE;
        } else {
            auto data = static_cast<char*>(SMemAlloc(small->m_size + 1, 0xB, s_tag, SStrHashFnv1a(s_tag), -6, 0));
            if (small->m_size != 0) {
                std::memcpy(data, small->m_data, small->m_size);
            }
            small->m_capacity &= ~INLINE;
            small->m_data = big->m_data;
            small->m_capacity = big->m_capacity & ~INLINE;
            big->m_capacity &= ~INLINE;
            big->m_data = data;
            big->m_capacity = small->m_size & ~INLINE;
        }
    } else {
        char* a = small->m_data;
        char* b = big->m_data;
        for (char* end = a + small->m_size; a != end; a++, b++) {
            char c = *a;
            *a = *b;
            *b = c;
        }
        uint64_t rest = big->m_size - static_cast<uint64_t>(b - big->m_data);
        if (rest != 0) {
            std::memcpy(small->m_data + small->m_size, b, rest);
        }
    }

    uint64_t size = this->m_size;
    this->m_size = other.m_size;
    other.m_size = size;
    this->m_data[this->m_size] = '\0';
    other.m_data[other.m_size] = '\0';
}

}
