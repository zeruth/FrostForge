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
    // Slot 1 is empty in the base class (_guard_check_icall); its purpose is unknown. Slots 3
    // and 4 set and return the script file position messages are reported against; the base
    // class ignores the file and hands out a shared dummy position.
    virtual void Unknown08() {}
    virtual void Add(const CStatus& other);
    virtual void SetFile(const uint32_t* file) {}
    virtual void* GetLocation();
    virtual void AddV(int32_t severity, const char* format, va_list args);
    virtual void PrependV(int32_t severity, const char* format, va_list args);

    // Calls `function` (invoked as (severity, text, storage)) for each entry, oldest first.
    void ForEach(blz::function_storage* function) const;

    // Whether nothing was reported (FUN_1402c9560).
    bool IsEmpty() const {
        return this->m_list.m_count == 0;
    }
    // The messages of at least `minSeverity`, each followed by `separator`.
    blz::string* Join(blz::string* out, int32_t minSeverity, const char* separator) const;

    // The variadic front of AddV.
    CStatus* Add(int32_t severity, const char* format, ...);

    private:
    void Insert(bool append, int32_t severity, const char* format, va_list args);
};
static_assert(sizeof(CStatus) == 0x28, "CStatus is 0x28 bytes");

#endif
