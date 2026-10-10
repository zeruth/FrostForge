#ifndef DB_DYNAMIC_LIFE_TIME_DB_HPP
#define DB_DYNAMIC_LIFE_TIME_DB_HPP

// DynamicLifeTimeDB<T>: a table that is not kept loaded. Its users hold it while they read it;
// the first hold loads it (or asks for its file when it is not on disk) and the last release
// unloads it again. The reference has one instance per table, each a block of globals laid out
// as below (GlobalStrings at DAT_1431e9ee8, GlobalColor at DAT_14311bf98).

#include <cstdint>

struct CAsyncObject;
class WowClientDB2_Base;

class DynamicLifeTimeDB {
    public:
    // A caller waiting on the file: told 1 when the table loaded, 0 when the file failed.
    struct WAITER {
        void (*m_callback)(int32_t loaded, void* param);
        void* m_param;
    };

    explicit DynamicLifeTimeDB(WowClientDB2_Base* table) : m_table(table) {}

    // Holds the table, loading it now when its file is on disk and asking for the file when it
    // is not. The two reference shapes differ in whether a loaded table is counted.
    void AcquireIfUnloaded();   // the GlobalStrings shape (inline in FUN_14196ff50)
    void Acquire();             // the GlobalColor shape (inline in FUN_1416728e0)
    // Blocks until a pending file request has landed.
    void Wait();
    // Drops a hold, unloading the table after the last.
    void ReleaseIfLoaded();     // the GlobalStrings shape (FUN_141970330)
    void Release();             // the GlobalColor shape (inline in FUN_1416728e0)

    CAsyncObject* m_request = nullptr;  // 0x00
    WAITER* m_waiters = nullptr;        // 0x08
    uint64_t m_waiterCount = 0;         // 0x10
    uint64_t m_waiterCapacity = 0;      // 0x18
    int32_t m_refs = 0;                 // 0x20
    uint8_t m_loadedOnce = 0;           // 0x24
    WowClientDB2_Base* m_table;         // the table the reference's accessor returns

    private:
    void Load();
};

// A table loaded before: replay its cached hotfixes, then load it again (FUN_140574300).
void WowClientDB2ReloadWithHotfixes(WowClientDB2_Base* table);

#endif
