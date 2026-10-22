#include "storm/Memory.hpp"
#include "storm/ErrorReport.hpp"
#include "util/FrostLog.hpp"
#include "util/Unimplemented.hpp"
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <windows.h>
#include <psapi.h>
#include <malloc.h>
#pragma comment(lib, "psapi.lib")

namespace {

// ---- the debug guard heap (not the reference's) -------------------------------------------------
// FROST_GUARD_HEAP=1 puts every Storm block flush against a no-access page and decommits it on
// free, so a write past a block or a use after free faults at the instruction that does it rather
// than as heap corruption at some later free: page heap for the blocks the port allocates, without
// the admin rights gflags needs. The CRT path below is otherwise unchanged; the mode is read once.
constexpr uint64_t kPage = 0x1000;

// FROST_GUARD_HEAP=world guards only what is allocated from Enter World on (MemoryGuardStart):
// the Battle.net TLS login does not survive the guard heap (schannel faulting in its handshake),
// and the corruption hunted was in the world. Blocks from before stay the CRT's; a free tells the
// two apart by the marker at the base of a guard reservation.
constexpr uint64_t kGuardMagic = 0x4755415244424c4bull;

int GuardMode() {
    static const int s_mode = [] {
        char value[8] = {};
        if (!GetEnvironmentVariableA("FROST_GUARD_HEAP", value, sizeof(value))) {
            return 0;
        }
        return value[0] == '1' ? 1 : value[0] == 'w' ? 2 : 0;
    }();
    return s_mode;
}

volatile long s_guardStarted = 0;

// Whether new blocks are guarded.
bool GuardHeap() {
    int mode = GuardMode();
    return mode == 1 || (mode == 2 && s_guardStarted != 0);
}

// Whether a block came from the guard heap (in world mode, blocks of both kinds are live).
bool GuardBlock(void* block) {
    if (GuardMode() == 1) {
        return true;
    }
    if (GuardMode() == 0 || s_guardStarted == 0 || block == nullptr) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(block, &info, sizeof(info)) || info.AllocationBase == nullptr || info.Type != MEM_PRIVATE) {
        return false;
    }
    return reinterpret_cast<const uint64_t*>(info.AllocationBase)[1] == kGuardMagic;
}

// The reservation's size sits in the 16 bytes at its base, ahead of the block.
void* GuardAlloc(uint64_t bytes, bool zero) {
    (void)zero;  // fresh pages are zero
    uint64_t pages = (bytes + 0x10 + kPage - 1) / kPage;
    uint64_t total = (pages + 1) * kPage;
    auto base = static_cast<uint8_t*>(VirtualAlloc(nullptr, total, MEM_RESERVE, PAGE_NOACCESS));
    if (!base || !VirtualAlloc(base, pages * kPage, MEM_COMMIT, PAGE_READWRITE)) {
        return nullptr;
    }
    *reinterpret_cast<uint64_t*>(base) = total;
    reinterpret_cast<uint64_t*>(base)[1] = kGuardMagic;
    // bytes is a multiple of 16 on every Storm path, so the block stays 16-aligned
    return base + pages * kPage - bytes;
}

uint8_t* GuardBase(void* block) {
    MEMORY_BASIC_INFORMATION info;
    VirtualQuery(block, &info, sizeof(info));
    return static_cast<uint8_t*>(info.AllocationBase);
}

// Each freed reservation's freeing stack, so a later fault on its pages names who freed it
// (MemoryDescribeFreed). The reservation is never handed out again, so its base stays its key.
struct GUARDFREE {
    void* m_frames[20];
    uint16_t m_count;
};
std::mutex s_guardFreesLock;
std::unordered_map<uint8_t*, GUARDFREE>* s_guardFrees;
// (the map's own nodes are Storm blocks too, freed through here: not recorded, nor relocked)
thread_local bool t_inGuardRecord;

void GuardFree(void* block) {
    uint8_t* base = GuardBase(block);
    if (!t_inGuardRecord) {
        GUARDFREE entry;
        entry.m_count = CaptureStackBackTrace(1, 20, entry.m_frames, nullptr);
        std::lock_guard<std::mutex> lock(s_guardFreesLock);
        t_inGuardRecord = true;
        if (s_guardFrees == nullptr) {
            s_guardFrees = new std::unordered_map<uint8_t*, GUARDFREE>();
        }
        (*s_guardFrees)[base] = entry;
        t_inGuardRecord = false;
    }
    // decommitted and left reserved: the address is never handed out again
    VirtualFree(base, *reinterpret_cast<uint64_t*>(base) - kPage, MEM_DECOMMIT);
}

