#include "cas/CAS.hpp"
#include "cas/CASLocalManifest.hpp"
#include "base/Status.hpp"
#include "blz/Log.hpp"
#include "blz/String.hpp"
#include "blz/Sync.hpp"
#include "os/Os.hpp"
#include "storm/Log.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "tact/AsyncState.hpp"
#include "tact/Codec.hpp"
#include "tact/FileHandle.hpp"
#include "tact/ImmediateRead.hpp"
#include "tact/InstallationInfo.hpp"
#include "tact/Instance.hpp"
#include "util/Unimplemented.hpp"
#include <windows.h>
#include <cstring>

namespace cas {

CASINSTANCE* g_casInstance;
CASMANIFEST* g_casManifest;
LOCALMANIFEST* g_localManifest;

namespace {

const char s_typesFile[] = "Engine/Source/CAS/CASTypes.h";
const char s_errorNodeTag[] = ".?AU?$list_node@UCASStreamingError@@@blz@@";

thread_local CASERROR t_lastError;

char s_dataPath[0x104];             // DAT_142e25690
char s_localRoot[0x104];            // DAT_142e25580
HSLOG s_tactLog;                    // DAT_142e25688
bool s_tactLogEnabled;              // DAT_142e20882
volatile bool s_initialized;        // DAT_142e20880
uint32_t s_mainThread;              // DAT_142e25578
bool s_manifestOnly;                // DAT_142e20883
uint32_t s_openA4;                  // DAT_142ccba48
bool s_patchInstances;              // DAT_142e20881: the patch path's instances are live
void* s_updateInstance;             // DAT_142e20868: an in-place update under way
CASKEYCALLBACK s_keyCallback;       // DAT_142e25560

// The last 64 tact/casc errors and warnings (0x98-byte slots, a blz::string first).
struct LOGSLOT {
    blz::string m_text;
    uint8_t m_28[0x70];
};
static_assert(sizeof(LOGSLOT) == 0x98, "LOGSLOT is 0x98 bytes");
LOGSLOT s_errors[64];               // DAT_142e20960
LOGSLOT s_warnings[64];             // DAT_142e22f60
int32_t s_errorIndex;               // DAT_142e20958
int32_t s_warningIndex;             // DAT_142e2095c
SRWLOCK s_logLock = SRWLOCK_INIT;   // DAT_142e20950

// The streaming errors tact's counters report (blz::list<CASStreamingError>, 0x210-byte nodes).
struct STREAMINGERROR {
    STREAMINGERROR* m_next;
    STREAMINGERROR* m_prev;
    char m_text[0x200];
};
struct ERRORLIST {
    STREAMINGERROR* m_next;
    STREAMINGERROR* m_prev;
    uint64_t m_count;
};
ERRORLIST s_streamingErrors = { reinterpret_cast<STREAMINGERROR*>(&s_streamingErrors),
    reinterpret_cast<STREAMINGERROR*>(&s_streamingErrors), 0 };  // PTR_LOOP_142ccba30
SRWLOCK s_streamingLock = SRWLOCK_INIT;  // DAT_142e20948

// What the failure counter hook watches (PTR_s_download_requests_failed_142ccba70).
struct COUNTERHOOK {
    const char* m_name;
    volatile long m_count;
};
COUNTERHOOK s_downloadFailures = { "download.requests.failed", 0 };

void Remember(LOGSLOT* ring, int32_t* index, const char* text) {
    LOGSLOT* slot = &ring[(*index)++];
    slot->m_text.reserve(std::strlen(text));
    uint64_t length = std::strlen(text);
    if (length != 0) {
        std::memmove(slot->m_text.m_data, text, length);
    }
    slot->m_text.m_data[length] = '\0';
    slot->m_text.m_size = length;
    if (*index > 0x3f) {
        *index = 0;
    }
}

// tact's and casc's log, into Tact.log and the rings.
// ref: FUN_1402cf670
void LogSink(const char* category, int32_t level, const char* file, int32_t line, const char* format, va_list args) {
    (void)file;
    (void)line;
    if (std::strcmp(category, "tact") != 0 && std::strcmp(category, "casc") != 0) {
        return;
    }
    char text[0x200];
    SStrVPrintfCount(text, sizeof(text), format, args);
    if (s_tactLog != nullptr) {
        SLogWrite(s_tactLog, text);
    }
    AcquireSRWLockExclusive(&s_logLock);
    if (level < 4) {
        if (level > 1) {
            Remember(s_warnings, &s_warningIndex, text);
        }
    } else {
        Remember(s_errors, &s_errorIndex, text);
    }
    ReleaseSRWLockExclusive(&s_logLock);
}

// tact's download statistics.
// ref: FUN_1402d3000
void StatsCallback(CASSTATS* stats, void* a2, int32_t started, int32_t finished, int32_t bytes) {
    (void)a2;
    int64_t now = OsGetElapsedNanoseconds();
    if (started != 0 && InterlockedExchangeAdd(&stats->m_active, started) == 0) {
        stats->m_start = now;
    }
    uint32_t sample = stats->m_bytes + static_cast<uint32_t>(bytes);
    stats->m_bytes = sample;
    // As the reference has it: the start minus now, so this holds only for a clock step back.
    if (stats->m_start - now > 1000000000) {
        int64_t elapsed = now - stats->m_start;
        if (sample != 0 && elapsed != 0) {
            stats->m_start = now;
            stats->m_bytes = 0;
            stats->m_rate = static_cast<float>(sample) * 1e9f / static_cast<float>(elapsed) * stats->m_smoothing
                + (1.0f - stats->m_smoothing) * stats->m_rate;
        }
    }
    if (finished != 0 && InterlockedExchangeAdd(&stats->m_active, -finished) == finished) {
        uint32_t pending = stats->m_bytes;
        int64_t elapsed = now - stats->m_start;
        if (pending != 0 && elapsed != 0) {
            stats->m_start = now;
            stats->m_bytes = 0;
            stats->m_rate = static_cast<float>(pending) * 1e9f / static_cast<float>(elapsed) * stats->m_smoothing
                + (1.0f - stats->m_smoothing) * stats->m_rate;
        }
    }
}

// tact's counter hook: a failed download becomes a streaming error.
// ref: FUN_1402d1140
void CounterHook(COUNTERHOOK* hook, void* a2, const char* counter) {
    (void)a2;
    if (SStrStrI(counter, hook->m_name) == nullptr) {
        return;
    }
    InterlockedIncrement(&hook->m_count);
    char text[0x200];
    SStrPrintf(text, sizeof(text), "Streaming Error: %s", "A download request has failed (no file context is available).");
    AcquireSRWLockExclusive(&s_streamingLock);
    auto node = static_cast<STREAMINGERROR*>(
        SMemAlloc(sizeof(STREAMINGERROR), 0xB, s_errorNodeTag, SStrHashFnv1a(s_errorNodeTag), -6, 0));
    if (node != nullptr) {
        std::memcpy(node->m_text, text, sizeof(text));
    }
    node->m_next = s_streamingErrors.m_next;
    node->m_prev = reinterpret_cast<STREAMINGERROR*>(&s_streamingErrors);
    s_streamingErrors.m_next = node;
    node->m_next->m_prev = node;
    s_streamingErrors.m_count++;
    ReleaseSRWLockExclusive(&s_streamingLock);
}

// tact's key lookup, through the client's callback.
// ref: FUN_1402d2690
bool KeyLookup(CASKEYCALLBACK* holder, uint64_t* keyName, void* key) {
    if (*holder == nullptr) {
        return false;
    }
    uint64_t name = *keyName;
    return (*holder)(&name, key, 0x10) != 0;
}

// The instance's destructor.
// ref: FUN_1402cc490
void DestroyInstance(CASINSTANCE* instance, uint32_t flags) {
    if (!instance->m_noCdn && !instance->m_noStreaming && instance->m_tact != nullptr) {
        tact::InstanceCancelOp(instance->m_tact, instance->m_ops[0]);
    }
    if (!instance->m_noCdn) {
        if (!instance->m_noStreaming && instance->m_tact != nullptr) {
            tact::InstanceCancelOp(instance->m_tact, instance->m_ops[1]);
        }
        if (!instance->m_noCdn && !instance->m_noStreaming && instance->m_tact != nullptr) {
            tact::InstanceCancelOp(instance->m_tact, instance->m_ops[2]);
        }
    }
    tact::Instance::Destroy(instance->m_tact);
    if (instance->m_stats != nullptr) {
        SMemFree(instance->m_stats, __FILE__, 0x3cd, 0);
        instance->m_stats = nullptr;
    }
    if ((flags & 1) != 0) {
        operator delete(instance, sizeof(CASINSTANCE));
    }
}

// strcpy bounded to `limit` characters and a terminator, as the reference inlines it.
void CopyBounded(char* dest, const char* source, int64_t limit) {
    for (int64_t left = limit;;) {
        char c = *source++;
        *dest++ = c;
        if (c == '\0') {
            return;
        }
        if (--left == 0) {
            *dest = '\0';
            return;
        }
    }
}

}

CASERROR* CASGetLastError() {
    return &t_lastError;
}

// ref: FUN_1402cdc50
uint32_t CASGetErrorCode(int32_t* kind) {
    int32_t current = t_lastError.m_kind;
    *kind = current;
    if (current == 0) {
        return t_lastError.m_tactError;
    }
    if (current == 2) {
        return GetLastError();  // FUN_1402144c0
    }
    if (current != 1 && current != 3) {
        return 0;
    }
    return t_lastError.m_code;
}

namespace {

// The text of CAS's own error codes (PTR_s_Success_142630e40).
const char* const s_casErrorStrings[] = {
    "Success",
    "General Failure",
    "Can't find file in build manifest",
    "Loose file not found and CAS is in loose file only mode.",
    "Unknown file type",
    "Deleted remote file",
    "Canceled",
    "Missing TACT Cryptkey",
    "Container needs repair",
    "OS File Error",
    "Code/Data mismatch",
};

char* CopyErrorText(char* dest, uint64_t size, const char* text) {
    if (size == 0) {
        return dest;
    }
    char* write = dest;
    while (--size != 0) {
        char c = *text++;
        *write++ = c;
        if (c == '\0') {
            return dest;
        }
    }
    *write = '\0';
    return dest;
}

}

// The last error as text, by its kind.
// ref: FUN_1402cdca0
char* CASFormatLastError(char* dest, uint64_t size) {
    int32_t kind = t_lastError.m_kind;
    if (kind == 0) {
        return CopyErrorText(dest, size, tact::ErrorString(t_lastError.m_tactError));
    }
    if (kind == 2) {
        OsGetLastErrorText(dest, static_cast<uint32_t>(size));
        return dest;
    }
    if (kind != 1 && kind != 3) {
        return CopyErrorText(dest, size, "");
    }
    return CopyErrorText(dest, size, t_lastError.m_code < 0xb ? s_casErrorStrings[t_lastError.m_code] : "Unknown CAS error");
}

namespace {

// blz::pair<uint64_t, blz::function<...>> (0x28 bytes).
struct HANDLERENTRY {
    uint64_t m_key;
    blz::function_storage m_function;
};
static_assert(sizeof(HANDLERENTRY) == 0x28, "HANDLERENTRY is 0x28 bytes");

// blz::vector: data, size, capacity (bit 63: the storage is not owned).
struct HANDLERVECTOR {
    HANDLERENTRY* m_data;
    uint64_t m_size;
    uint64_t m_capacity;
};

const uint64_t NOT_OWNED = 0x8000000000000000ull;
const char s_hotfixTag[] = ".?AU?$pair@_KV?$function@$$A6AXAEBVCStatus@@@Z@blz@@@blz@@";
const char s_manifestTag[] = ".?AU?$pair@_KV?$function@$$A6AXPEBD@Z@blz@@@blz@@";
const char s_preSwapTag[] = ".?AU?$pair@_KV?$function@$$A6AXXZ@blz@@@blz@@";

HANDLERVECTOR s_hotfixHandlers;         // DAT_142e208a0
blz::shared_mutex s_hotfixLock;         // DAT_142e208b8
HANDLERVECTOR s_preSwapHandlers;        // DAT_142e208d8
blz::shared_mutex s_preSwapLock;        // DAT_142e208f0
HANDLERVECTOR s_manifestHandlers;       // DAT_142e20910
blz::shared_mutex s_manifestLock;       // DAT_142e20928

// Moves a blz::function: the manager's move, and the source left empty.
void MoveFunction(blz::function_storage* dest, blz::function_storage* source) {
    source->m_manager(dest->m_storage, source->m_storage, 2);
    dest->m_invoke = source->m_invoke;
    dest->m_manager = source->m_manager;
    source->m_invoke = nullptr;
    source->m_manager = &blz::function_empty_manager;
}

// ref: FUN_1402d4500 (hot-fix), FUN_1402d46e0 (manifest), FUN_1402d48c0 (pre-swap)
void ReserveHandlers(HANDLERVECTOR* vector, uint64_t count, const char* tag) {
    uint64_t capacity = vector->m_capacity;
    if ((capacity & ~NOT_OWNED) >= count) {
        return;
    }
    uint64_t grown = ((capacity >> 1) & 0x3FFFFFFFFFFFFFFFull) + (capacity & ~NOT_OWNED);
    if (grown < count) {
        grown = count;
    }
    vector->m_capacity = ((capacity ^ grown) & NOT_OWNED) ^ grown;
    auto data = static_cast<HANDLERENTRY*>(SMemAlloc(grown * sizeof(HANDLERENTRY), 0xB, tag, SStrHashFnv1a(tag), -6, 0));
    for (uint64_t i = 0; i < vector->m_size; i++) {
        data[i].m_key = vector->m_data[i].m_key;
        MoveFunction(&data[i].m_function, &vector->m_data[i].m_function);
    }
    for (uint64_t i = 0; i < vector->m_size; i++) {
        blz::function_storage* function = &vector->m_data[i].m_function;
        if (function->m_manager != &blz::function_empty_manager) {
            function->m_manager(function->m_storage, nullptr, 0);
        }
    }
    if ((vector->m_capacity & NOT_OWNED) == 0) {
        SMemFree(vector->m_data, tag, -6, 0);
    }
    vector->m_data = data;
    vector->m_capacity &= ~NOT_OWNED;
}

// push_back of a moved-in pair under the lock. (The reference also handles a source inside
// the vector, which a stack temporary never is.)
// ref: FUN_1402d48c0
void AddHandler(HANDLERVECTOR* vector, blz::shared_mutex* lock, const char* tag, blz::function_storage* handler,
    uint64_t key) {
    lock->lock();
    HANDLERENTRY entry;
    entry.m_key = key;
    MoveFunction(&entry.m_function, handler);
    if (vector->m_size == (vector->m_capacity & ~NOT_OWNED)) {
        ReserveHandlers(vector, (vector->m_capacity & ~NOT_OWNED) + 1, tag);
    }
    HANDLERENTRY* slot = vector->m_data + vector->m_size;
    if (slot != nullptr) {
        slot->m_key = entry.m_key;
        MoveFunction(&slot->m_function, &entry.m_function);
    }
    vector->m_size++;
    if (entry.m_function.m_manager != &blz::function_empty_manager) {
        entry.m_function.m_manager(entry.m_function.m_storage, nullptr, 0);
    }
    lock->unlock();
}

// Removes every handler registered under `key` (std::remove_if, then the tail destroyed).
// ref: FUN_1402d4120
void RemoveHandlers(HANDLERVECTOR* vector, blz::shared_mutex* lock, uint64_t key) {
    lock->lock();
    HANDLERENTRY* end = vector->m_data + vector->m_size;
    HANDLERENTRY* write = vector->m_data;
    while (write != end && write->m_key != key) {
        write++;
    }
    if (write != end) {
        for (HANDLERENTRY* read = write + 1; read != end; read++) {
            if (read->m_key != key) {
                write->m_key = read->m_key;
                write->m_function.m_manager(write->m_function.m_storage, nullptr, 0);
                MoveFunction(&write->m_function, &read->m_function);
                write++;
            }
        }
        for (HANDLERENTRY* dead = write; dead != end; dead++) {
            dead->m_function.m_manager(dead->m_function.m_storage, nullptr, 0);
        }
        vector->m_size = static_cast<uint64_t>(write - vector->m_data);
    }
    lock->unlock();
}

}

// ref: FUN_1402cc6a0
void CASAddHotfixHandler(blz::function_storage* handler, uint64_t key) {
    AddHandler(&s_hotfixHandlers, &s_hotfixLock, s_hotfixTag, handler, key);
}

// ref: FUN_1402cc840
void CASAddManifestHandler(blz::function_storage* handler, uint64_t key) {
    AddHandler(&s_manifestHandlers, &s_manifestLock, s_manifestTag, handler, key);
}

// ref: FUN_1402cc9e0
void CASAddPreSwapHandler(blz::function_storage* handler, uint64_t key) {
    AddHandler(&s_preSwapHandlers, &s_preSwapLock, s_preSwapTag, handler, key);
}

// ref: FUN_1402d0a10
void CASRemoveManifestHandlers(uint64_t key) {
    RemoveHandlers(&s_manifestHandlers, &s_manifestLock, key);
}

// ref: FUN_1402d0a20
void CASRemovePreSwapHandlers(uint64_t key) {
    RemoveHandlers(&s_preSwapHandlers, &s_preSwapLock, key);
}

// ref: FUN_1402ccd00
void CASSetLogEnabled(bool enabled) {
    s_tactLogEnabled = enabled;
}

// ref: FUN_1402cee30
void CASSetKeyCallback(CASKEYCALLBACK callback) {
    s_keyCallback = callback;
}

// ref: FUN_1402cec40
bool CASInitialize(const char** dataPath, CStatus* status) {
    if (g_localManifest != nullptr) {
        return true;
    }
    const char* path = *dataPath;
    if (path == nullptr || *path == '\0') {
        path = "../Data";
        *dataPath = "../Data";  // DAT_142630cf0
    }
    CopyBounded(s_dataPath, path, 0x103);
    if (!OsCreateDirectory(*dataPath, true, false)) {
        char error[0x400];
        OsGetLastErrorText(error, sizeof(error));
        status->Add(3, "CASInitialize() failed to create folder %s: %s\n", *dataPath, error);
        return false;
    }

    auto manifest = static_cast<LOCALMANIFEST*>(SMemAlloc(sizeof(LOCALMANIFEST), 10, __FILE__, SStrHashFnv1a(__FILE__), 0x90e, 0));
    manifest->Construct();
    g_localManifest = manifest;
    if (s_tactLogEnabled) {
        OsCreateDirectory("Logs", false, false);
        SLogCreate("Logs\\Tact.log", 0, &s_tactLog);
        SLogWrite(s_tactLog, "Initialze");
    }
    blz::log_register(&LogSink);
    if (!s_initialized) {
        s_mainThread = OsGetCurrentThreadId();
        s_initialized = true;
    }
    return true;
}

// ref: FUN_1402d1420
bool CASCreateInstance(CASINSTANCE* instance, const char* storagePath, tact::BUILDINFOENTRY* entry, CStatus* status,
    int32_t streaming, void* keyFunction, void* keyContext, CASSTATS* stats) {
    if (instance->m_tact != nullptr) {
        status->Add(2, "Tact instance already exists. Aborting.");
        return false;
    }
    if (stats == nullptr) {
        stats = static_cast<CASSTATS*>(SMemAlloc(sizeof(CASSTATS), 9, __FILE__, SStrHashFnv1a(__FILE__), 800, 0));
        stats->m_active = 0;
        stats->m_start = 0;
        stats->m_bytes = 0;
        stats->m_smoothing = 0.7f;
        stats->m_rate = 0.0f;
    }
    instance->m_stats = stats;

    tact::INSTANCEPARAMS params = {};
    params.m_mode = 0;
    params.m_08 = ~uint64_t(0);
    params.m_storagePath = storagePath;
    std::memcpy(params.m_buildKey, entry->m_buildKey, 16);
    params.m_buildConfigPath = nullptr;
    std::memcpy(params.m_cdnKey, entry->m_cdnKey, 16);
    params.m_cdnPath = entry->m_cdnPath;
    params.m_cdnHosts = entry->m_cdnServers;
    params.m_cdnHostCount = entry->m_cdnServerCount;
    params.m_armadilloKey = entry->m_armadillo;
    params.m_60 = 200;
    params.m_64 = 1;
    params.m_65 = 1;
    params.m_68 = 0x10;
    params.m_70 = 0x10;
    params.m_74 = 0x20;
    params.m_78 = 0x20000;
    params.m_streaming = streaming;
    params.m_affinity = ~uint64_t(0);
    params.m_priority = 4;
    params.m_threads = 1;
    params.m_a0 = ~uint64_t(0);
    params.m_a8 = 4;
    params.m_b0 = 0.75;  // DAT_142631a40
    params.m_c8 = 1;
    params.m_f0 = reinterpret_cast<void*>(&StatsCallback);
    params.m_f8 = stats;
    params.m_140 = keyFunction;
    params.m_148 = keyContext;
    params.m_150 = reinterpret_cast<void*>(&CounterHook);
    params.m_158 = &s_downloadFailures;
    params.m_loadEncodingTable = 1;
    params.m_1d8 = 1;
    params.m_1d9 = 0;

    uint8_t error = 0;
    instance->m_tact = tact::Instance::Create(&params, &error);
    if (instance->m_tact == nullptr) {
        status->Add(3, "Tact instance creation failed. Error (%d): %s", error, tact::ErrorString(error));
        return false;
    }
    instance->m_storagePath = storagePath;
    instance->m_noStreaming = streaming == 1;
    instance->m_noCdn = entry->m_cdnServers == nullptr;
    return true;
}

// ref: FUN_1402d2cd0
bool CASReadFile(CASINSTANCE* instance, const char* name, uint8_t** data, uint32_t* size, uint32_t flags,
    int32_t allocFlags) {
    if ((flags & 8) != 0) {
        uint8_t resident = tact::InstanceIsResident(instance->m_tact, name);
        if (resident != 0) {
            t_lastError.m_kind = 0;
            t_lastError.m_tactError = resident;
            return false;
        }
    }
    uint32_t extra = (flags >> 1) & 1;
    tact::FILESTAT stat = {};
    uint8_t error = tact::InstanceStatByName(instance->m_tact, name, &stat, 1);
    t_lastError.m_tactError = error;
    t_lastError.m_kind = 0;
    if (error != 0 || stat.m_key.m_csize == 0) {
        t_lastError.m_tactError = 8;
        return false;
    }

    uint32_t csize = stat.m_key.m_csize;
    *data = static_cast<uint8_t*>(SMemAlloc(uint64_t(csize) + extra, allocFlags, __FILE__, SStrHashFnv1a(__FILE__), 0x3fa, 0));
    uint32_t read = 0;
    uint8_t result = 0;
    if (tact::ImmediateReadByName(instance->m_tact, name, *data, 0, csize, &read, 0) != 0) {
        uint64_t id = tact::InstanceStartReadByName(instance->m_tact, name, *data, csize, nullptr, nullptr, 1, 0);
        result = tact::InstanceGetOpResult(instance->m_tact, id, reinterpret_cast<int32_t*>(&read));
    }
    t_lastError.m_tactError = result;
    t_lastError.m_kind = 0;
    if (result == 0) {
        std::memset(*data + csize, 0, extra);
        if (size != nullptr) {
            *size = read;
        }
        return true;
    }
    SMemFree(*data, __FILE__, 0x408, 0);
    *data = nullptr;
    return false;
}

// ref: FUN_1402d2c00
bool CASLoadManifest(const uint8_t* const* data, uint64_t size, const MANIFESTLOCALE* locale, CStatus* status) {
    auto manifest = static_cast<CASMANIFEST*>(SMemAlloc(sizeof(CASMANIFEST), 10, __FILE__, SStrHashFnv1a(__FILE__), 0x6aa, 0));
    manifest->Construct();
    g_casManifest = manifest;
    bool loaded = manifest->Load(data, size, locale, status);
    if (!loaded) {
        status->Add(3, "Unable to read build manifest into memory. \n");
        CASShutdown();
    }
    return loaded;
}

// ref: FUN_1402ce070
bool CASOpen(uint32_t patch, const MANIFESTLOCALE* locale, tact::BUILDINFOENTRY* entry, uint32_t a4,
    bool manifestOnly, CStatus* status) {
    uint32_t size = 0;
    uint8_t* root = nullptr;  // a CASTypes.h buffer holder
    bool opened = false;
    bool load = manifestOnly;
    if (!manifestOnly) {
        auto instance = static_cast<CASINSTANCE*>(SMemAlloc(sizeof(CASINSTANCE), 10, __FILE__, SStrHashFnv1a(__FILE__), 0x931, 0));
        instance->m_tact = nullptr;
        instance->m_stats = nullptr;
        instance->m_28 = 0;
        instance->m_noStreaming = 0;
        instance->m_noCdn = 0;
        instance->m_storagePath = nullptr;
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&g_casInstance), instance);
        if (CASCreateInstance(instance, s_dataPath, entry, status, 0, reinterpret_cast<void*>(&KeyLookup), &s_keyCallback,
                nullptr)) {
            uint8_t* read = nullptr;
            if (!CASReadFile(instance, ".root", &read, &size, 0, 10)) {
                for (uint32_t retries = 0;;) {
                    if (t_lastError.m_tactError != 7 || ++retries > 3) {
                        if (root != nullptr) {
                            SMemFree(root, s_typesFile, 0xe9, 0);
                        }
                        status->Add(2, "Unable to load manifest for patch %d: %s", patch,
                            tact::ErrorString(t_lastError.m_tactError));
                        root = nullptr;
                        goto fail;
                    }
                    OsSleepNanoseconds(500 * 1000000ll);  // FUN_141238570
                    if (CASReadFile(instance, ".root", &read, &size, 0, 10)) {
                        break;
                    }
                }
            }
            uint8_t* old = root;
            root = read;
            if (old != nullptr) {
                SMemFree(old, s_typesFile, 0xe9, 0);
            }
            std::memcpy(instance->m_buildKey, entry->m_buildKey, 16);
            std::memcpy(instance->m_cdnKey, entry->m_cdnKey, 16);
            s_openA4 = a4;
            load = true;
        }
    }
    if (load) {
        if (CASLoadManifest(&root, size, locale, status)) {
            s_manifestOnly = manifestOnly;
            opened = true;
            goto done;
        }
    }
