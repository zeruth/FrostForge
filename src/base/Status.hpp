#ifndef BASE_STATUS_HPP
#define BASE_STATUS_HPP

// CStatus: a list of formatted messages, each with a severity, and the highest severity seen.
// Startup and CAS take one to report into (vtable 0x142619430).

#include "blz/Function.hpp"
#include "blz/String.hpp"
#include <cstdarg>
#include <cstdint>

class CStatus {
    public:
    // blz::list_node<CStatus::STATUSENTRY> (0x40 bytes).
    struct STATUSENTRY {
        STATUSENTRY* m_prev;        // 0x00
        STATUSENTRY* m_next;        // 0x08
        blz::string m_text;         // 0x10
        int32_t m_severity;         // 0x38
    };

    // The list's sentinel: prev, next, count (blz::list).
    struct LIST {
        STATUSENTRY* m_prev;        // 0x08
        STATUSENTRY* m_next;        // 0x10
        uint64_t m_count;           // 0x18

        void Clear();
    };

    LIST m_list;                    // 0x08
    int32_t m_maxSeverity;          // 0x20

    CStatus();
    virtual ~CStatus();
    // Slots 1 and 3 are empty in the base class (_guard_check_icall); their purpose is unknown.
    virtual void Unknown08() {}
    virtual void Add(const CStatus& other);
    virtual void Unknown18() {}
    virtual void* Unknown20();
    virtual void AddV(int32_t severity, const char* format, va_list args);
    virtual void PrependV(int32_t severity, const char* format, va_list args);

    // Calls `function` (invoked as (severity, text, storage)) for each entry, oldest first.
    void ForEach(blz::function_storage* function) const;

    // The variadic front of AddV.
    CStatus* Add(int32_t severity, const char* format, ...);

    private:
    void Insert(bool append, int32_t severity, const char* format, va_list args);
};
static_assert(sizeof(CStatus) == 0x28, "CStatus is 0x28 bytes");

#endif