void* GuardRealloc(void* block, uint64_t bytes) {
    if (block == nullptr) {
        return GuardAlloc(bytes, false);
    }
    uint8_t* base = GuardBase(block);
    uint64_t old = base + *reinterpret_cast<uint64_t*>(base) - kPage - static_cast<uint8_t*>(block);
    void* fresh = GuardAlloc(bytes, false);
    if (fresh) {
        std::memcpy(fresh, block, old < bytes ? old : bytes);
        GuardFree(block);
    }
    return fresh;
}

// The heap's live bytes, and a ceiling past which the process stops: a translated function that
// faulted hands its caller garbage, and on the first entry to the world one built a container
// from it until 16 GB were committed and the machine stalled (2026-10-21). Past the ceiling the
// allocation that crossed it is logged with its stack, and the process ends.
constexpr int64_t kLiveCeiling = 4ll << 30;
volatile int64_t s_live;

// (and the whole process, whatever allocates it, the CRT directly or the graphics driver: a
// thread looks at its commit four times a second and ends it past 8 GB)
DWORD WINAPI CommitWatchdog(void*) {
    // (FROST_COMMIT_LIMIT_GB raises it: Windows' full page heap gives every block its own pages,
    // and a healthy run passes 8 GB under it)
    uint64_t limit = 8ull << 30;
    char value[16] = {};
    if (GetEnvironmentVariableA("FROST_COMMIT_LIMIT_GB", value, sizeof(value)) && std::atoi(value) > 0) {
        limit = static_cast<uint64_t>(std::atoi(value)) << 30;
    }
    for (;;) {
        PROCESS_MEMORY_COUNTERS_EX counters = {};
        counters.cb = sizeof(counters);
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                 sizeof(counters))
            && counters.PrivateUsage > limit) {
            FrostLog("memory", "process commit past %llu MB (Storm heap %lld MB live): stopping",
                     static_cast<unsigned long long>(counters.PrivateUsage >> 20), s_live >> 20);
            TerminateProcess(GetCurrentProcess(), 0xE0000003);
        }
        Sleep(250);
    }
}

void CountLive(int64_t delta) {
    static volatile long s_watching;
    if (InterlockedExchange(&s_watching, 1) == 0) {
        CloseHandle(CreateThread(nullptr, 0, CommitWatchdog, nullptr, 0, nullptr));
    }
    int64_t now = InterlockedAdd64(&s_live, delta);
    static volatile long s_stopped;
    if (delta > 0 && now > kLiveCeiling && InterlockedExchange(&s_stopped, 1) == 0) {
        FrostLog("memory", "heap past %lld MB live (this request %lld bytes): stopping", now >> 20,
                 static_cast<long long>(delta));
        FrostLogStack(1, 24);
        TerminateProcess(GetCurrentProcess(), 0xE0000002);
    }
}

void* RawMalloc(uint64_t bytes) {
    if (GuardHeap()) {
        return GuardAlloc(bytes, false);
    }
    void* block = std::malloc(bytes);
    if (block) {
        CountLive(static_cast<int64_t>(_msize(block)));
    }
    return block;
}

void* RawCalloc(uint64_t bytes) {
    if (GuardHeap()) {
        return GuardAlloc(bytes, true);
    }
    void* block = std::calloc(1, bytes);
    if (block) {
        CountLive(static_cast<int64_t>(_msize(block)));
    }
    return block;
}

void RawFree(void* block);

void* RawRealloc(void* block, uint64_t bytes) {
    if (block == nullptr ? GuardHeap() : GuardBlock(block)) {
        return GuardRealloc(block, bytes);
    }
    if (block != nullptr && GuardHeap()) {
        // (a block from before the guard, grown after it: moved to the guard heap, the old one
        // quarantined as a free would be; std::realloc would free it out of sight)
        size_t had = _msize(block);
        void* fresh = GuardAlloc(bytes, false);
        if (fresh != nullptr) {
            std::memcpy(fresh, block, had < bytes ? had : bytes);
            RawFree(block);
        }
        return fresh;
    }
    int64_t old = block ? static_cast<int64_t>(_msize(block)) : 0;
    void* fresh = std::realloc(block, bytes);
    if (fresh) {
        CountLive(static_cast<int64_t>(_msize(fresh)) - old);
    }
    return fresh;
}