fail:
    CASShutdown();
done:
    if (root != nullptr) {
        SMemFree(root, s_typesFile, 0xe9, 0);
    }
    return opened;
}

// ref: FUN_1402ce320
bool CASInitFromBuildInfo(uint32_t patch, const char* version, const MANIFESTLOCALE* locale, const char* dataPath,
    const char* localRoot, CStatus* status, blz::string* product, bool offline) {
    tact::INSTALLATIONINFO* info = tact::InstallationInfoRead(".");
    tact::BUILDINFOENTRY* active = info->m_active;
    bool initialized = false;
    if (active == nullptr) {
        status->Add(2, "Unable to initialize CAS from build info file. No active install info entries.\n");
    } else {
        product->assign(active->m_product);
        bool matches = _stricmp(active->m_version, version) == 0;
        if (!matches) {
            char code[0x40];
            CopyBounded(code, version, 0x3f);
            char* dot = std::strrchr(code, '.');
            if (dot != nullptr && dot[1] != '\0') {
                *dot = '\0';
                const char* build = dot + 1;
                static const char* const s_branches[] = { "WOW-%spatch%s_Retail", "WOW-%spatch%s_ClassicRetail",
                    "WOW-%spatch%s_Beta", "WOW-%spatch%s_ClassicBeta", "WOW-%spatch%s_PTR", "WOW-%spatch%s_ClassicPTR" };
                for (const char* format : s_branches) {
                    char name[0x40];
                    SStrPrintf64(name, format, build, code);
                    if (_stricmp(active->m_version, name) == 0) {
                        matches = true;
                        break;
                    }
                }
            }
            if (!matches) {
                status->Add(2, "Unable to initialize CAS from build info file. Code version does not match .build.info.\n");
            }
        }
        if (matches) {
            tact::BUILDINFOENTRY entry = *active;
            if (offline) {
                entry.m_cdnHosts = nullptr;
                entry.m_cdnHostCount = 0;
                entry.m_cdnServers = nullptr;
                entry.m_cdnServerCount = 0;
            }
            const char* path = dataPath;
            if (CASInitialize(&path, status) && CASOpen(patch, locale, &entry, 5, false, status)) {
                if (localRoot != nullptr && *localRoot != '\0') {
                    char full[0x104];
                    OsGetFullPath(localRoot, full, sizeof(full));
                    CopyBounded(s_localRoot, full, 0x103);
                    g_localManifest->SetRoot(full);
                }
                initialized = true;
            }
        }
    }
    tact::InstallationInfoFree(info);
    return initialized;
}

