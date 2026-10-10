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