// FROST_GUARD_HEAP=world: a block from before the guard started, freed after it, is quarantined
// rather than freed: filled with 0xFD and kept, with the stack that freed it. A write through a
// stale pointer then changes the fill, and QuarantineScan names the block and its freeing stack
// (a use after free of a block from before Enter World, which the guard heap cannot catch, broke
// the logger's own std::set nodes on Enter World, 2026-10-22). Capped at 1 GB.
struct QUARANTINED {
    uint8_t* m_block;
    size_t m_size;
    void* m_frames[16];
    uint16_t m_count;
    bool m_reported;
};
std::mutex s_quarantineLock;
std::vector<QUARANTINED>* s_quarantine;
uint64_t s_quarantineBytes;
thread_local bool t_inQuarantine;

bool Quarantine(void* block) {
    if (GuardMode() != 2 || s_guardStarted == 0 || block == nullptr || t_inQuarantine) {
        return false;
    }
    size_t size = _msize(block);
    std::lock_guard<std::mutex> lock(s_quarantineLock);
    if (s_quarantineBytes + size > (1ull << 30)) {
        return false;
    }
    t_inQuarantine = true;
    if (s_quarantine == nullptr) {
        s_quarantine = new std::vector<QUARANTINED>();
        s_quarantine->reserve(1 << 20);
    }
    QUARANTINED entry = {};
    entry.m_block = static_cast<uint8_t*>(block);
    entry.m_size = size;
    entry.m_count = static_cast<uint16_t>(CaptureStackBackTrace(2, 16, entry.m_frames, nullptr));
    std::memset(block, 0xFD, size);
    s_quarantine->push_back(entry);
    s_quarantineBytes += size;
    t_inQuarantine = false;
    return true;
}

void RawFree(void* block) {
    if (GuardBlock(block)) {
        GuardFree(block);
    } else if (Quarantine(block)) {
        CountLive(-static_cast<int64_t>(_msize(block)));
    } else {
        if (block) {
            CountLive(-static_cast<int64_t>(_msize(block)));
        }
        std::free(block);
    }
}

// 1 = the CRT-backed default allocator is enabled (its static value).
int32_t s_defaultAllocator = 1;  // 0x142cc8a48
// whether running out of memory shows a dialog first (static value 0).
uint8_t s_showOutOfMemoryDialog = 0;  // 0x142cc8a4c

SMEMALLOCHOOK s_allocHook;          // 0x142e1c450
SMEMFREEHOOK s_freeHook;            // 0x142e1c458
void* s_hook3;                      // 0x142e1c460
SMEMOWNERHOOK s_ownerHook;          // 0x142e1c468
SMEMREALLOCHOOK s_reallocHook;      // 0x142e1c470 hook5
void* s_hook6;                      // 0x142e1c478
void* s_hook7;                      // 0x142e1c480
void* s_hook8;                      // 0x142e1c488

}

