#include "util/FunctionPointer.hpp"
#include "storm/ErrorReport.hpp"
#include "storm/String.hpp"
#include <cstring>
#include <windows.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

// / DAT_1430a1558: the .text section's bounds, found on first use.
uint64_t s_textStart;  // 0x1430a1550
uint64_t s_textEnd;

}

// ref: FUN_1412395b0
void CheckFunctionPointer(uint64_t function) {
    if (s_textStart == 0 || s_textEnd == 0) {
        auto base = reinterpret_cast<const uint8_t*>(&__ImageBase);
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + __ImageBase.e_lfanew);
        auto sections = IMAGE_FIRST_SECTION(nt);
        bool found = false;
        for (uint16_t i = 0; i < nt->FileHeader.NumberOfSections; i++) {
            if (std::strncmp(reinterpret_cast<const char*>(sections[i].Name), ".text", 8) == 0) {
                s_textStart = sections[i].VirtualAddress + nt->OptionalHeader.ImageBase;
                s_textEnd = sections[i].Misc.VirtualSize + sections[i].VirtualAddress + nt->OptionalHeader.ImageBase;
                found = true;
                break;
            }
        }
        if (!found) {
            ErrorDisplayFormatted(0x85100086, nullptr, 0, "Unable to find .text section");
        }
    }

    // DIVERGED: translated code hands on the reference's own function addresses, which RefImage
    // backs with jumps inside the reference's .text, mapped at its original base; those are
    // valid code pointers too (they were reported as invalid on every ClientServices poll).
    constexpr uint64_t kReferenceTextStart = 0x140001000ull;
    constexpr uint64_t kReferenceTextEnd = 0x14260c5d3ull;
    if (kReferenceTextStart <= function && function < kReferenceTextEnd) {
        return;
    }

    if (function < s_textStart || s_textEnd <= function) {
        char message[64];
        SStrPrintf64(message, "Invalid function pointer: %p", reinterpret_cast<void*>(function));
        ErrorDisplayFormatted(0x85100086, nullptr, 0, message);
    }
}
