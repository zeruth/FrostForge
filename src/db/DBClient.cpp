#include "db/DBClient.hpp"

#include <new>

#include "base/ObjectAlloc.hpp"
#include "db/DB2Tables.hpp"
#include "fileservices/AsyncFile.hpp"
#include "storm/ErrorReport.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/Unimplemented.hpp"

#include <windows.h>

extern "C" {
int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);
unsigned long long FUN_14230c510();     // DBCTypes.cpp's heaps
}

void WowClientDB2SetHotfixRequests(bool enabled);

namespace {

const char s_db2File[] = "D:\\BuildServer\\A\\work-git\\wow\\WoW\\Source\\DB\\DBEngine\\WowClientDB2.cpp";
const char s_sparseFile[] = "D:\\BuildServer\\A\\work-git\\wow\\WoW\\Source\\DB\\DBEngine\\WowClientSparseDB2.cpp";

// The patched-record cache and the record-callback heap; the sparse tables' async read data.
// Each at its own address, so translated code shares them (the port's heap has the reference's
// layout: size 0x08, counts 0x10 / 0x18, heap id 0x28). Untied until 2026-10-21, and a sparse
// table's first record (FUN_140579580) faulted on the image's null cache.
TObjectAllocHeap* s_recordCache;  // DAT_142e8e2a0
uint32_t s_recordCallbackHeap;  // DAT_142e8e2d0
TObjectAllocHeap* s_sparseReadData;  // DAT_142e8e2f8

// Whether groups 1 and 2 are loaded (DAT_142e30491, DAT_142e30490).
bool s_group1Loaded;
bool s_group2Loaded;

// The default load flags (DAT_142ccdf30): bit 1 reports errors.
uint32_t s_loadFlags = 3;  // DAT_142ccdf30

// The registry walk the cache's init does: every table, through vtable slot 0x08.
void ForEachGroupTable(int32_t first, int32_t last, void (*visit)(WowClientDB2_Base* table)) {
    for (int32_t g = first; g < last; g++) {
        for (WowClientDB2_Base** table = g_db2LoadGroups[g].m_tables; *table != nullptr; table++) {
            visit(*table);
        }
    }
}

}

// ref: FUN_14057d1b0: a table's load with the given flags (4: the defaults).
bool WowClientDB2LoadWithFlags(WowClientDB2_Base* table, uint32_t flags) {
    if (flags == 4) {
        flags = s_loadFlags;
    }
    return table->Load(((flags >> 1) & 1) != 0);
}

// ref: FUN_14057cfa0
void WowClientDB2InitializeCache() {
    void* memory = SMemAlloc(sizeof(TObjectAllocHeap), 0x24, s_db2File, SStrHashFnv1a(s_db2File), 0x194, 0);
    s_recordCache = new (memory) TObjectAllocHeap("DBCACHE", 0x28, 0x400, 0x24);
    s_recordCallbackHeap = ObjectAllocAddHeap(0x3c, 0x100, "DB Rec Callbacks", 0x24, false);
    // Every table's slot 0x08 (a no-op), and its address range for crash reports.
    uintptr_t low = ~uintptr_t(0);
    uintptr_t high = 0;
    for (int32_t g = 0; g < DB2_LOAD_GROUP_COUNT; g++) {
        for (WowClientDB2_Base** table = g_db2LoadGroups[g].m_tables; *table != nullptr; table++) {
            (*table)->Slot08();
            auto at = reinterpret_cast<uintptr_t>(*table);
            if (at < low) {
                low = at;
            }
            if (high < at) {
                high = at;
            }
        }
    }
    if (low <= high) {
        ErrorReportAddMemoryRange(reinterpret_cast<void*>(low), (high - low) + 0x388);
    }
}