// Reports that `bytes` could not be allocated at tag/line (optionally through the
// out-of-memory dialog first), under the memory system's lock.
// ref: FUN_140254890
void SMemReportOutOfMemory(const char* tag, int32_t line, uint64_t bytes) {
    (void)tag;
    (void)line;
    (void)bytes;
    (void)s_showOutOfMemoryDialog;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140255140
void* SMemAlloc(uint64_t bytes, int32_t a2, const char* tag, uint32_t tagHash, int32_t line, uint32_t flags) {
    (void)tagHash;  // not passed on to the hook, nor used here

    if (s_allocHook && !(flags & SMEM_FLAG_NOHOOKS)) {
        void* block = s_allocHook(bytes, a2, tag, line, (flags & SMEM_FLAG_ZEROMEMORY) != 0);
        if (block) {
            return block;
        }
    }

    uint64_t size = (bytes + 0xF) & ~uint64_t(0xF);

    if (s_defaultAllocator == 1) {
        // a 16-byte header, size | 1 then 0, in front of a 16-byte aligned payload; any
        // gap between them is zero, which is how SMemFree walks back to the header
        uint64_t* header = static_cast<uint64_t*>(
            (flags & SMEM_FLAG_ZEROMEMORY) ? RawCalloc(size + 0x10) : RawMalloc(size + 0x10));
        if (header) {
            header[0] = size | 1;
            header[1] = 0;
            uint64_t* payload = header + 2;
            while (reinterpret_cast<uintptr_t>(payload) & 0xF) {
                *payload++ = 0;
            }
            return payload;
        }
    }

    SMemReportOutOfMemory(tag, line, size);
    return nullptr;
}

// A development aid, not the reference's: blocks a caller asked to have watched (FROST_NODELOG)
// log the stack that frees them, so a block freed behind its owner's back names the freer.
namespace {
std::mutex s_watchLock;
std::unordered_set<void*>* s_watched;
// (set while the watch touches its own set: the set's nodes are freed through these same delete
// operators, and locking s_watchLock again from inside threw resource_deadlock_would_occur)
thread_local bool t_inWatch;
}

bool MemoryWatchEnabled() {
    static const bool s_on = [] {
        char value[8] = {};
        return GetEnvironmentVariableA("FROST_NODELOG", value, sizeof(value)) && value[0] == '1';
    }();
    return s_on;
}

void MemoryWatchBlock(void* block, bool watch) {
    if (!MemoryWatchEnabled() || block == nullptr || t_inWatch) {
        return;
    }
    std::lock_guard<std::mutex> lock(s_watchLock);
    t_inWatch = true;
    if (s_watched == nullptr) {
        s_watched = new std::unordered_set<void*>();
    }
    if (watch) {
        s_watched->insert(block);
    } else {
        s_watched->erase(block);
    }
    t_inWatch = false;
}

// Every path that frees a Storm block (SMemFree, the delete operators, a realloc that moves)
// tells the watch: the child node freed behind its frame's back in the first watched runs went
// through none of the checked ones.
void MemoryWatchFreed(void* ptr, const char* tag) {
    if (!MemoryWatchEnabled() || ptr == nullptr || t_inWatch) {
        return;
    }
    bool watched = false;
    {
        std::lock_guard<std::mutex> lock(s_watchLock);
        t_inWatch = true;
        watched = s_watched != nullptr && s_watched->erase(ptr) != 0;
        t_inWatch = false;
    }
    if (watched) {
        FrostLog("nodelog", "watched block %p freed outside its owner (tag %s)", ptr, tag ? tag : "?");
        FrostLogStack(0, 24);
    }
}

// FROST_HEAP_VALIDATE=1: the process heap checked at each call (after each packet handled, once
// a frame). The first failure names the checkpoint and the last one that passed, so a corruption
// that only shows at a later free is pinned to the packet or frame that did it.
void MemoryHeapCheck(const char* where, uint64_t a, uint64_t b, uint64_t c) {
    // (=N: every Nth checkpoint, a validation costing about a second on a 3 GB heap)
    static const long s_every = [] {
        char value[16] = {};
        return GetEnvironmentVariableA("FROST_HEAP_VALIDATE", value, sizeof(value)) ? std::atol(value) : 0L;
    }();
    static volatile long s_failed = 0;
    static volatile long s_calls = 0;
    static const char* s_lastWhere = "start";
    static uint64_t s_last[3];
    static long s_lastCall = 0;
    if (s_every <= 0 || s_failed) {
        return;
    }
    long call = InterlockedIncrement(&s_calls);
    if (call % s_every != 0) {
        return;
    }
    if (HeapValidate(GetProcessHeap(), 0, nullptr)) {
        s_lastWhere = where;
        s_last[0] = a;
        s_last[1] = b;
        s_last[2] = c;
        s_lastCall = call;
        return;
    }
    if (InterlockedExchange(&s_failed, 1) == 0) {
        FrostLog("memory", "process heap corrupt at checkpoint %ld %s (%llx %llx %llx); last sound: %ld %s (%llx %llx %llx)", call, where,
                 static_cast<unsigned long long>(a), static_cast<unsigned long long>(b),
                 static_cast<unsigned long long>(c), s_lastCall, s_lastWhere, static_cast<unsigned long long>(s_last[0]),
                 static_cast<unsigned long long>(s_last[1]), static_cast<unsigned long long>(s_last[2]));
        FrostLogStack(1, 16);
    }
}

// FROST_GUARD_HEAP=world: the quarantined blocks checked for writes since their free (every 30th
// call; the main loop calls it once a frame). Each changed block is reported once, with the
// offset and value written and the stack that freed it.
void MemoryQuarantineScan() {
    static long s_calls = 0;
    if (GuardMode() != 2 || s_guardStarted == 0 || ++s_calls % 30 != 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(s_quarantineLock);
    if (s_quarantine == nullptr) {
        return;
    }
    int reported = 0;
    for (QUARANTINED& entry : *s_quarantine) {
        if (entry.m_reported) {
            continue;
        }
        for (size_t i = 0; i < entry.m_size; i++) {
            if (entry.m_block[i] != 0xFD) {
                entry.m_reported = true;
                uint64_t value = 0;
                std::memcpy(&value, entry.m_block + (i & ~size_t(7)), entry.m_size - (i & ~size_t(7)) >= 8 ? 8 : 1);
                FrostLog("quarantine", "freed block %p (%zu bytes) written at +0x%zx (qword %016llx) after its free; freed at:",
                         static_cast<void*>(entry.m_block), entry.m_size, i, static_cast<unsigned long long>(value));
                FrostLogFrames(entry.m_frames, entry.m_count);
                reported++;
                break;
            }
        }
        if (reported >= 8) {
            break;
        }
    }
}

// FROST_GUARD_HEAP=world: guard what is allocated from here on (EnterWorld calls it).
void MemoryGuardStart() {
    if (GuardMode() == 2 && InterlockedExchange(&s_guardStarted, 1) == 0) {
        FrostLog("guardheap", "guarding the blocks allocated from Enter World on");
    }
}

// Under FROST_GUARD_HEAP: when `address` lies in a block already freed, the stack that freed it
// (the fault's own stack names only the use; this names the free).
void MemoryDescribeFreed(uint64_t address) {
    if (!GuardHeap() || address < 0x10000) {
        return;
    }
    MEMORY_BASIC_INFORMATION info;
    if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == 0 || info.State == MEM_FREE) {
        return;
    }
    GUARDFREE entry = {};
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(s_guardFreesLock);
        if (s_guardFrees != nullptr) {
            auto it = s_guardFrees->find(static_cast<uint8_t*>(info.AllocationBase));
            if (it != s_guardFrees->end()) {
                entry = it->second;
                found = true;
            }
        }
    }
    if (found) {
        FrostLog("guardheap", "%llx is in a block freed at:", static_cast<unsigned long long>(address));
        FrostLogFrames(entry.m_frames, entry.m_count);
    }
}

