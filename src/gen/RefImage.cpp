#include "gen/RefImage.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <vector>
#include <unordered_set>

#include <windows.h>
#include <dbghelp.h>

#include "storm/thread/SThread.hpp"
#include "storm/Memory.hpp"
#include "util/FrostLog.hpp"

// The reference's own address space, for the translated functions (src/gen/translated_*.c).
//
// Their globals are the reference's: its data sections are mapped at their original addresses
// from the clean executable, so a global is its address and a pointer stored in the data stays
// valid. Its code is never mapped: the reference .text range is filled with int3, and at the
// start of every function that has a translation (or a port, through an adapter) goes a jump to
// it, written here. A call through a stored pointer to anything else lands on an int3, which the
// handler below turns into "log the address once and return 0".

extern "C" {
struct GENJUMP {
    uint64_t m_address;
    void* m_target;
};
extern const GENJUMP g_genJumps[];
extern const size_t g_genJumpCount;
extern const uint64_t g_genInits[];
extern const size_t g_genInitCount;
// the image's globals the port holds: their reference range and the port's object (or, for an
// internal one, the pointer its file exports to it)
struct GENHELD {
    uint64_t m_ref;
    uint64_t m_size;
    void* m_port;
    void* const* m_portPtr;
};
extern const GENHELD g_genHeld[];
extern const size_t g_genHeldCount;
// the code ranges mapped as the reference's own bytes, from and to in pairs (third-party
// assembly the translated code calls: tools/translate.py native_ok)
extern const uint64_t g_genNative[];
extern const size_t g_genNativeCount;
// the import slots that native code calls through (the protector resolves the image's own at
// run time, so the mapped ones are empty): filled here, by name
struct GENIMPORT {
    uint64_t m_slot;
    const char* m_dll;
    const char* m_name;
};
extern const GENIMPORT g_genImports[];
extern const size_t g_genImportCount;
// each translated file's call counts and the addresses they belong to (hits.c)
struct GENHITS {
    unsigned int* m_hits;
    const unsigned long long* m_addrs;
    unsigned int m_count;
};
extern const GENHITS gen_hit_tables[];
extern const unsigned int gen_hit_table_count;

void GenUnimplemented(uint64_t address);
int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);
}

// Ghidra.h's GEN_OPAQUE: a zero the optimizer cannot see through, so a reference address used as
// an indexed base is a value, not a displacement MSVC may truncate to 32 bits.
extern "C" volatile unsigned long long gen_opaque_zero = 0;

// The runtime's thread-safe-statics epoch, for translated function-local statics (Ghidra.h).
extern "C" __declspec(thread) int _Init_thread_epoch;
extern "C" int GenInitThreadEpoch() {
    return _Init_thread_epoch;
}

// Its address, for code that holds a pointer to the epoch (bgs_low's Initialize, FUN_141dea240).
extern "C" int* GenInitThreadEpochPtr() {
    return &_Init_thread_epoch;
}

