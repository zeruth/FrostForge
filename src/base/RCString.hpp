#ifndef BASE_RCSTRING_HPP
#define BASE_RCSTRING_HPP

// Engine/Source/Base/RCString.cpp. In 3.4.3 an RCString is a single pointer into a
// process-wide pool of interned strings: equal strings share one copy, which is never freed
// until the pool goes at shutdown. (3.3.5's was a reference-counted object; this is not.)
class RCString {
    public:
    const char* m_str = nullptr;

    const char* GetString() const {
        return this->m_str;
    }

    // Returns the pool's copy of `str`, adding it on first sight. `this` is passed and not
    // used: each call site stores the result into m_str itself, with its own null handling.
    const char* Intern(const char* str);
};

// Frees every interned string and the pool itself (process shutdown).
void RCStringDestroyPool();

// Shutdown step: RCStringDestroyPool, then a call to an empty function.
void RCStringShutdown();
// Whether the string pool is gone (or not made yet).
bool RCStringPoolIsEmpty();

#endif
