#include "storm/Log.hpp"
#include "util/Unimplemented.hpp"

namespace {

// DAT_142e1d854: set once the log system is initialized (not ported yet).
uint32_t s_logInitialized;

}

// ref: FUN_14025a990
bool SLogCreate(const char* filename, uint32_t flags, HSLOG* log) {
    (void)filename;
    (void)flags;
    *log = nullptr;
    FROST_UNIMPLEMENTED();
    return false;
}

// Flushes and closes a log and unlinks it from its bucket. Nothing happens before the log
// system is up or for a null log; the rest waits for the log system's port.
// ref: FUN_14025a880
void SLogClose(HSLOG log) {
    if (!s_logInitialized || !log) {
        return;
    }
    FROST_UNIMPLEMENTED();
}

// The rest waits for the log system's port, as SLogCreate does.
// ref: FUN_14025b4a0
int32_t SLogWrite(HSLOG log, const char* format, ...) {
    (void)format;
    if (!log) {
        return 0;
    }
    FROST_UNIMPLEMENTED();
    return 0;
}