namespace {

// One static initializer, at its reference address (the jump to its translation).
// Which translated functions a run reaches and how often (hits.c): with FROST_HITS naming a
// file, every five seconds the functions called so far, "address count" a line, most first.
DWORD WINAPI HitDumpThread(void* param) {
    auto path = static_cast<const char*>(param);
    std::vector<std::pair<uint32_t, uint64_t>> rows;
    for (;;) {
        Sleep(5000);
        rows.clear();
        for (uint32_t t = 0; t < gen_hit_table_count; t++) {
            const GENHITS& table = gen_hit_tables[t];
            for (uint32_t i = 0; i < table.m_count; i++) {
                if (table.m_hits[i] != 0) {
                    rows.emplace_back(table.m_hits[i], table.m_addrs[i]);
                }
            }
        }
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        FILE* file = nullptr;
        if (fopen_s(&file, path, "w") == 0 && file != nullptr) {
            for (const auto& row : rows) {
                fprintf(file, "%llx %u\n", static_cast<unsigned long long>(row.second), row.first);
            }
            fclose(file);
        }
    }
}

void StartHitDump() {
    static char path[MAX_PATH];
    DWORD length = GetEnvironmentVariableA("FROST_HITS", path, sizeof(path));
    if (length == 0 || length >= sizeof(path)) {
        return;
    }
    CloseHandle(CreateThread(nullptr, 0, HitDumpThread, path, 0, nullptr));
}

// A write watch on one 8-byte slot of the image (FROST_WATCH=<hex address>), for a value that
// arrives through a pointer no static search follows: the main thread's first debug register,
// set from inside a handler (a running thread cannot set its own), and each write logged with
// the new value and the writer's stack. The first 16 writes are logged.
constexpr DWORD kArmWatch = 0xE0F05701;
constexpr DWORD kCheckWatch = 0xE0F05702;
uint64_t s_watch = 0;
LONG s_watchHits = 0;

LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* info) {
    DWORD code = info->ExceptionRecord->ExceptionCode;
    CONTEXT* context = info->ContextRecord;
    if (code == kArmWatch) {
        context->Dr0 = s_watch;
        // local enable 0, break on write (RW0 = 01), 8 bytes (LEN0 = 10)
        context->Dr7 = (context->Dr7 & ~0xF0003ull) | 1ull | (1ull << 16) | (2ull << 18);
        // (without this flag the continue leaves the debug registers as they were: the first
        // watched run, 2026-10-17, armed nothing and saw no write)
        context->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == kCheckWatch) {
        // what the thread's debug registers hold now, as the kernel sees them
        FrostLog("watch", "thread %lu: dr0 %llx dr7 %llx", GetCurrentThreadId(),
                 static_cast<unsigned long long>(context->Dr0), static_cast<unsigned long long>(context->Dr7));
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && (context->Dr6 & 1)) {
        context->Dr6 &= ~0xFull;
        // (a write by ntdll itself, a critical section's or an SRW lock's own bookkeeping on a
        // watched lock, is left out: the residency mutex's 16 were all EnterCriticalSection, and
        // the store that broke it came later, 2026-10-21)
        HMODULE writer = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(context->Rip), &writer);
        if (writer != nullptr && writer == GetModuleHandleW(L"ntdll.dll")) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (InterlockedIncrement(&s_watchHits) <= 16) {
            FrostLog("watch", "%llx written: now %llx", static_cast<unsigned long long>(s_watch),
                     static_cast<unsigned long long>(*reinterpret_cast<volatile uint64_t*>(s_watch)));
            FrostLogContextStack(context, 12);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// An A/B switch for a suspect translation (FROST_NATIVE=<hex address>[,<hex address>...]): each
// function named runs as the reference's own machine code instead. Its bytes (every .pdata piece
// whose chain leads back to it) are mapped from the image over the int3 fill and the entry jump,
// with their unwind entries, and its translation's own entry (FUN_<address>, found in Frost.pdb)
// jumps there, so a translated caller and a call through the image both reach it. What it calls
// still goes through the image's entries, to the translations. A difference between the two runs
// is the translation's; the character-select skin stripes were bisected this way (2026-10-22).
std::vector<RUNTIME_FUNCTION> s_nativeOverrides;

uint32_t ChainRoot(uint64_t base, const RUNTIME_FUNCTION* function) {
    // (an unwind entry chained to its function's, UNW_FLAG_CHAININFO: the parent entry follows
    // the unwind codes, which are padded to an even count)
    for (int depth = 0; depth < 32; depth++) {
        auto info = reinterpret_cast<const uint8_t*>(base + function->UnwindData);
        if ((info[0] >> 3 & 4) == 0) {
            return function->BeginAddress;
        }
        uint32_t codes = (info[2] + 1u) & ~1u;
        function = reinterpret_cast<const RUNTIME_FUNCTION*>(info + 4 + codes * 2);
    }
    return function->BeginAddress;
}

void ApplyNativeOverrides(const char* path, uint64_t base, const IMAGE_NT_HEADERS64& nt) {
    char value[1024];
    DWORD length = GetEnvironmentVariableA("FROST_NATIVE", value, sizeof(value));
    if (length == 0 || length >= sizeof(value)) {
        return;
    }
    using SymInitializeFn = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
    using SymFromNameFn = BOOL(WINAPI*)(HANDLE, PCSTR, PSYMBOL_INFO);
    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    auto symInitialize = dbghelp ? reinterpret_cast<SymInitializeFn>(GetProcAddress(dbghelp, "SymInitialize")) : nullptr;
    auto symFromName = dbghelp ? reinterpret_cast<SymFromNameFn>(GetProcAddress(dbghelp, "SymFromName")) : nullptr;
    if (symInitialize == nullptr || symFromName == nullptr || !symInitialize(GetCurrentProcess(), nullptr, TRUE)) {
        FrostLog("native", "dbghelp unavailable: FROST_NATIVE ignored");
        return;
    }
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return;
    }
    auto sections = IMAGE_FIRST_SECTION(reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew));
    auto fileOffset = [&](uint32_t rva) -> long {
        for (uint32_t i = 0; i < nt.FileHeader.NumberOfSections; i++) {
            if (rva >= sections[i].VirtualAddress && rva < sections[i].VirtualAddress + sections[i].SizeOfRawData) {
                return static_cast<long>(sections[i].PointerToRawData + (rva - sections[i].VirtualAddress));
            }
        }
        return -1;
    };
    const IMAGE_DATA_DIRECTORY& pdata = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    auto functions = reinterpret_cast<const RUNTIME_FUNCTION*>(base + pdata.VirtualAddress);
    size_t total = pdata.VirtualAddress != 0 ? pdata.Size / sizeof(RUNTIME_FUNCTION) : 0;
    for (char* next = nullptr, *token = strtok_s(value, ",; ", &next); token != nullptr;
         token = strtok_s(nullptr, ",; ", &next)) {
        uint64_t address = std::strtoull(token, nullptr, 16);
        if (address < base || address >= base + nt.OptionalHeader.SizeOfImage) {
            continue;
        }
        auto rva = static_cast<uint32_t>(address - base);
        size_t pieces = 0;
        for (size_t i = 0; i < total; i++) {
            if (ChainRoot(base, &functions[i]) != rva) {
                continue;
            }
            long at = fileOffset(functions[i].BeginAddress);
            size_t size = functions[i].EndAddress - functions[i].BeginAddress;
            if (at < 0 || std::fseek(file, at, SEEK_SET) != 0
                || std::fread(reinterpret_cast<void*>(base + functions[i].BeginAddress), 1, size, file) != size) {
                continue;
            }
            s_nativeOverrides.push_back(functions[i]);
            pieces++;
        }
        if (pieces == 0) {
            FrostLog("native", "%llx: no unwind entry, not overridden", static_cast<unsigned long long>(address));
            continue;
        }
        char name[32];
        std::snprintf(name, sizeof(name), "FUN_%llx", static_cast<unsigned long long>(address));
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 64] = {};
        auto symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 64;
        if (!symFromName(GetCurrentProcess(), name, symbol)) {
            FrostLog("native", "%s mapped, but its translation is not in the symbols: only image calls reach it", name);
            continue;
        }
        // jmp [rip+0]; dq address
        uint8_t patch[14] = {0xff, 0x25, 0, 0, 0, 0};
        std::memcpy(patch + 6, &address, 8);
        auto entry = reinterpret_cast<void*>(symbol->Address);
        DWORD old;
        VirtualProtect(entry, sizeof(patch), PAGE_EXECUTE_READWRITE, &old);
        std::memcpy(entry, patch, sizeof(patch));
        VirtualProtect(entry, sizeof(patch), old, &old);
        FlushInstructionCache(GetCurrentProcess(), entry, sizeof(patch));
        FrostLog("native", "%s runs as the reference's own code (%zu pieces)", name, pieces);
    }
    std::fclose(file);
    if (!s_nativeOverrides.empty()) {
        std::sort(s_nativeOverrides.begin(), s_nativeOverrides.end(),
                  [](const RUNTIME_FUNCTION& a, const RUNTIME_FUNCTION& b) { return a.BeginAddress < b.BeginAddress; });
        RtlAddFunctionTable(s_nativeOverrides.data(), static_cast<DWORD>(s_nativeOverrides.size()), base);
    }
}

