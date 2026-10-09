#include "util/Unimplemented.hpp"
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <windows.h>

void FrostUnimplemented(const char* function) {
    static std::mutex s_lock;
    static std::set<std::string> s_seen;

    std::lock_guard<std::mutex> guard(s_lock);
    if (!s_seen.insert(function).second) {
        return;
    }

    char line[512];
    std::snprintf(line, sizeof(line), "[frostlite] unimplemented: %s\n", function);
    std::fputs(line, stderr);
    std::fflush(stderr);
    OutputDebugStringA(line);
}