// ref: FUN_140255210
bool SMemFree(void* ptr, const char* tag, int32_t line, uint32_t flags) {
    if (!ptr) {
        return true;
    }
    MemoryWatchFreed(ptr, tag);

    if (s_freeHook && !(flags & SMEM_FLAG_NOHOOKS)) {
        int32_t out = 0;
        if (s_ownerHook(ptr, &out) && s_freeHook(ptr)) {
            return true;
        }
    }

    uint64_t* header = static_cast<uint64_t*>(ptr) - 1;
    uint64_t value = *header;
    if (value == 0) {
        do {
            value = *(header - 1);
            header--;
        } while (value == 0);
        value = static_cast<uint32_t>(value);
    }

    if ((value & 3) != 1) {
        ErrorDisplayFormatted(0x8510007B, nullptr, 0, "Memory corrupted -- %s %d", tag ? tag : "unknown", line);
        return true;
    }

    RawFree(header);
    return true;
}

// Aligned allocation. Up to 16 bytes of alignment is what SMemAlloc already gives; beyond
// that the block is over-allocated by `alignment` (bypassing the hooks) and zero-filled one
// qword at a time up to the first aligned address, which is returned. The zero gap is what
// lets SMemFree walk back to the header.
// ref: FUN_140254a00
void* SMemAllocAligned(uint64_t bytes, uint64_t alignment, int32_t a2, const char* tag, uint32_t tagHash, int32_t line,
    uint32_t flags) {
    if (alignment <= 0x10) {
        return SMemAlloc(bytes, a2, tag, tagHash, line, flags);
    }
    auto block = static_cast<uint64_t*>(SMemAlloc(bytes + alignment, a2, tag, tagHash, line, flags | SMEM_FLAG_NOHOOKS));
    do {
        *block++ = 0;
    } while (reinterpret_cast<uintptr_t>(block) & (alignment - 1));
    return block;
}