void StartWatch() {
    char value[32];
    DWORD length = GetEnvironmentVariableA("FROST_WATCH", value, sizeof(value));
    if (length == 0 || length >= sizeof(value)) {
        return;
    }
    s_watch = std::strtoull(value, nullptr, 16) & ~7ull;
    if (s_watch == 0) {
        return;
    }
    AddVectoredExceptionHandler(1, WatchHandler);
    FrostLog("watch", "watching writes to %llx", static_cast<unsigned long long>(s_watch));
    g_sthreadStartHook = RefImageArmWatch;
    RefImageArmWatch();
}

}

// Arms the write watch (FROST_WATCH) on the calling thread; each thread has its own debug
// registers, so the port's thread entry calls this too. Nothing without a watch set.
void RefImageArmWatch() {
    if (s_watch == 0) {
        return;
    }
    RaiseException(kArmWatch, 0, 0, nullptr);
    RaiseException(kCheckWatch, 0, 0, nullptr);
}

namespace {

bool RunInitializer(uint64_t address) {
    __try {
        reinterpret_cast<void (*)()>(address)();
        return true;
    } __except (GenFault(address, GetExceptionInformation())) {
        return false;
    }
}

}

namespace {

constexpr uint64_t kTextStart = 0x140001000ull;
constexpr uint64_t kTextEnd = 0x14260c5d3ull;
bool s_mapped = false;
uint64_t s_pool = 0;
// the functions reached through the int3 rather than a jump, by address (sorted)
std::vector<std::pair<uint64_t, void*>> s_traps;

std::mutex s_reportedLock;
std::unordered_set<uint64_t>* s_reported;

// Logs a reference function translated code reached, once; true the first time.
bool ReportUnimplemented(uint64_t address) {
    std::lock_guard<std::mutex> guard(s_reportedLock);
    if (s_reported == nullptr) {
        s_reported = new std::unordered_set<uint64_t>();
    }
    if (!s_reported->insert(address).second) {
        return false;
    }
    FrostLog("unimplemented", "reference FUN_%llx (translated code reached it)", static_cast<unsigned long long>(address));
    return true;
}

LONG CALLBACK TrapHandler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* context = info->ContextRecord;
    uint64_t address = context->Rip;
    if (address < kTextStart || address >= kTextEnd) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // a function too close to the next for a jump (RefImageInitialize): on to its target, as
    // the jump would have gone, the return address still on top
    auto trap = std::lower_bound(s_traps.begin(), s_traps.end(), std::make_pair(address, static_cast<void*>(nullptr)));
    if (trap != s_traps.end() && trap->first == address) {
        context->Rip = reinterpret_cast<uint64_t>(trap->second);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (ReportUnimplemented(address)) {
        // (who called it: the int3 is the callee's first byte, its return address on top)
        FrostLogContextStack(context, 8);
    }
    // return 0 to the caller, as an empty function would
    context->Rax = 0;
    context->Rip = *reinterpret_cast<uint64_t*>(context->Rsp);
    context->Rsp += 8;
    return EXCEPTION_CONTINUE_EXECUTION;
}

}