void (*g_casReadHook)(const uint8_t* ckey, uint32_t bytes, void* context);
namespace {
uint8_t s_casReadHookContext[0x10];  // DAT_142ccba60
}

// ref: FUN_1402cb310
void CASFormatKeyName(char* dest, const uint8_t* ckey, uint32_t fileDataId, uint32_t localeFlags, uint32_t contentFlags) {
    uint32_t content = contentFlags & 0xe7fe;
    if (s_openA4 == 2) {
        fileDataId = 0;
        localeFlags = 0;
        content = 0;
    }
    // FUN_1402cbec0: SStrPrintf into 0x36 bytes
    SStrPrintf(dest, 0x36, "%08X%04X:%08X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
        localeFlags, content, fileDataId, ckey[0], ckey[1], ckey[2], ckey[3], ckey[4], ckey[5], ckey[6], ckey[7], ckey[8],
        ckey[9], ckey[10], ckey[11], ckey[12], ckey[13], ckey[14], ckey[15]);
}

// ref: FUN_1402d3730
bool CASFileOpenByKey(CASINSTANCE* instance, const char* name, const uint8_t* ckey, CASFILE** file) {
    tact::FILESTAT stat = {};
    uint8_t error = tact::InstanceStatByName(instance->m_tact, name, &stat, 1);
    t_lastError.m_kind = 0;
    t_lastError.m_tactError = error;
    if (error != 0 || stat.m_key.m_csize == 0) {
        *file = nullptr;
        t_lastError.m_tactError = 6;
        return false;
    }
    InterlockedIncrement(reinterpret_cast<volatile long*>(&instance->m_28));
    auto opened = static_cast<CASFILE*>(SMemAlloc(sizeof(CASFILE), 10, __FILE__, SStrHashFnv1a(__FILE__), 0x422, 0));
    opened->m_handle = nullptr;
    opened->m_type = 2;
    CopyBounded(opened->m_name, name, 0x35);
    opened->m_instance = instance;
    opened->m_position = 0;
    std::memcpy(opened->m_ckey, ckey, 16);
    if (opened->m_handle == nullptr) {
        opened->m_handle = tact::FileHandleOpen(instance->m_tact, name, nullptr);
    }
    *file = opened;
    t_lastError.m_kind = 0;
    t_lastError.m_tactError = 0;
    return true;
}