namespace {

// The global allocation operators' shared body: SMemAlloc's default path without flags (no
// zeroing, hooks always consulted). The hook gets tag/line, the report gets the same.
void* StormNew(uint64_t bytes, int32_t a2, const char* tag, int32_t line) {
    if (s_allocHook) {
        if (void* block = s_allocHook(bytes, a2, tag, line, false)) {
            return block;
        }
    }

    uint64_t size = (bytes + 0xF) & ~uint64_t(0xF);
    if (s_defaultAllocator == 1) {
        if (uint64_t* header = static_cast<uint64_t*>(RawMalloc(size + 0x10))) {
            header[0] = size | 1;
            header[1] = 0;
            uint64_t* payload = header + 2;
            while (reinterpret_cast<uintptr_t>(payload) & 0xF) {
                *payload++ = 0;
            }
            return payload;
        }
    }

    SMemReportOutOfMemory(tag, line, size);
    return nullptr;
}

// The global deallocation operators' shared body: SMemFree with the operator's name as the
// tag and line -1.
void StormDelete(void* ptr, const char* name) {
    if (!ptr) {
        return;
    }
    MemoryWatchFreed(ptr, name);

    if (s_freeHook) {
        int32_t out = 0;
        if (s_ownerHook(ptr, &out) && s_freeHook(ptr)) {
            return;
        }
    }

    uint64_t* header = static_cast<uint64_t*>(ptr) - 1;
    uint64_t value = *header;
    if (value == 0) {
        do {
            value = *(header - 1);
            header--;
        } while (value == 0);
        value = static_cast<uint32_t>(value);
    }

    if ((value & 3) != 1) {
        ErrorDisplayFormatted(0x8510007B, nullptr, 0, "Memory corrupted -- %s %d", name, -1);
        return;
    }

    RawFree(header);
}

}

// Storm replaces the global allocation operators, so every `new` in the client lands in the
// same headered blocks SMemAlloc makes, and SMemFree can release them.

// ref: FUN_140254520
void* operator new(size_t bytes) {
    return StormNew(bytes, 0, nullptr, 0);
}

// The tagged form generated code uses for typed allocations (3.8k call sites): tag is the
// type name, as for SMemAlloc. tagHash is accepted and dropped, as SMemAlloc drops it.
// ref: FUN_1402545c0
void* operator new(size_t bytes, int32_t a2, const char* tag, uint32_t tagHash, int32_t line) {
    (void)tagHash;
    return StormNew(bytes, a2, tag, line);
}

// ref: FUN_140254720
void* operator new[](size_t bytes) {
    return StormNew(bytes, 0, "new[]", -1);
}

// The sized form is a thunk to the same body (0x14026bd4c, 4.2k call sites).
// ref: FUN_140254670
void operator delete(void* ptr) noexcept {
    StormDelete(ptr, "delete");
}

// The reference's sized form is a jump to the unsized one (0x14026bd4c).
void operator delete(void* ptr, size_t) noexcept {
    ::operator delete(ptr);
}

// Matches the tagged new, for a constructor that throws.
void operator delete(void* ptr, int32_t, const char*, uint32_t, int32_t) noexcept {
    StormDelete(ptr, "delete");
}

// The array form of the tagged new is the same body (FUN_1402545c0): the compiler puts the
// element count in front of arrays of destructible types itself.
void* operator new[](size_t bytes, int32_t a2, const char* tag, uint32_t tagHash, int32_t line) {
    (void)tagHash;
    return StormNew(bytes, a2, tag, line);
}

void operator delete[](void* ptr, int32_t, const char*, uint32_t, int32_t) noexcept {
    StormDelete(ptr, "delete[]");
}

