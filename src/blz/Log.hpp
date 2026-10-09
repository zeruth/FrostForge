#ifndef BLZ_LOG_HPP
#define BLZ_LOG_HPP

#include <cstdarg>
#include <cstdint>

namespace blz {

// The packages' log (tact, casc): a message goes to every installed sink with its category
// ("tact", "casc"), level (3 warning, 4 error), source file and line.
typedef void (*log_sink)(const char* category, int32_t level, const char* file, int32_t line, const char* format,
    va_list args);

void log(const char* category, int32_t level, const char* file, int32_t line, const char* format, ...);

// The sink table holds 16 entries.
extern log_sink g_logSinks[16];

// Installs a sink in the first free slot; false when all 16 are taken.
bool log_register(log_sink sink);

}

#endif