// ref: FUN_1402cfd70
bool CASFileOpenById(int32_t fileDataId, CASFILE** file, uint64_t flags) {
    *file = nullptr;
    t_lastError.m_kind = 1;
    if (fileDataId == 0) {
        t_lastError.m_code = 2;
        return false;
    }
    BuildManifestEntry* entry = g_casManifest->FindById(static_cast<uint32_t>(fileDataId));
    t_lastError.m_kind = 1;
    t_lastError.m_code = entry != nullptr ? 0 : 2;
    if ((flags & 1) != 0) {
        // Found by id, a loose file that fails to open fails the open; found by name, the
        // build's copy is the fallback.
        const char* path = nullptr;
        BuildManifestEntry* fallback = nullptr;
        LocalManifestEntry* local = g_localManifest->FindById(fileDataId);
        if (local != nullptr) {
            path = local->m_path;
        } else if (entry != nullptr) {
            path = g_localManifest->FindPathByName(entry->m_nameHash, nullptr);
            fallback = entry;
        } else {
            return false;
        }
        if (path != nullptr) {
            t_lastError.m_kind = 2;
            void* handle = OsCreateFile(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0);
            if (handle != INVALID_HANDLE_VALUE) {
                auto opened = static_cast<CASFILE*>(SMemAlloc(sizeof(CASFILE), 10, __FILE__, SStrHashFnv1a(__FILE__), 0xb89, 0));
                opened->m_handle = nullptr;
                opened->m_type = 1;
                opened->m_file = handle;
                *file = opened;
                return true;
            }
            entry = fallback;
        }
    }
    if (entry == nullptr) {
        return false;
    }
    t_lastError.m_kind = 0;
    char name[0x40];
    CASFormatKeyName(name, entry->m_ckey, entry->m_fileDataId, entry->m_localeFlags, entry->m_contentFlags);
    return CASFileOpenByKey(g_casInstance, name, entry->m_ckey, file);
}

// The size tact reports for a build file.
// ref: FUN_1402d2630
uint32_t CASFileStatSize(CASINSTANCE* instance, CASFILE* file) {
    tact::FILESTAT stat = {};
    uint8_t error = tact::InstanceStatByName(instance->m_tact, file->m_name, &stat, 1);
    t_lastError.m_kind = 0;
    t_lastError.m_tactError = error;
    if (error != 0) {
        stat.m_key.m_csize = 0;
    }
    return stat.m_key.m_csize;
}

// ref: FUN_1402cdac0
uint64_t CASFileGetSize(CASFILE* file) {
    if (file->m_type == 1) {
        t_lastError.m_kind = 2;
        return OsGetFileSize(file->m_file, 0);
    }
    if (file->m_type != 2) {
        t_lastError.m_kind = 1;
        t_lastError.m_code = 4;
        return 0;
    }
    t_lastError.m_kind = 0;
    return CASFileStatSize(file->m_instance, file);
}