// ref: FUN_1402547e0
void operator delete[](void* ptr) noexcept {
    StormDelete(ptr, "delete[]");
}

void operator delete[](void* ptr, size_t) noexcept {
    StormDelete(ptr, "delete[]");
}

// ref: FUN_1402550f0
void SMemInstallHooks(SMEMALLOCHOOK alloc, SMEMFREEHOOK free, void* hook3, SMEMOWNERHOOK owner,
    void* hook5, void* hook6, void* hook7, void* hook8) {
    s_hook6 = hook6;
    s_hook7 = hook7;
    s_hook8 = hook8;
    s_reallocHook = reinterpret_cast<SMEMREALLOCHOOK>(hook5);
    s_hook3 = hook3;
    s_allocHook = alloc;
    s_freeHook = free;
    s_ownerHook = owner;
}

// _onexit-style registration of 0x14025b770, then atexit(0x14025b790).
// ref: FUN_14025b7b0
void MemoryRegisterExitHandlers() {
    FROST_UNIMPLEMENTED();
}

// Initializes the heaps at DAT_1430a84a0 and DAT_1430a8c70 and passes eight hooks to
// SMemInstallHooks.
// ref: FUN_1412c46b0
void MemoryInstallAllocatorHooks() {
    FROST_UNIMPLEMENTED();
}

namespace {

// Walks back over the zero gap to a block's header word.
uint64_t* SMemHeader(void* ptr) {
    uint64_t* header = static_cast<uint64_t*>(ptr) - 1;
    while (*header == 0) {
        header--;
    }
    return header;
}

// Resizes a headered block to `size` (already 16-aligned) bytes: in place through the CRT
// for a default-allocator block, else by allocating, copying and freeing the old one.
// ref: FUN_140254f70
void* SMemReAllocBlock(int32_t a2, const char* tag, uint32_t tagHash, int32_t line, void* ptr, uint64_t size) {
    (void)a2;
    (void)tagHash;
    uint64_t* header = SMemHeader(ptr);
    uint64_t value = *header;
    MemoryWatchFreed(ptr, tag);
    if (static_cast<uint32_t>(value & 3) == static_cast<uint32_t>(s_defaultAllocator) && s_defaultAllocator == 1) {
        auto block = static_cast<uint64_t*>(RawRealloc(header, size + 0x10));
        if (block) {
            *block = size | 1;
            return block + (static_cast<uint64_t*>(ptr) - header);
        }
        return nullptr;
    }

    uint64_t* payload = nullptr;
    if (s_defaultAllocator == 1) {
        if (uint64_t* block = static_cast<uint64_t*>(RawMalloc(size + 0x10))) {
            block[0] = size | 1;
            block[1] = 0;
            payload = block + 2;
            while (reinterpret_cast<uintptr_t>(payload) & 0xF) {
                *payload++ = 0;
            }
        }
    }
    if (!payload) {
        return nullptr;
    }
    uint64_t old = value & ~uint64_t(0xF);
    std::memcpy(payload, ptr, old < size ? old : size);

    uint64_t* oldHeader = static_cast<uint64_t*>(ptr) - 1;
    uint64_t oldValue = *oldHeader;
    if (oldValue == 0) {
        do {
            oldHeader--;
        } while (*oldHeader == 0);
        oldValue = static_cast<uint32_t>(*oldHeader);
    }
    if ((oldValue & 3) != 1) {
        ErrorDisplayFormatted(0x8510007B, nullptr, 0, "Memory corrupted -- %s %d", tag ? tag : "unknown", line);
        return payload;
    }
    RawFree(oldHeader);
    return payload;
}

}

// The size a block was allocated with (rounded to 16), from the owner hook or the header.
// ref: FUN_1402552f0
uint64_t SMemSize(void* ptr) {
    if (s_ownerHook) {
        int32_t out = 0;
        if (uint64_t size = reinterpret_cast<uint64_t>(s_ownerHook(ptr, &out))) {
            return size;
        }
    }
    return *SMemHeader(ptr) & ~uint64_t(0xF);
}

