#include "util/Unimplemented.hpp"
#include "util/FrostLog.hpp"
#include <mutex>
#include <set>
#include <string>

void FrostUnimplemented(const char* function) {
    static std::mutex s_lock;
    static std::set<std::string> s_seen;

    {
        std::lock_guard<std::mutex> guard(s_lock);
        if (!s_seen.insert(function).second) {
            return;
        }
    }

    // The stub, then who reached it.
    FrostLog("unimplemented", "%s", function);
    FrostLogStack(0, 4);
}

// What a stub forwarded to its translation (tools/forward_stubs.py) links against where src/gen is
// not linked: each forwarding file names these as the alternates of the translated function and
// of GenFault (`#pragma comment(linker, "/alternatename:...")`), which the linker takes only when
// nothing defines the real one. The client links src/gen and never reaches them; a test that links
// a forwarding library (ObjectAllocTest pulling os's Path.cpp) links. 2026-10-18
extern "C" unsigned long long GenForwardMissing() {
    FrostUnimplemented("a translated function, without src/gen linked");
    return 0;
}

extern "C" int GenFaultMissing(unsigned long long, void*) {
    return 0;   // EXCEPTION_CONTINUE_SEARCH
}