extern "C" void GenUnimplemented(uint64_t address) {
    ReportUnimplemented(address);
}

// The exception filter around a translated Lua handler (src/gen/handlers.cpp): a hardware fault
// is logged once with where it happened and the call returns nothing; anything else (a Lua error
// unwinding as a C++ exception or a longjmp) goes on its way.
// A line in the log from translated code (a diagnostic put into src/gen by hand, which the next
// translation replaces): `where` and a number, once per distinct pair.
extern "C" void GenDebugLog(const char* where, int point) {
    static std::mutex s_lock;
    static std::unordered_set<uint64_t>* s_seen;
    std::lock_guard<std::mutex> lock(s_lock);
    if (s_seen == nullptr) {
        s_seen = new std::unordered_set<uint64_t>();
    }
    if (s_seen->insert(reinterpret_cast<uint64_t>(where) * 131 + static_cast<uint64_t>(point)).second) {
        FrostLog("gendebug", "%s: %d", where, point);
    }
}

// Temporary instrumentation for src/gen: `count` bytes at `at`, 16 a line, the first few calls only.
extern "C" void GenDebugDump(const char* where, const void* at, int count) {
    static volatile long s_dumps = 0;
    if (_InterlockedIncrement(&s_dumps) > 16 || at == nullptr) {
        return;
    }
    FrostLog("gendebug", "%s: %d bytes at %p", where, count, at);
    const unsigned char* p = static_cast<const unsigned char*>(at);
    for (int i = 0; i < count; i += 16) {
        char line[80];
        int n = 0;
        for (int k = 0; k < 16 && i + k < count; k++) {
            n += snprintf(line + n, sizeof(line) - n, "%02x ", p[i + k]);
        }
        FrostLog("gendebug", "  +%04x  %s", i, line);
    }
}

