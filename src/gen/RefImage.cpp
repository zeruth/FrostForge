#include "gen/RefImage.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>
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

void GenUnimplemented(uint64_t address);
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
    return true;
}

bool RefImageMapped() {
    return s_mapped;
}