// Moves a file's read position (0 from the start, 1 from the current position, 2 from the
// end) and returns the new one.
// ref: FUN_1402d0a60
int64_t CASFileSetPosition(CASFILE* file, int64_t offset, uint32_t method) {
    if (file->m_type == 1) {
        t_lastError.m_kind = 2;
        return OsSetFilePointer(file->m_file, offset, method);
    }
    if (file->m_type != 2) {
        t_lastError.m_kind = 1;
        t_lastError.m_code = 4;
        return 0;
    }
    t_lastError.m_kind = 0;
    if (method == 0) {
        file->m_position = offset;
    } else if (method == 1) {
        file->m_position += offset;
        return file->m_position;
    } else if (method == 2) {
        uint32_t size = CASFileStatSize(file->m_instance, file);
        file->m_position = size + offset;
        return size + offset;
    }
    return file->m_position;
}

// ref: FUN_1402d3c90
bool CASFileReadAt(CASINSTANCE* instance, CASFILE* file, void* dest, uint64_t offset, uint32_t size, uint32_t* read,
    uint8_t flags, int32_t slot) {
    if ((flags & 8) != 0) {
        uint64_t id = tact::InstanceCheckResidency(instance->m_tact, file->m_name, static_cast<uint32_t>(offset), size);
        uint8_t error = tact::InstanceGetOpResult(instance->m_tact, id, nullptr);
        if (error != 0) {
            t_lastError.m_kind = 0;
            t_lastError.m_tactError = error;
            return false;
        }
    }
    uint64_t id;
    if (dest == nullptr) {
        if (read != nullptr) {
            *read = size;
            read = nullptr;
        }
        id = tact::InstanceMakeSpanResident(instance->m_tact, file->m_name, static_cast<uint32_t>(offset), size, 0,
            nullptr, nullptr);
    } else {
        uint32_t done = 0;
        uint8_t error;
        if (file->m_handle == nullptr) {
            error = tact::ImmediateReadByName(instance->m_tact, file->m_name, dest, static_cast<uint32_t>(offset), size,
                &done, 0);
            if (error == 0) {
                if (read != nullptr) {
                    *read = done;
                }
                t_lastError.m_kind = 0;
                t_lastError.m_tactError = 0;
                return true;
            }
            id = tact::InstanceStartSpanReadByName(instance->m_tact, file->m_name, dest, static_cast<uint32_t>(offset),
                size, nullptr, nullptr, 1, 8);
        } else {
            error = tact::ImmediateReadByHandle(file->m_handle, dest, static_cast<uint32_t>(offset), size, &done, 0);
            if (error == 0) {
                if (read != nullptr) {
                    *read = done;
                }
                t_lastError.m_kind = 0;
                t_lastError.m_tactError = 0;
                return true;
            }
            id = tact::InstanceStartSpanReadByHandle(file->m_handle, dest, static_cast<uint32_t>(offset), size, nullptr,
                nullptr, 1, 8);
        }
    }
    if (id == 0) {
        t_lastError.m_kind = 0;
        t_lastError.m_tactError = 3;
        return false;
    }
    if ((flags & 0x10) != 0 && slot != -1) {
        instance->m_ops[slot] = id;
    }
    uint8_t error = tact::InstanceGetOpResult(instance->m_tact, id, reinterpret_cast<int32_t*>(read));
    t_lastError.m_kind = 0;
    t_lastError.m_tactError = error;
    return error == 0 || error == 0xe;
}

// ref: FUN_1402d0690
bool CASFileReadEx(CASFILE* file, void* dest, uint32_t size, uint32_t* read, uint32_t flags, int32_t slot) {
    if (size == 0) {
        *read = 0;
        return true;
    }
    if (file->m_type == 1) {
        t_lastError.m_kind = 2;
        return OsReadFile(file->m_file, dest, size, read);
    }
    if (file->m_type != 2) {
        t_lastError.m_kind = 1;
        t_lastError.m_code = 4;
        return false;
    }
    CASINSTANCE* instance = file->m_instance;
    t_lastError.m_kind = 0;
    bool ok = CASFileReadAt(instance, file, dest, file->m_position, size, read, static_cast<uint8_t>(flags), slot);
    file->m_position += *read;
    if (g_casReadHook != nullptr) {
        g_casReadHook(file->m_ckey, *read, s_casReadHookContext);
    }
    return ok;
}

// ref: FUN_1402d0660
bool CASFileRead(CASFILE* file, void* dest, uint32_t size) {
    uint32_t read;
    return CASFileReadEx(file, dest, size, &read, 0, -1);
}

// ref: FUN_1402ccc60
void CASFileClose(CASFILE** file) {
    CASFILE* closing = *file;
    if (closing->m_type == 1) {
        OsCloseFile(closing->m_file);
    } else if (closing->m_type == 2) {
        CASINSTANCE* instance = closing->m_instance;
        long left = InterlockedDecrement(reinterpret_cast<volatile long*>(&instance->m_28));
        if (left < 0 && instance != g_casInstance && s_patchInstances) {
            // DAT_142e20878: the patch path's wake-up, not ported with it.
            FROST_UNIMPLEMENTED();
        }
        closing->m_type = 0;
        tact::FileHandleClose(closing->m_handle);
        closing->m_handle = nullptr;
    }
    operator delete(closing, sizeof(CASFILE));
    *file = nullptr;
}

// ref: FUN_1402d2330
bool CASIdIsKnown(int32_t fileDataId) {
    BuildManifestEntry* entry = g_casManifest->FindById(static_cast<uint32_t>(fileDataId));
    t_lastError.m_kind = 1;
    t_lastError.m_code = entry != nullptr ? 0 : 2;
    return entry != nullptr;
}

// ref: FUN_1402ccd10
bool CASFileExists(int32_t fileDataId) {
    LOCALMANIFEST* local = g_localManifest;
    if (fileDataId == 0) {
        return false;
    }
    t_lastError.m_kind = 1;
    if (local->FindById(fileDataId) != nullptr) {
        return true;
    }
    return CASIdIsKnown(fileDataId);
}

// Reads a whole file by FileDataID into a new Storm block: from the local manifest's loose
// file when bit 0 of `flags` allows one, else from storage.
// ref: FUN_1402cf090
bool CASFileLoadById(int32_t fileDataId, uint8_t** data, uint32_t* size, uint64_t flags, int32_t allocFlags) {
    t_lastError.m_kind = 1;
    BuildManifestEntry* entry = g_casManifest->FindById(static_cast<uint32_t>(fileDataId));
    t_lastError.m_kind = 2;
    t_lastError.m_code = entry != nullptr ? 0 : 2;
    if ((flags & 1) != 0) {
        const char* path = nullptr;
        LocalManifestEntry* local = g_localManifest->FindById(fileDataId);
        if (local != nullptr) {
            path = local->m_path;
        } else if (entry != nullptr) {
            path = g_localManifest->FindPathByName(entry->m_nameHash, nullptr);
        }
        if (path != nullptr) {
            return OsFileReadAll(path, data, size, static_cast<int32_t>((flags >> 1) & 1), allocFlags);
        }
    }
    if (entry == nullptr) {
        t_lastError.m_kind = 1;
        t_lastError.m_code = 2;
        return false;
    }
    uint32_t localSize = 0;
    t_lastError.m_kind = 0;
    uint32_t* sizeOut = size != nullptr ? size : &localSize;
    char name[0x40];
    CASFormatKeyName(name, entry->m_ckey, entry->m_fileDataId, entry->m_localeFlags, entry->m_contentFlags);
    bool result = CASReadFile(g_casInstance, name, data, sizeOut, static_cast<uint32_t>(flags), allocFlags);
    if (g_casReadHook != nullptr) {
        g_casReadHook(entry->m_ckey, *sizeOut, s_casReadHookContext);
    }
    return result;
}