// Temporary instrumentation for src/gen: a CDataStore's read position and size (+0x1c, +0x18).
extern "C" void GenDebugPos(const char* where, const void* store) {
    static volatile long s_count = 0;
    if (store == nullptr || _InterlockedIncrement(&s_count) > 5000) {
        return;
    }
    const unsigned char* s = static_cast<const unsigned char*>(store);
    FrostLog("gendebug", "%s: read %u of %u", where, *reinterpret_cast<const unsigned*>(s + 0x1c),
             *reinterpret_cast<const unsigned*>(s + 0x18));
}

extern "C" int GenFault(unsigned long long address, EXCEPTION_POINTERS* info) {
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_INT_DIVIDE_BY_ZERO
        && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    std::lock_guard<std::mutex> guard(s_reportedLock);
    if (s_reported == nullptr) {
        s_reported = new std::unordered_set<uint64_t>();
    }
    // keyed apart from GenUnimplemented's addresses
    if (s_reported->insert(address | (1ull << 63)).second) {
        uint64_t at = 0;
        if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
            at = info->ExceptionRecord->ExceptionInformation[1];
        }
        FrostLog("translated", "FUN_%llx faulted (%08lx at %p, touching %llx); it returns nothing",
                 address, code, info->ExceptionRecord->ExceptionAddress, static_cast<unsigned long long>(at));
        // (the filter runs on the faulting thread with its stack intact: from the fault up to the
        // translated function this filter guards, which may be many calls away)
        FrostLogContextStack(info->ContextRecord, 24);
        MemoryDescribeFreed(at);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// The image's own data holds pointers to its globals (tables of {table, callback}, of objects to
// visit): where the port holds that global, the image's copy is never built, and the pointer
// must be the port's object instead. Every such pointer is one of the image's base relocations
// (DIR64), so each relocation outside the code whose value falls inside a held global is moved
// to the same offset in the port's object. Without it the DB2 patch-callback table (0x142d2d1d0)
// named Spell's unbuilt image copy, and entering the world aborted freeing its garbage vector
// (2026-10-21).
size_t RedirectHeldPointers(uint64_t base, const IMAGE_NT_HEADERS64& nt) {
    const IMAGE_DATA_DIRECTORY& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (dir.VirtualAddress == 0 || g_genHeldCount == 0) {
        return 0;
    }
    // (g_genHeld is sorted by m_ref)
    size_t moved = 0;
    auto at = reinterpret_cast<const uint8_t*>(base + dir.VirtualAddress);
    auto end = at + dir.Size;
    while (at + sizeof(IMAGE_BASE_RELOCATION) <= end) {
        auto block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(at);
        if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) {
            break;
        }
        auto entries = reinterpret_cast<const uint16_t*>(block + 1);
        size_t n = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2;
        for (size_t i = 0; i < n; i++) {
            if ((entries[i] >> 12) != IMAGE_REL_BASED_DIR64) {
                continue;
            }
            uint64_t slot = base + block->VirtualAddress + (entries[i] & 0xfff);
            if (slot >= kTextStart && slot < kTextEnd) {
                continue;
            }
            uint64_t value = *reinterpret_cast<const uint64_t*>(slot);
            // the last held global starting at or before the value
            size_t lo = 0;
            size_t hi = g_genHeldCount;
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                if (g_genHeld[mid].m_ref <= value) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            if (lo == 0) {
                continue;
            }
            const GENHELD& held = g_genHeld[lo - 1];
            if (value >= held.m_ref + held.m_size) {
                continue;
            }
            auto port = static_cast<uint8_t*>(held.m_port != nullptr ? held.m_port : *held.m_portPtr);
            *reinterpret_cast<uint64_t*>(slot) = reinterpret_cast<uint64_t>(port + (value - held.m_ref));
            moved++;
        }
        at += block->SizeOfBlock;
    }
    return moved;
}

