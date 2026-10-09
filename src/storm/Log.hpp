#ifndef STORM_LOG_HPP
#define STORM_LOG_HPP

// Storm's text logs: a handle per log, lines buffered in memory with a time stamp each and
// written out in blocks (storm/Log.cpp).

#include <cstdarg>
#include <cstdint>

typedef void* HSLOG;

// SLogCreate flags: open the file now (else on the first write), no file, append.
#define SLOG_FLAG_OPEN_NOW 0x1
#define SLOG_FLAG_NO_FILE  0x2
#define SLOG_FLAG_APPEND   0x4

void SLogInitialize();
void SLogResetDebugOutput();

// Opens (creates) a log file. 3.3.5's name: same role and the same (filename, flags, handle)
// shape, returning whether the log was opened. A name without a folder goes beside the
// executable.
bool SLogCreate(const char* filename, uint32_t flags, HSLOG* log);

void SLogClose(HSLOG log);

// Writes out what the log has buffered.
void SLogFlush(HSLOG log);

// Appends a formatted line; nothing for a null log. Returns what the formatter returned.
int32_t SLogWrite(HSLOG log, const char* format, ...);
int32_t SLogWriteV(HSLOG log, const char* format, va_list args);

#endif