// CASFileLoadById's form that hands the block back in a holder (`*holder`, null when nothing was
// read) and returns the holder; the size goes to `size` when it is not null. Translated callers
// (the preload of FUN_141297eb0) read the holder: stubbed, it left them stack garbage
// (2026-10-15).
// ref: FUN_1402ceef0
uint8_t** CASFileLoadByIdHolder(uint8_t** holder, int32_t fileDataId, uint32_t* size, uint32_t flags,
    int32_t allocFlags) {
    uint8_t* data = nullptr;
    t_lastError.m_kind = 1;
    BuildManifestEntry* entry = g_casManifest->FindById(static_cast<uint32_t>(fileDataId));
    t_lastError.m_kind = 2;
    t_lastError.m_code = entry != nullptr ? 0 : 2;
    const char* path = nullptr;
    if ((flags & 1) != 0) {
        if (LocalManifestEntry* local = g_localManifest->FindById(fileDataId)) {
            path = local->m_path;
        } else if (entry != nullptr) {
            path = g_localManifest->FindPathByName(entry->m_nameHash, nullptr);
        }
    }
    if (path != nullptr) {
        OsFileReadAll(path, &data, size, static_cast<int32_t>((flags >> 1) & 1), allocFlags);
    } else if (entry == nullptr) {
        t_lastError.m_kind = 1;
        t_lastError.m_code = 2;
    } else {
        uint32_t localSize = 0;
        t_lastError.m_kind = 0;
        uint32_t* sizeOut = size != nullptr ? size : &localSize;
        char name[0x40];
        CASFormatKeyName(name, entry->m_ckey, entry->m_fileDataId, entry->m_localeFlags, entry->m_contentFlags);
        CASReadFile(g_casInstance, name, &data, sizeOut, flags, allocFlags);
        if (g_casReadHook != nullptr) {
            g_casReadHook(entry->m_ckey, *sizeOut, s_casReadHookContext);
        }
    }
    *holder = data;
    return holder;
}

// Reads a whole file by path into a new Storm block: from the local manifest's loose file
// when bit 0 of `flags` allows one, else from storage (never while only the manifest is up).
// ref: FUN_1402cf230
bool CASFileLoadByName(const char* name, uint8_t** data, uint32_t* size, uint64_t flags, int32_t allocFlags) {
    uint64_t hash = LocalManifestNameHash(name);
    t_lastError.m_kind = 2;
    if ((flags & 1) != 0 && g_localManifest != nullptr) {
        int32_t fileDataId = 0;
        if (const char* path = g_localManifest->FindPathByName(hash, &fileDataId)) {
            return OsFileReadAll(path, data, size, static_cast<int32_t>((flags >> 1) & 1), allocFlags);
        }
    }
    bool manifestOnly = s_manifestOnly;
    t_lastError.m_kind = 1;
    if (manifestOnly) {
        t_lastError.m_code = 3;
        return false;
    }
    BuildManifestEntry* entry = g_casManifest->FindByName(hash);
    if (entry == nullptr) {
        t_lastError.m_kind = 1;
        t_lastError.m_code = 2;
        return false;
    }
    t_lastError.m_kind = 0;
    uint32_t localSize = 0;
    uint32_t* sizeOut = size != nullptr ? size : &localSize;
    char key[0x40];
    CASFormatKeyName(key, entry->m_ckey, entry->m_fileDataId, entry->m_localeFlags, entry->m_contentFlags);
    bool result = CASReadFile(g_casInstance, key, data, sizeOut, static_cast<uint32_t>(flags), allocFlags);
    if (g_casReadHook != nullptr) {
        g_casReadHook(entry->m_ckey, *sizeOut, s_casReadHookContext);
    }
    return result;
}

// Whether a named file can be loaded: a local override (flag 1) that is relative or exists on
// disk, else an entry in the build manifest.
// ref: FUN_1402ccd80
bool CASFileExistsByName(const char* name, uint64_t flags) {
    uint64_t hash = LocalManifestNameHash(name);
    if ((flags & 1) != 0 && g_localManifest != nullptr) {
        if (const char* path = g_localManifest->FindPathByName(hash, nullptr)) {
            if (OsPathRootLength(path) == 0 || OsFileExists(path)) {
                return true;
            }
        }
    }
    bool manifestOnly = s_manifestOnly;
    t_lastError.m_kind = 1;
    if (manifestOnly) {
        t_lastError.m_code = 3;
    }
    return g_casManifest->FindByName(hash) != nullptr;
}

// Whether the keys the file named `name` is encrypted with are known, over [offset, offset +
// size): 1 when they are (or none are needed), 0 when one is missing, 0x2100 when the file
// cannot be looked at. `check` decides each key when set, else the key callback.
// ref: FUN_1402d2460
uint64_t CASInstanceCheckKeys(CASINSTANCE* instance, const char* name, blz::function_storage* check, uint32_t offset,
    uint32_t size) {
    if (instance->m_tact == nullptr) {
        t_lastError.m_kind = 0;
        t_lastError.m_tactError = 1;
        return 0x2100;
    }
    tact::FILESTAT stat = {};
    uint8_t error = tact::InstanceStatByName(instance->m_tact, name, &stat, 1);
    if (error != 0) {
        t_lastError.m_kind = 0;
        t_lastError.m_tactError = error;
        return 0x2100;
    }
    uint64_t* keys = nullptr;
    uint32_t count = 0;
    error = tact::ESpecGetKeyNames(stat.m_key.m_eSpec, stat.m_key.m_csize, &keys, &count, offset, size);
    uint64_t result;
    if (error != 0) {
        result = 0;
        t_lastError.m_kind = 0;
        t_lastError.m_tactError = error;
    } else {
        result = 1;
        for (uint32_t i = 0; i < count; i++) {
            bool known;
            if (check->m_invoke == nullptr) {
                if (s_keyCallback == nullptr) {
                    known = false;
                } else {
                    uint64_t keyName = keys[i];
                    uint8_t key[0x10];
                    known = s_keyCallback(&keyName, key, 0x10) != 0;
                }
            } else {
                uint64_t keyName = keys[i];
                known = reinterpret_cast<bool (*)(uint64_t*, void*)>(check->m_invoke)(&keyName, check->m_storage);
            }
            if (!known) {
                result = 0;
                t_lastError.m_kind = 0;
                t_lastError.m_tactError = 0xd;
                break;
            }
        }
    }
    // FUN_1420d7bf0
    tact::ESpecFreeBlocks(reinterpret_cast<tact::ESPECBLOCK*>(keys));
    return result;
}

// ref: FUN_1402cd520
uint64_t CASFileCheckKeysById(int32_t fileDataId, blz::function_storage* check, uint32_t offset, uint32_t size) {
    if (fileDataId < 0) {
        return 1;
    }
    BuildManifestEntry* entry = g_casManifest->FindById(static_cast<uint32_t>(fileDataId));
    if (entry == nullptr) {
        t_lastError.m_kind = 1;
        t_lastError.m_code = 2;
        return 0;
    }
    t_lastError.m_kind = 0;
    char name[0x40];
    CASFormatKeyName(name, entry->m_ckey, entry->m_fileDataId, entry->m_localeFlags, entry->m_contentFlags);
    return CASInstanceCheckKeys(g_casInstance, name, check, offset, size);
}

// ref: FUN_1402cd660
uint64_t CASFileCheckKeysByName(const char* path, blz::function_storage* check, uint32_t offset, uint32_t size,
    uint32_t flags) {
    (void)flags;  // passed on to the name lookup as (flags >> 5) & 1
    BuildManifestEntry* entry = g_casManifest->FindByName(LocalManifestNameHash(path));
    if (entry == nullptr) {
        t_lastError.m_kind = 1;
        t_lastError.m_code = 2;
        return 0x2100;
    }
    t_lastError.m_kind = 0;
    char name[0x40];
    CASFormatKeyName(name, entry->m_ckey, entry->m_fileDataId, entry->m_localeFlags, entry->m_contentFlags);
    return CASInstanceCheckKeys(g_casInstance, name, check, offset, size);
}

// ref: FUN_1402cda40
int32_t CASFileGetIdByName(const char* name, uint32_t flags) {
    if ((flags & 1) != 0 && g_localManifest != nullptr) {
        int32_t fileDataId = g_localManifest->FindIdByName(name);
        if (fileDataId != 0) {
            return fileDataId;
        }
    }
    uint64_t hash = LocalManifestNameHash(name);
    if (g_casManifest != nullptr) {
        BuildManifestEntry* entry = g_casManifest->FindByName(hash);
        if (entry != nullptr) {
            return static_cast<int32_t>(entry->m_fileDataId);
        }
    }
    return 0;
}

// ref: FUN_1402cdfc0
bool CASFileGetContentKeyById(int32_t fileDataId, uint8_t* key) {
    if (g_casManifest == nullptr) {
        return false;
    }
    BuildManifestEntry* entry = g_casManifest->FindById(static_cast<uint32_t>(fileDataId));
    if (entry == nullptr) {
        return false;
    }
    std::memcpy(key, entry->m_ckey, 16);
    return true;
}