bool RefImageInitialize(const char* path) {
    if (s_mapped) {
        return true;
    }
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        FrostLog("refimage", "%s not found: translated code disabled", path);
        return false;
    }
    IMAGE_DOS_HEADER dos = {};
    IMAGE_NT_HEADERS64 nt = {};
    bool ok = std::fread(&dos, sizeof(dos), 1, file) == 1 && dos.e_magic == IMAGE_DOS_SIGNATURE
        && std::fseek(file, dos.e_lfanew, SEEK_SET) == 0 && std::fread(&nt, sizeof(nt), 1, file) == 1
        && nt.Signature == IMAGE_NT_SIGNATURE && nt.OptionalHeader.ImageBase == 0x140000000ull;
    if (!ok) {
        std::fclose(file);
        FrostLog("refimage", "%s is not the reference image", path);
        return false;
    }
    uint64_t base = nt.OptionalHeader.ImageBase;
    uint32_t count = nt.FileHeader.NumberOfSections;
    // one reservation for the whole image (an allocation starts on a 64K boundary, and the
    // sections do not), then the jump pool right after it
    uint64_t imageSize = nt.OptionalHeader.SizeOfImage;
    s_pool = (base + imageSize + 0xffff) & ~0xffffull;
    void* image = VirtualAlloc(reinterpret_cast<void*>(base), imageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (image != reinterpret_cast<void*>(base)) {
        FrostLog("refimage", "cannot reserve the reference image at %llx: translated code disabled",
                 static_cast<unsigned long long>(base));
        std::fclose(file);
        return false;
    }
    // the headers too: the reference names its own image as the module of its resources (the
    // region picker's dialog and strings), and the resource functions read them from here
    std::fseek(file, 0, SEEK_SET);
    if (std::fread(image, 1, nt.OptionalHeader.SizeOfHeaders, file) != nt.OptionalHeader.SizeOfHeaders) {
        FrostLog("refimage", "short read of the headers");
    }
    std::fseek(file, dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader,
               SEEK_SET);
    for (uint32_t i = 0; i < count; i++) {
        IMAGE_SECTION_HEADER section;
        if (std::fread(&section, sizeof(section), 1, file) != 1) {
            break;
        }
        long next = std::ftell(file);
        uint64_t va = base + section.VirtualAddress;
        uint64_t size = (std::min)(static_cast<uint64_t>((std::max)(section.Misc.VirtualSize, section.SizeOfRawData)),
                                   imageSize - section.VirtualAddress);
        bool code = (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        void* memory = reinterpret_cast<void*>(va);
        if (code) {
            std::memset(memory, 0xcc, size);
            // except the native ranges: the reference's own machine code, which runs in place
            for (size_t n = 0; n < g_genNativeCount; n++) {
                uint64_t lo = (std::max)(g_genNative[2 * n], va);
                uint64_t hi = (std::min)(g_genNative[2 * n + 1], va + (std::min)(size, static_cast<uint64_t>(section.SizeOfRawData)));
                if (lo < hi && std::fseek(file, static_cast<long>(section.PointerToRawData + (lo - va)), SEEK_SET) == 0
                    && std::fread(reinterpret_cast<void*>(lo), 1, hi - lo, file) != hi - lo) {
                    FrostLog("refimage", "short read of the native range at %llx", static_cast<unsigned long long>(lo));
                }
            }
        } else if (section.SizeOfRawData != 0) {
            std::fseek(file, section.PointerToRawData, SEEK_SET);
            size_t raw = (std::min)(static_cast<size_t>(section.SizeOfRawData), static_cast<size_t>(section.Misc.VirtualSize));
            if (std::fread(memory, 1, raw, file) != raw) {
                FrostLog("refimage", "short read of %.8s", section.Name);
            }
        }
        std::fseek(file, next, SEEK_SET);
    }
    std::fclose(file);

    for (size_t i = 0; i < g_genImportCount; i++) {
        const GENIMPORT& imp = g_genImports[i];
        HMODULE module = LoadLibraryA(imp.m_dll);
        FARPROC proc = nullptr;
        if (module) {
            // (`#12`: by ordinal)
            proc = imp.m_name[0] == '#'
                ? GetProcAddress(module, MAKEINTRESOURCEA(static_cast<WORD>(std::atoi(imp.m_name + 1))))
                : GetProcAddress(module, imp.m_name);
        }
        if (proc) {
            *reinterpret_cast<FARPROC*>(imp.m_slot) = proc;
        } else {
            FrostLog("refimage", "native code's import %s!%s not found", imp.m_dll, imp.m_name);
        }
    }

    // at every function the port can run, a 5-byte `jmp rel32` (functions can start closer
    // together than an absolute jump is long) into a pool past the image, whose entry is
    // `jmp [rip+0]` and the target
    size_t poolSize = (g_genJumpCount + 1) * 16;
    auto pool = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(s_pool), poolSize, MEM_RESERVE | MEM_COMMIT,
                                                   PAGE_READWRITE));
    if (pool != reinterpret_cast<uint8_t*>(s_pool)) {
        FrostLog("refimage", "cannot place the jump pool: translated code disabled");
        return false;
    }
    // A function that starts less than 5 bytes before the next one (MSVC packs one-instruction
    // functions, `mov al, 1; ret`, four bytes apart) cannot take the jump: the next one's would
    // overwrite its last byte, or its own would overwrite the next one's int3. FUN_14026bea0, the
    // SSL filter's slot 1, jumped to 0x129a4d530 at login on that (2026-10-18; 19 such pairs).
    // Those keep the int3, and the trap handler sends them on. Function starts: the jump
    // table's, and the image's .pdata, which also has the non-leaf functions the port cannot run.
    std::vector<uint64_t> starts;
    starts.reserve(g_genJumpCount * 2);
    for (size_t i = 0; i < g_genJumpCount; i++) {
        starts.push_back(g_genJumps[i].m_address);
    }
    const IMAGE_DATA_DIRECTORY& pdata = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    auto functions = reinterpret_cast<const RUNTIME_FUNCTION*>(base + pdata.VirtualAddress);
    for (size_t i = 0; pdata.VirtualAddress != 0 && i < pdata.Size / sizeof(RUNTIME_FUNCTION); i++) {
        starts.push_back(base + functions[i].BeginAddress);
    }
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    // the native ranges' unwind entries (the image's .pdata is sorted, so each range's are one
    // run of it): a fault or a stack walk through the reference's own code unwinds its frames
    for (size_t n = 0; pdata.VirtualAddress != 0 && n < g_genNativeCount; n++) {
        size_t total = pdata.Size / sizeof(RUNTIME_FUNCTION);
        size_t first = 0;
        while (first < total && base + functions[first].BeginAddress < g_genNative[2 * n]) {
            first++;
        }
        size_t last = first;
        while (last < total && base + functions[last].BeginAddress < g_genNative[2 * n + 1]) {
            last++;
        }
        if (last > first) {
            RtlAddFunctionTable(const_cast<RUNTIME_FUNCTION*>(functions + first), static_cast<DWORD>(last - first), base);
        }
    }
    for (size_t i = 0; i < g_genJumpCount; i++) {
        uint8_t* entry = pool + i * 16;
        entry[0] = 0xff;
        entry[1] = 0x25;
        std::memset(entry + 2, 0, 4);
        std::memcpy(entry + 6, &g_genJumps[i].m_target, 8);
        uint64_t address = g_genJumps[i].m_address;
        auto next = std::upper_bound(starts.begin(), starts.end(), address);
        if (next != starts.end() && *next - address < 5) {
            s_traps.emplace_back(address, g_genJumps[i].m_target);
            continue;
        }
        auto at = reinterpret_cast<uint8_t*>(address);
        int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(entry) - reinterpret_cast<intptr_t>(at + 5));
        at[0] = 0xe9;
        std::memcpy(at + 1, &rel, 4);
    }
    // (after the entry jumps, which the reference's own bytes replace)
    ApplyNativeOverrides(path, base, nt);
    std::sort(s_traps.begin(), s_traps.end());
    if (!s_traps.empty()) {
        FrostLog("refimage", "%zu functions too close to the next for a jump: reached through the trap",
                 s_traps.size());
    }
    DWORD old;
    VirtualProtect(pool, poolSize, PAGE_EXECUTE_READ, &old);
    VirtualProtect(reinterpret_cast<void*>(kTextStart), kTextEnd - kTextStart, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(kTextStart), kTextEnd - kTextStart);
    AddVectoredExceptionHandler(1, TrapHandler);
    s_mapped = true;
    FrostLog("refimage", "reference image mapped; %zu functions reachable through it", g_genJumpCount);

    // the image's own static initializers (its CRT table), the ones translated: they construct
    // the reference's globals (locks, lists, tables) as its startup would have, so translated
    // code finds them built rather than zeroed. Each goes through its guarded entry.
    size_t moved = RedirectHeldPointers(base, nt);
    FrostLog("refimage", "%zu of the image's pointers sent to the port's globals", moved);
    StartWatch();
    size_t ran = 0;
    for (size_t i = 0; i < g_genInitCount; i++) {
        ran += RunInitializer(g_genInits[i]);
    }
    FrostLog("refimage", "%zu of the image's static initializers run", ran);
    StartHitDump();
    return true;
}

