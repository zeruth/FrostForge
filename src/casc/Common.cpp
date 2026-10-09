#include "casc/Common.hpp"
#include "os/Os.hpp"
#include <cstring>

namespace casc {

const char g_logCategory[] = "casc";

namespace {

const char s_statsPrefix[] = "casc.stats.";  // s_casc_stats__1428f21e0
const float s_fullRate = 1.0f;               // DAT_142618d64

// ref: FUN_1421570b0
int64_t FormatCounter(char* out, uint64_t size, const char* name, int64_t value, float rate) {
    if (rate < s_fullRate) {
        return OsSnprintf(out, size, "%s:%lld|c|@%0.2f", name, value, static_cast<double>(rate));
    }
    return OsSnprintf(out, size, "%s:%lld|c", name, value);
}

// ref: FUN_142157110
int64_t FormatGauge(char* out, uint64_t size, const char* name, int64_t value) {
    return OsSnprintf(out, size, "%s:%lld|g", name, value);
}

// ref: FUN_142157130
int64_t FormatSet(char* out, uint64_t size, const char* name, int64_t value) {
    return OsSnprintf(out, size, "%s:%lld|s", name, value);
}

// ref: FUN_142157150
int64_t FormatTimer(char* out, uint64_t size, const char* name, int64_t value, float rate) {
    if (rate < s_fullRate) {
        return OsSnprintf(out, size, "%s:%lld|ms|@%0.2f", name, value, static_cast<double>(rate));
    }
    return OsSnprintf(out, size, "%s:%lld|ms", name, value);
}

}

// ref: FUN_142143390
void StatsReport(STATSINK* sink, int32_t kind, const char* name, int64_t value, float rate) {
    if (!sink->m_report) {
        return;
    }
    char line[0x200];
    std::memset(line + 1, 0, 0x1FF);
    std::memcpy(line, s_statsPrefix, 11);
    char* body = line + 11;
    if (kind == 0) {
        FormatCounter(body, 0x1F5, name, value, rate);
    } else if (kind == 1) {
        FormatGauge(body, 0x1F5, name, value);
    } else if (kind == 2) {
        FormatSet(body, 0x1F5, name, value);
    } else if (kind == 3) {
        FormatTimer(body, 0x1F5, name, value, rate);
    }
    sink->m_report(sink->m_context, sink->m_owner, line);
}

// ref: FUN_142157210
int32_t ErrorFromOs(uint32_t error) {
    if (error == 0) {
        return 0;
    }
    if (error != 5) {
        return error != 0x70 ? 8 : 7;
    }
    return 0x14;
}

// The name a table file has; built inline at every call site in the reference.
void IndexFileName(char name[16], uint32_t bucket, uint32_t version) {
    static const char s_digits[] = "0123456789abcdef";
    name[0] = '0';
    name[1] = bucket < 0x10 ? s_digits[bucket] : '\0';  // FUN_142366220
    for (int32_t i = 0; i < 8; i++) {
        name[2 + i] = s_digits[(version >> (28 - 4 * i)) & 0xF];
    }
    std::memcpy(name + 10, ".idx", 5);  // DAT_1428f39a8
}

}