// ref: FUN_1402ce000
bool CASFileGetContentKeyByName(const char* name, uint8_t* key, uint32_t flags) {
    (void)flags;
    if (g_casManifest == nullptr) {
        return false;
    }
    BuildManifestEntry* entry = g_casManifest->FindByName(LocalManifestNameHash(name));
    if (entry == nullptr) {
        return false;
    }
    std::memcpy(key, entry->m_ckey, 16);
    return true;
}

// Frees what CASFileLoadById / CASFileLoadByName returned.
// ref: FUN_1402d0f80
void CASFileFree(void* data) {
    SMemFree(data, "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\CAS\\CAS.cpp", 0xD99, 0);
}

// ref: FUN_1402cd230
uint8_t CASFileGetResidency(int32_t fileDataId, uint64_t size, uint32_t a3) {
    if (fileDataId < 0) {
        return 2;
    }
    BuildManifestEntry* entry = g_casManifest->FindById(static_cast<uint32_t>(fileDataId));
    if (entry == nullptr) {
        return 0;
    }
    char name[0x40];
    CASFormatKeyName(name, entry->m_ckey, entry->m_fileDataId, entry->m_localeFlags, entry->m_contentFlags);
    if (size >= 0x100000000ull) {
        return 0;
    }
    CASINSTANCE* instance = g_casInstance;
    uint64_t id = tact::InstanceCheckResidency(instance->m_tact, name, static_cast<uint32_t>(size), a3);
    uint8_t error = tact::InstanceGetOpResult(instance->m_tact, id, nullptr);
    t_lastError.m_kind = 0;
    t_lastError.m_tactError = error;
    if (error == 0) {
        return 2;
    }
    return error == 7;
}

// Whether a span of a build file is in local storage: 2 resident, 1 not (tact's 7), 0 unknown
// or failed.
// ref: FUN_1402d23e0
uint8_t CASInstanceGetSpanResidency(CASINSTANCE* instance, const char* name, uint64_t offset, uint64_t size) {
    if (offset >= 0x100000000ull || size >= 0x100000000ull) {
        return 0;
    }
    uint64_t id = tact::InstanceCheckResidency(instance->m_tact, name, static_cast<uint32_t>(offset),
        static_cast<uint32_t>(size));
    uint8_t error = tact::InstanceGetOpResult(instance->m_tact, id, nullptr);
    t_lastError.m_kind = 0;
    t_lastError.m_tactError = error;
    if (error == 0) {
        return 2;
    }
    if (error == 7) {
        return 1;
    }
    return 0;
}

// A loose file is always resident.
// ref: FUN_1402cd330
uint8_t CASFileGetSpanResidency(CASFILE* file, uint64_t offset, uint32_t size) {
    int32_t type = file->m_type;
    if (type != 1) {
        if (type == 2) {
            return CASInstanceGetSpanResidency(g_casInstance, file->m_name, offset, size);
        }
        if (type != 3) {
            return 0;
        }
    }
    return 2;
}

// How much of a file is resident, in fifths (at least 16 KB a step).
// ref: FUN_1402cd040
void CASFileGetResidentBytes(CASFILE* file, uint64_t* resident, uint64_t* total) {
    *total = CASFileGetSize(file);
    *resident = 0;
    uint64_t size = *total;
    uint32_t step = 0x4000;
    if (size / 5 > 0x3fff) {
        step = static_cast<uint32_t>(size / 5);
    }
    step++;
    for (uint64_t offset = 0; offset < size; offset += step) {
        if (size <= step + offset) {
            step = static_cast<uint32_t>(size) - static_cast<uint32_t>(offset);
        }
        int32_t type = file->m_type;
        if (type == 1 || type == 3
            || (type == 2 && CASInstanceGetSpanResidency(g_casInstance, file->m_name, offset, step) == 2)) {
            *resident += step;
        }
        size = *total;
    }
}

// Whether a named file is resident: a build file through tact; with only the manifest loaded,
// a loose file present (or a relative path) counts, else whether the build knows the name.
// ref: FUN_1402cd370
uint8_t CASGetResidencyByName(const char* name, uint64_t offset, uint32_t size, uint64_t flags) {
    if (!s_manifestOnly) {
        uint64_t hash = LocalManifestNameHash(name);
        BuildManifestEntry* entry = g_casManifest->FindByName(hash);
        (void)flags;    // bit 5 goes to the manifest lookup, which takes only the hash
        if (entry == nullptr) {
            return 0;
        }
        char key[0x40];
        CASFormatKeyName(key, entry->m_ckey, entry->m_fileDataId, entry->m_localeFlags, entry->m_contentFlags);
        if (offset >= 0x100000000ull) {
            return 0;
        }
        CASINSTANCE* instance = g_casInstance;
        uint64_t id = tact::InstanceCheckResidency(instance->m_tact, key, static_cast<uint32_t>(offset), size);
        uint8_t error = tact::InstanceGetOpResult(instance->m_tact, id, nullptr);
        t_lastError.m_kind = 0;
        t_lastError.m_tactError = error;
        if (error == 0) {
            return 2;
        }
        return error == 7;
    }
    uint64_t hash = LocalManifestNameHash(name);
    if (g_localManifest != nullptr) {
        if (const char* path = g_localManifest->FindPathByName(hash, nullptr)) {
            // FUN_140213e40: a relative path
            if (OsPathRootLength(path) == 0 || OsFileExists(path)) {
                return 2;
            }
        }
    }
    bool manifestOnly = s_manifestOnly;
    t_lastError.m_kind = 1;
    if (manifestOnly) {
        t_lastError.m_code = 3;
    }
    return g_casManifest->FindByName(hash) != nullptr ? 2 : 1;
}

// The residency of an encoding key's span, for the background download.
// ref: FUN_1402cd180
uint8_t CASGetResidencyByKey(const uint8_t* ckey, uint32_t fileDataId, uint32_t localeFlags, uint32_t contentFlags) {
    t_lastError.m_kind = 0;
    char name[0x40];
    CASFormatKeyName(name, ckey, fileDataId, localeFlags, contentFlags);
    CASINSTANCE* instance = g_casInstance;
    uint64_t id = tact::InstanceCheckResidency(instance->m_tact, name, 0, 0);
    uint8_t error = tact::InstanceGetOpResult(instance->m_tact, id, nullptr);
    t_lastError.m_kind = 0;
    t_lastError.m_tactError = error;
    if (error == 0) {
        return 2;
    }
    return error == 7;
}

// Opens a build file by its content key.
// ref: FUN_1402cfcf0
bool CASFileOpenByCKey(const uint8_t* ckey, uint32_t fileDataId, uint32_t localeFlags, uint32_t contentFlags,
    CASFILE** file) {
    t_lastError.m_kind = 0;
    char name[0x40];
    CASFormatKeyName(name, ckey, fileDataId, localeFlags, contentFlags);
    uint8_t key[16];
    std::memcpy(key, ckey, 16);
    return CASFileOpenByKey(g_casInstance, name, key, file);
}

// Opens a file by path: with `flags` 1 a loose file entered under that name wins.
// ref: FUN_1402cffe0
bool CASFileOpenByName(const char* name, CASFILE** file, uint64_t flags) {
    *file = nullptr;
    uint64_t hash = LocalManifestNameHash(name);
    if ((flags & 1) != 0 && g_localManifest != nullptr) {
        int32_t fileDataId = 0;
        if (const char* path = g_localManifest->FindPathByName(hash, &fileDataId)) {
            t_lastError.m_kind = 2;
            void* handle = OsCreateFile(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0);
            if (handle == INVALID_HANDLE_VALUE) {
                return false;
            }
            auto opened = static_cast<CASFILE*>(SMemAlloc(sizeof(CASFILE), 10, __FILE__, SStrHashFnv1a(__FILE__), 0xaed, 0));
            opened->m_handle = nullptr;
            opened->m_type = 1;
            opened->m_file = handle;
            *file = opened;
            return true;
        }
    }
    bool manifestOnly = s_manifestOnly;
    t_lastError.m_kind = 1;
    if (manifestOnly) {
        *file = nullptr;
        t_lastError.m_code = 3;
        return false;
    }
    BuildManifestEntry* entry = g_casManifest != nullptr ? g_casManifest->FindByName(hash) : nullptr;
    if (entry == nullptr) {
        t_lastError.m_kind = 1;
        *file = nullptr;
        t_lastError.m_code = 2;
        return false;
    }
    t_lastError.m_kind = 0;
    char key[0x40];
    CASFormatKeyName(key, entry->m_ckey, entry->m_fileDataId, entry->m_localeFlags, entry->m_contentFlags);
    return CASFileOpenByKey(g_casInstance, key, entry->m_ckey, file);
}

