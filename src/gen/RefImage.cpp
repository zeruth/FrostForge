#include "gen/RefImage.hpp"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <vector>
#include <unordered_set>

#include <windows.h>

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

std::mutex s_reportedLock;
std::unordered_set<uint64_t>* s_reported;

LONG CALLBACK TrapHandler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* context = info->ContextRecord;
    uint64_t address = context->Rip;
    if (address < kTextStart || address >= kTextEnd) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    GenUnimplemented(address);
    // return 0 to the caller, as an empty function would
    context->Rax = 0;
    context->Rip = *reinterpret_cast<uint64_t*>(context->Rsp);
    context->Rsp += 8;
    return EXCEPTION_CONTINUE_EXECUTION;
}

}

extern "C" void GenUnimplemented(uint64_t address) {
    std::lock_guard<std::mutex> guard(s_reportedLock);
    if (s_reported == nullptr) {
        s_reported = new std::unordered_set<uint64_t>();
    }
    if (s_reported->insert(address).second) {
        FrostLog("unimplemented", "reference FUN_%llx (translated code reached it)",
                 static_cast<unsigned long long>(address));
    }
}

// The exception filter around a translated Lua handler (src/gen/handlers.cpp): a hardware fault
// is logged once with where it happened and the call returns nothing; anything else (a Lua error
// unwinding as a C++ exception or a longjmp) goes on its way.
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
    }
    return EXCEPTION_EXECUTE_HANDLER;
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
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
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
    for (size_t i = 0; i < g_genJumpCount; i++) {
        uint8_t* entry = pool + i * 16;
        entry[0] = 0xff;
        entry[1] = 0x25;
        std::memset(entry + 2, 0, 4);
        std::memcpy(entry + 6, &g_genJumps[i].m_target, 8);
        auto at = reinterpret_cast<uint8_t*>(g_genJumps[i].m_address);
        int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(entry) - reinterpret_cast<intptr_t>(at + 5));
        at[0] = 0xe9;
        std::memcpy(at + 1, &rel, 4);
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