bool RefImageMapped() {
    return s_mapped;
}

// The reference's own thread-locals: translated code reaches them as the image's TLS block
// (`ThreadLocalStoragePointer[_tls_index]`), which the loader never built, the image not being
// loaded as a module. Each thread gets one the first time it asks, from the image's TLS template
// (its raw data, then the zero fill), as the loader would have made it. Its callbacks do not run,
// so a thread-local with a constructor starts as its template. The thread-safe statics' epoch
// (+0x2140) is the port's own and never read from here (tools/translate.py, INIT_EPOCH).
// The thread's TLS slot array as the reference's code indexes it (`*(longlong *)(
// ThreadLocalStoragePointer + _tls_index * 8)`): one slot, the block below, at index 0, which is
// what Ghidra.h spells _tls_index. With these two names any shape of the access works, where the
// translator matched a few spellings and stubbed 18 functions for the rest (2026-10-16).
extern "C" void* GenRefTlsBlock();
extern "C" void** GenRefTlsSlots() {
    static thread_local void* s_slot = nullptr;
    if (s_slot == nullptr) {
        s_slot = GenRefTlsBlock();
    }
    return &s_slot;
}

extern "C" void* GenRefTlsBlock() {
    static thread_local uint8_t* s_block = nullptr;
    if (s_block == nullptr) {
        constexpr uint64_t base = 0x140000000ull;
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        const uint8_t* raw = nullptr;
        size_t rawSize = 0;
        size_t size = 0;
        if (dir.VirtualAddress != 0) {
            auto tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(base + dir.VirtualAddress);
            raw = reinterpret_cast<const uint8_t*>(tls->StartAddressOfRawData);
            rawSize = static_cast<size_t>(tls->EndAddressOfRawData - tls->StartAddressOfRawData);
            size = rawSize + tls->SizeOfZeroFill;
        }
        s_block = static_cast<uint8_t*>(std::calloc(1, (std::max)(size, static_cast<size_t>(0x2200))));
        if (raw != nullptr && rawSize != 0) {
            std::memcpy(s_block, raw, rawSize);
        }
    }
    return s_block;
}
