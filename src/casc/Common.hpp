#ifndef CASC_COMMON_HPP
#define CASC_COMMON_HPP

// pkg/casc/1.4.41: what the casc sources share - the log category, OS errors as casc errors,
// the statistics sink, and the names of index files.

#include <cstdint>

namespace casc {

extern const char g_logCategory[];  // "casc" (DAT_142630ce4)

// Statistics are reported through a callback block the container hands down (the same
// 0x28-byte block tact keeps at instance+0x2d8): {owner, stat callback, context, ...}.
struct STATSINK {
    void* m_owner;
    void (*m_report)(void* context, void* owner, const char* line);
    void* m_context;
    void* m_18;
    void* m_20;
};

// kind: 0 counter, 1 gauge, 2 set, 3 timer; rate is the sample rate.
void StatsReport(STATSINK* sink, int32_t kind, const char* name, int64_t value, float rate);

// An OS error as a casc error: 5 (access denied) is CascLocked... see the table in Common.cpp.
int32_t ErrorFromOs(uint32_t error);

// "0" + bucket hex digit + 8 hex digits of the version (big-endian) + ".idx".
void IndexFileName(char name[16], uint32_t bucket, uint32_t version);

}

#endif
