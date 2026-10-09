#include "base/Status.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include <new>

namespace {

const char s_nodeTag[] = ".?AU?$list_node@USTATUSENTRY@CStatus@@@blz@@";
const char s_stringTag[] = ".D";

// DAT_142ddfa84, the position slot 4 hands out.
uint64_t s_location;

}

CStatus::CStatus() {
    this->m_list.m_prev = reinterpret_cast<STATUSENTRY*>(&this->m_list);
    this->m_list.m_next = reinterpret_cast<STATUSENTRY*>(&this->m_list);
    this->m_list.m_count = 0;
    this->m_maxSeverity = 0;
}

// ref: FUN_1402c8fd0
CStatus::~CStatus() {
    this->m_list.Clear();
    this->m_maxSeverity = 0;
    this->m_list.Clear();
}

// ref: FUN_1402291d0
void CStatus::LIST::Clear() {
    auto sentinel = reinterpret_cast<STATUSENTRY*>(this);
    STATUSENTRY* node = this->m_next;
    this->m_next = sentinel;
    this->m_prev = sentinel;
    this->m_count = 0;
    while (node != sentinel) {
        STATUSENTRY* next = node->m_next;
        if (!node->m_text.IsInline()) {
            SMemFree(node->m_text.m_data, s_stringTag, -6, 0);
        }
        SMemFree(node, s_nodeTag, -6, 0);
        node = next;
    }
}

// ref: FUN_1402c9040
void CStatus::Add(const CStatus& other) {
    STATUSENTRY* node = other.m_list.m_next;
    if (static_cast<int32_t>(other.m_list.m_count) > 0) {
        for (uint32_t count = static_cast<uint32_t>(other.m_list.m_count); count != 0; count--) {
            this->Add(node->m_severity, "%s", node->m_text.m_data);
            node = node->m_next;
        }
    }
}

// ref: FUN_1402289a0
void* CStatus::GetLocation() {
    return &s_location;
}

// ref: FUN_1402c90a0
void CStatus::AddV(int32_t severity, const char* format, va_list args) {
    this->Insert(true, severity, format, args);
}

// ref: FUN_1402c97a0
void CStatus::PrependV(int32_t severity, const char* format, va_list args) {
    this->Insert(false, severity, format, args);
}

// ref: FUN_1402c9010
CStatus* CStatus::Add(int32_t severity, const char* format, ...) {
    va_list args;
    va_start(args, format);
    this->AddV(severity, format, args);
    va_end(args);
    return this;
}

// ref: FUN_1402c9570
void CStatus::Insert(bool append, int32_t severity, const char* format, va_list args) {
    blz::string text;
    int32_t length = SStrVSnprintf(nullptr, 0, format, args);
    if (length < 0) {
        text.m_data[0] = '\0';
        text.m_size = 0;
        return;
    }
    text.reserve(static_cast<uint64_t>(length));
    SStrVSnprintf(text.m_data, static_cast<uint64_t>(length) + 1, format, args);
    text.m_data[length] = '\0';
    text.m_size = static_cast<uint64_t>(length);
    if (length == 0) {
        return;
    }

    // FUN_1402c97c0
    auto node = static_cast<STATUSENTRY*>(SMemAlloc(sizeof(STATUSENTRY), 0xB, s_nodeTag, SStrHashFnv1a(s_nodeTag), -6, 0));
    if (node) {
        new (&node->m_text) blz::string();
        node->m_severity = 0;
    }
    auto sentinel = reinterpret_cast<STATUSENTRY*>(&this->m_list);
    if (append) {
        node->m_prev = sentinel->m_prev;
        node->m_next = sentinel;
        sentinel->m_prev = node;
    } else {
        STATUSENTRY* first = this->m_list.m_next;
        node->m_prev = first->m_prev;
        node->m_next = first;
        first->m_prev = node;
    }
    node->m_prev->m_next = node;
    this->m_list.m_count++;
    node->m_text.swap(text);
    node->m_severity = severity;
    if (this->m_maxSeverity < severity) {
        this->m_maxSeverity = severity;
    }
}

// ref: FUN_1402c90e0
void CStatus::ForEach(blz::function_storage* function) const {
    typedef void (*INVOKE)(int32_t severity, const blz::string* text, void* storage);
    auto sentinel = reinterpret_cast<const STATUSENTRY*>(&this->m_list);
    for (const STATUSENTRY* node = this->m_list.m_next; node != sentinel; node = node->m_next) {
        reinterpret_cast<INVOKE>(function->m_invoke)(node->m_severity, &node->m_text, function->m_storage);
    }
}
