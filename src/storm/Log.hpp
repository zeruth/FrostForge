#ifndef STORM_LOG_HPP
#define STORM_LOG_HPP

#include <cstdint>

typedef void* HSLOG;

// Opens (creates) a log file. 3.3.5's name: same role and the same (filename, flags, handle)
// shape, returning whether the log was opened.
bool SLogCreate(const char* filename, uint32_t flags, HSLOG* log);

void SLogClose(HSLOG log);

// Appends a formatted line; nothing for a null log. Returns what the formatter returned.
int32_t SLogWrite(HSLOG log, const char* format, ...);

#endif