// Resizes a block: SMemAlloc for null, SMemFree for size 0, else the block grows or shrinks
// (new space zeroed with SMEM_FLAG_ZEROMEMORY). The realloc hook goes first.
// ref: FUN_140255350
void* SMemReAlloc(void* ptr, uint64_t bytes, int32_t a2, const char* tag, uint32_t tagHash, int32_t line, uint32_t flags) {
    if (s_reallocHook) {
        if (void* block = s_reallocHook(ptr, bytes, a2, tag, line, (flags & SMEM_FLAG_ZEROMEMORY) != 0)) {
            if (ptr && bytes == 0) {
                return nullptr;
            }
            return block;
        }
    }
    if (!ptr) {
        return SMemAlloc(bytes, a2, tag, tagHash, line, flags);
    }
    if (bytes == 0) {
        SMemFree(ptr, tag, line, flags);
        return nullptr;
    }
    uint64_t size = (bytes + 0xF) & ~uint64_t(0xF);
    uint64_t old = *SMemHeader(ptr) & ~uint64_t(0xF);
    if (old == size) {
        return ptr;
    }
    void* block = nullptr;
    if (size < 0xFFFFFFFC) {
        block = SMemReAllocBlock(a2, tag, tagHash, line, ptr, size);
    }
    if (!block) {
        SMemReportOutOfMemory(tag, line, size);
        return nullptr;
    }
    if (old < size && (flags & SMEM_FLAG_ZEROMEMORY)) {
        std::memset(static_cast<uint8_t*>(block) + old, 0, size - old);
    }
    return block;
}

// SMemAllocAligned that also reports the size asked for.
// ref: FUN_140254a90
void* SMemAllocAlignedSize(uint64_t* allocated, uint64_t bytes, uint64_t alignment, int32_t a2, const char* tag,
    uint32_t tagHash, int32_t line, uint32_t flags) {
    *allocated = bytes;
    return SMemAllocAligned(bytes, alignment, a2, tag, tagHash, line, flags);
}

// Aligned resize: SMemReAlloc when the alignment is at most 16 and the result is aligned,
// else a fresh aligned block with the old contents copied in.
// ref: FUN_140254be0
void* SMemReAllocAligned(void* ptr, uint64_t bytes, uint64_t alignment, int32_t a2, const char* tag, uint32_t tagHash,
    int32_t line, uint32_t flags) {
    if (!ptr) {
        return SMemAllocAligned(bytes, alignment, a2, tag, tagHash, line, flags);
    }
    if (bytes == 0) {
        SMemFree(ptr, tag, line, flags);
        return nullptr;
    }
    if (alignment <= 0x10 && s_defaultAllocator == 1) {
        void* block = SMemReAlloc(ptr, bytes, a2, tag, tagHash, line, flags);
        if (block) {
            ptr = block;
            if ((reinterpret_cast<uintptr_t>(block) & (alignment - 1)) == 0) {
                return block;
            }
        }
    }
    void* block = SMemAllocAligned(bytes, alignment, a2, tag, tagHash, line, flags);
    if (block) {
        uint64_t old = *SMemHeader(ptr) & ~uint64_t(0xF);
        std::memcpy(block, ptr, old < bytes ? old : bytes);
    }
    SMemFree(ptr, tag, line, flags);
    return block;
}

// SMemReAllocAligned that also reports the size asked for.
// ref: FUN_140254d80
void* SMemReAllocAlignedSize(void* ptr, uint64_t* allocated, uint64_t bytes, uint64_t alignment, int32_t a2,
    const char* tag, uint32_t tagHash, int32_t line, uint32_t flags) {
    *allocated = bytes;
    return SMemReAllocAligned(ptr, bytes, alignment, a2, tag, tagHash, line, flags);
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142cc8a48 = (void*)&s_defaultAllocator;
extern "C" void* const gen_p_142e1c450 = (void*)&s_allocHook;
extern "C" void* const gen_p_142e1c458 = (void*)&s_freeHook;
extern "C" void* const gen_p_142e1c460 = (void*)&s_hook3;
extern "C" void* const gen_p_142e1c468 = (void*)&s_ownerHook;
extern "C" void* const gen_p_142e1c470 = (void*)&s_reallocHook;
extern "C" void* const gen_p_142e1c478 = (void*)&s_hook6;
extern "C" void* const gen_p_142e1c480 = (void*)&s_hook7;
extern "C" void* const gen_p_142e1c488 = (void*)&s_hook8;
// ---- end of generated globals ----