// ref: FUN_1405a3ce0
void WowClientSparseDB2Initialize() {
    void* memory = SMemAlloc(sizeof(TObjectAllocHeap), 0x24, s_sparseFile, SStrHashFnv1a(s_sparseFile), 0x5b, 0);
    s_sparseReadData = new (memory) TObjectAllocHeap("AsyncDBReadCallbackData", 0x448, 0x40, 0x24);
    // FUN_14230c510: DBCTypes.cpp's heaps (the table at DAT_1434913c8), which a sparse record's
    // strings and arrays come from: the reference's own, translated. A stub until 2026-10-21,
    // and the first Spell record read on entering the world allocated from a null heap.
    __try {
        FUN_14230c510();
    } __except (GenFault(0x14230c510ull, GetExceptionInformation())) {
    }
}

// ref: FUN_140351bf0
void ClientDBInitialize() {
    WowClientDB2SetHotfixRequests(true);
    WowClientDB2InitializeCache();
    WowClientSparseDB2Initialize();
    ForEachGroupTable(0, 2, [](WowClientDB2_Base* table) {
        WowClientDB2LoadWithFlags(table, 4);
    });
    ForEachGroupTable(4, 6, [](WowClientDB2_Base* table) {
        AsyncFilePrefetchById(table->m_meta->m_fileDataId);
    });
    ForEachGroupTable(2, 4, [](WowClientDB2_Base* table) {
        AsyncFilePrefetchById(table->m_meta->m_fileDataId);
    });
}

// ref: FUN_140351d40
void ClientDBLoadGroup1() {
    ForEachGroupTable(2, 4, [](WowClientDB2_Base* table) {
        WowClientDB2LoadWithFlags(table, 4);
    });
    s_group1Loaded = true;
}

// ref: FUN_140351db0
void ClientDBLoadGroup2() {
    ForEachGroupTable(4, 6, [](WowClientDB2_Base* table) {
        WowClientDB2LoadWithFlags(table, 4);
    });
    // DAT_142e71900 (a lookup walked through FUN_14051bca0 / FUN_14022b1b0) and FUN_14167a850.
    FROST_UNIMPLEMENTED();
    s_group2Loaded = true;
}

// ref: FUN_140351f70: reloads every loaded table.
void ClientDBReload() {
    ForEachGroupTable(0, 2, [](WowClientDB2_Base* table) {
        table->Reload(true);
    });
    ForEachGroupTable(4, 6, [](WowClientDB2_Base* table) {
        table->Reload(true);
    });
    ForEachGroupTable(2, 4, [](WowClientDB2_Base* table) {
        table->Reload(true);
    });
}

// ref: FUN_140352060
void ClientDBShutdown() {
    // FUN_14230b520: the parameter curves' teardown.
    ForEachGroupTable(0, 2, [](WowClientDB2_Base* table) {
        table->Unload(false);
    });
    if (s_group1Loaded) {
        ForEachGroupTable(2, 4, [](WowClientDB2_Base* table) {
            table->Unload(false);
        });
        s_group1Loaded = false;
    }
    if (s_group2Loaded) {
        ForEachGroupTable(4, 6, [](WowClientDB2_Base* table) {
            table->Unload(false);
        });
        s_group2Loaded = false;
    }
    // FUN_140581a10: the cache's teardown.
    if (s_recordCache != nullptr) {
        s_recordCache->~TObjectAllocHeap();
        SMemFree(s_recordCache, s_db2File, 0, 0);
        s_recordCache = nullptr;
    }
    ObjectAllocDestroyHeap(s_recordCallbackHeap);
    s_recordCallbackHeap = 0;
    WowClientDB2SetHotfixRequests(false);
}

bool ClientDBGroup1Loaded() {
    return s_group1Loaded;
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142ccdf30 = (void*)&s_loadFlags;
extern "C" void* const gen_p_142e8e2a0 = (void*)&s_recordCache;
extern "C" void* const gen_p_142e8e2d0 = (void*)&s_recordCallbackHeap;
extern "C" void* const gen_p_142e8e2f8 = (void*)&s_sparseReadData;
// ---- end of generated globals ----
