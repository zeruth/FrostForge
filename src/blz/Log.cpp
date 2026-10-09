#include "blz/Log.hpp"

namespace blz {

log_sink g_logSinks[16];  // DAT_144751aa0

// ref: FUN_14235a850
bool log_register(log_sink sink) {
    for (log_sink& slot : g_logSinks) {
        if (!slot) {
            slot = sink;
            return true;
        }
    }
    return false;
}

// ref: FUN_14235a890
void log(const char* category, int32_t level, const char* file, int32_t line, const char* format, ...) {
    for (log_sink sink : g_logSinks) {
        if (sink) {
            va_list args;
            va_start(args, format);
            sink(category, level, file, line, format, args);
            va_end(args);
        }
    }
}

}