// Reads `size` bytes at `offset` (a null destination makes the span resident instead).
// ref: FUN_1402d07a0
bool CASFileReadAtOffset(CASFILE* file, void* dest, uint32_t offset, uint32_t size, uint32_t* read, uint32_t flags,
    int32_t slot) {
    if (size == 0) {
        if (read != nullptr) {
            *read = 0;
        }
        return true;
    }
    uint32_t local = 0;
    uint32_t* done = read != nullptr ? read : &local;
    if (file->m_type == 1) {
        t_lastError.m_kind = 2;
        return OsReadFileAt(file->m_file, dest, offset, size, done);
    }
    if (file->m_type != 2) {
        t_lastError.m_kind = 1;
        t_lastError.m_code = 4;
        return false;
    }
    t_lastError.m_kind = 0;
    bool ok = CASFileReadAt(file->m_instance, file, dest, offset, size, done, static_cast<uint8_t>(flags), slot);
    if (g_casReadHook != nullptr) {
        g_casReadHook(file->m_ckey, *done, s_casReadHookContext);
    }
    return ok;
}

// Cancels the background read a thread slot holds (while reads are allowed).
// ref: FUN_1402ccbf0
void CASCancelSlot(int32_t slot) {
    CASINSTANCE* instance = g_casInstance;
    if (instance != nullptr && !instance->m_noCdn && !instance->m_noStreaming && instance->m_tact != nullptr) {
        tact::InstanceCancelOp(instance->m_tact, instance->m_ops[slot]);
    }
}

// ref: FUN_1402d0a30
void CASClearSlot(int32_t slot) {
    if (g_casInstance != nullptr) {
        g_casInstance->m_ops[slot] = 0;
    }
}

// Whether the last error was tact's "not available" (not resident).
// ref: FUN_1402cee50
bool CASLastErrorIsNotResident() {
    int32_t kind = t_lastError.m_kind;
    uint32_t code;
    if (kind == 0) {
        code = t_lastError.m_tactError;
    } else {
        return false;
    }
    return code == 7;
}

// Whether the last tact error was a cancel.
// ref: FUN_1402ceec0
bool CASLastErrorWasCanceled() {
    return t_lastError.m_tactError == 2;
}

// The download rate the stats measure, bytes a second.
// ref: FUN_1402cda20
float CASGetBytesPerSecond() {
    return g_casInstance->m_stats->m_rate;
}

// The last error as a class: 0 none, 1 failed, 6 canceled, 7 not resident, 8 tact's 0xb, 9 an
// OS error; the code itself for kinds 1 and 3.
// ref: FUN_1402cdba0
uint32_t CASGetErrorClass() {
    int32_t kind = t_lastError.m_kind;
    if (kind == 0) {
        uint8_t tact = t_lastError.m_tactError;
        if (tact == 0) {
            return 0;
        }
        if (tact == 2) {
            return 6;
        }
        if (tact == 0xb) {
            return 8;
        }
        return tact == 0xd ? 7 : 1;
    }
    if (kind != 1) {
        if (kind == 2) {
            return GetLastError() == 0 ? 0 : 9;
        }
        if (kind != 3) {
            return 1;
        }
    }
    return t_lastError.m_code;
}

// An error as text, by its kind and code (CASFormatLastError for a saved error).
// ref: FUN_1402cde10
char* CASFormatError(int32_t kind, uint32_t code, char* dest, uint64_t size) {
    if (kind == 0) {
        return CopyErrorText(dest, size, tact::ErrorString(static_cast<uint8_t>(code)));
    }
    if (kind == 2) {
        OsGetErrorText(code, dest, static_cast<uint32_t>(size));
        return dest;
    }
    if (kind != 1 && kind != 3) {
        return CopyErrorText(dest, size, "");
    }
    return CopyErrorText(dest, size, code < 0xb ? s_casErrorStrings[code] : "Unknown CAS error");
}

// Residency and placement of several open files at once, for sorting reads by where they lie
// on disk.
// ref: FUN_1402cdf60
bool CASFileGetMetaData(CASFILE** files, uint32_t* residency, uint64_t* containers, uint64_t* offsets, uint64_t count) {
    t_lastError.m_kind = 0;
    // FUN_1402d26d0 asks tact for the files' metadata in one call (FUN_1420d47a0), which is not
    // ported; only the sortDiskReads path comes here.
    (void)files;
    (void)residency;
    (void)containers;
    (void)offsets;
    (void)count;
    FROST_UNIMPLEMENTED();
    return false;
}

// ref: FUN_1402d2380
int32_t CASGetFileDataIdByName(const char* name) {
    CASMANIFEST* manifest = g_casManifest;
    if (manifest == nullptr) {
        return 0;
    }
    BuildManifestEntry* entry = manifest->FindByName(LocalManifestNameHash(name));
    return entry != nullptr ? static_cast<int32_t>(entry->m_fileDataId) : 0;
}

// The in-place update's state machine: a hot-fix instance taking over (state 1), the
// manifest swap (8), the new instance replacing the old (0xb). Nothing creates the update
// instance (DAT_142e20868) -- the patch path is not ported -- so there is never a step.
// ref: FUN_1402cf8d0
void CASPollUpdate() {
    if (s_updateInstance == nullptr) {
        return;
    }
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1402d0b80
void CASShutdown() {
    if (s_patchInstances) {
        // The patch path's instances (DAT_142e20868, DAT_142ccba08) are not ported; nothing sets
        // the flag.
        FROST_UNIMPLEMENTED();
    }
    CASMANIFEST* manifest = g_casManifest;
    if (manifest != nullptr) {
        manifest->Destroy();
        operator delete(manifest, sizeof(CASMANIFEST));
    }
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&g_casManifest), nullptr);
    LOCALMANIFEST* local = g_localManifest;
    if (local != nullptr) {
        local->Destroy();
        operator delete(local, sizeof(LOCALMANIFEST));
    }
    g_localManifest = nullptr;
    if (g_casInstance != nullptr) {
        DestroyInstance(g_casInstance, 1);
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&g_casInstance), nullptr);
    }
    STREAMINGERROR* node = s_streamingErrors.m_prev;
    s_streamingErrors.m_prev = reinterpret_cast<STREAMINGERROR*>(&s_streamingErrors);
    s_streamingErrors.m_next = reinterpret_cast<STREAMINGERROR*>(&s_streamingErrors);
    s_streamingErrors.m_count = 0;
    while (node != reinterpret_cast<STREAMINGERROR*>(&s_streamingErrors)) {
        STREAMINGERROR* next = node->m_prev;
        SMemFree(node, s_errorNodeTag, -6, 0);
        node = next;
    }
    if (s_tactLog != nullptr) {
        SLogWrite(s_tactLog, "Shutdown");
        SLogClose(s_tactLog);
        s_tactLog = nullptr;
    }
}

}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142ccba48 = (void*)&cas::s_openA4;
extern "C" void* const gen_p_142ccba60 = (void*)&cas::s_casReadHookContext;
extern "C" void* const gen_p_142e20868 = (void*)&cas::s_updateInstance;
extern "C" void* const gen_p_142e20881 = (void*)&cas::s_patchInstances;
extern "C" void* const gen_p_142e208a0 = (void*)&cas::s_hotfixHandlers;
extern "C" void* const gen_p_142e208b8 = (void*)&cas::s_hotfixLock;
extern "C" void* const gen_p_142e208d8 = (void*)&cas::s_preSwapHandlers;
extern "C" void* const gen_p_142e208f0 = (void*)&cas::s_preSwapLock;
extern "C" void* const gen_p_142e20910 = (void*)&cas::s_manifestHandlers;
extern "C" void* const gen_p_142e20928 = (void*)&cas::s_manifestLock;
extern "C" void* const gen_p_142e20948 = (void*)&cas::s_streamingLock;
extern "C" void* const gen_p_142e20950 = (void*)&cas::s_logLock;
extern "C" void* const gen_p_142e20958 = (void*)&cas::s_errorIndex;
extern "C" void* const gen_p_142e2095c = (void*)&cas::s_warningIndex;
extern "C" void* const gen_p_142e20960 = (void*)&cas::s_errors;
extern "C" void* const gen_p_142e22f60 = (void*)&cas::s_warnings;
extern "C" void* const gen_p_142e25560 = (void*)&cas::s_keyCallback;
extern "C" void* const gen_p_142e25580 = (void*)&cas::s_localRoot;
extern "C" void* const gen_p_142e25688 = (void*)&cas::s_tactLog;
// ---- end of generated globals ----
