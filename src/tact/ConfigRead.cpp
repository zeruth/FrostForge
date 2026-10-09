#include "tact/ConfigRead.hpp"
#include "aio/Aio.hpp"
#include "tact/ConfigReader.hpp"
#include "tact/DownloadQueue.hpp"
#include "tact/Instance.hpp"
#include "blz/AllocString.hpp"
#include "blz/Log.hpp"
#include "blz/Md5.hpp"
#include "os/Os.hpp"
#include "util/Unimplemented.hpp"
#include <cstddef>
#include <cstring>
#include <new>

namespace tact {

namespace {

const char s_file[] =
    "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\async_state\\async_config_read_state.cpp";
// PTR_s__build_config_1428e98f0, PTR_s_config_1428e98d8
const char s_defaultPath[] = ".build.config";
const char s_configDir[] = "config";

const uint8_t s_zeroKey[16] = {};

// 0xd0 bytes, vtable 0x1428e7d38.
class ConfigReadState : public AsyncState {
    public:
    uint8_t m_stage;                // 0x40
    blz::alloc_string m_path;       // 0x48
    uint8_t m_key[16];              // 0x68
    uint8_t m_writeBack;            // 0x78
    uint8_t m_allowDownload;        // 0x79
    uint8_t m_type;                 // 0x7a
    uint8_t m_kind;                 // 0x7b: 1 build, 2 CDN, 3 the third configuration
    BUILDCONFIG* m_buildConfig;     // 0x80
    void* m_cdnConfig;              // 0x88
    void* m_90;                     // 0x90
    OSFILE m_file;                  // 0x98
    Array<uint8_t> m_data;          // 0xa0
    uint64_t m_aioKey;              // 0xb8
    uint64_t m_downloadOp;          // 0xc0
    uint32_t m_hostIndex;           // 0xc8
    uint32_t m_retries;             // 0xcc

    ConfigReadState(Instance* instance, uint64_t opId, OPCALLBACK callback, void* context);
    ~ConfigReadState() override;

    void Destroy() override;
    uint8_t GetType() override;
    void OnCancel() override;

    uint8_t OpenAndRead(aio::CALLBACK_ callback);
    bool Parse();
    bool Verify();
    void Step();
};
static_assert(offsetof(ConfigReadState, m_path) == 0x48, "ConfigReadState layout");
static_assert(offsetof(ConfigReadState, m_file) == 0x98, "ConfigReadState layout");
static_assert(sizeof(ConfigReadState) == 0xd0, "ConfigReadState is 0xd0 bytes");

aio::CONTEXT* AioOf(Instance* instance) {
    return static_cast<aio::CONTEXT*>(instance->m_ioContext);
}

// ref: FUN_1420ee550
void StepTask(void* argument) {
    auto state = static_cast<ConfigReadState*>(argument);
    AsyncStateScope scope(state);
    state->Step();
}

// The aio read finished: continue on the scheduler.
// ref: FUN_1420edca0
void OnAioDone(void* context, aio::CONTEXT* aio, uint64_t key) {
    (void)aio;
    (void)key;
    auto state = static_cast<ConfigReadState*>(context);
    int64_t* lock = SpinLockShared(&state->m_lock);
    if (state) {
        state->AddRef();
    }
    state->m_instance->m_scheduler->Post(&StepTask, state, 0);
    if (lock) {
        InterlockedIncrement64(lock);
    }
    state->Release();
}

// "<storage>/config/xx/yy/<key>" (or "config/..." without a storage path); returns the length
// the path needs.
// ref: FUN_142127150
uint32_t ConfigPath(char* out, uint32_t size, const char* storage, const uint8_t key[16]) {
    uint32_t dirLength = static_cast<uint32_t>(std::strlen(s_configDir));
    uint32_t length = dirLength + 1;
    uint32_t total = length;
    if (size != 0) {
        out[0] = '\0';
    }
    if (!storage || storage[0] == '\0') {
        if (size > length) {
            std::memcpy(out, s_configDir, length);
            out[length] = '\0';
            out[dirLength] = '/';
        }
    } else {
        uint32_t storageLength = static_cast<uint32_t>(std::strlen(storage));
        uint32_t extra = storageLength;
        char last = storage[storageLength - 1];
        if (last != '/' && last != '\\') {
            extra = storageLength + 1;
        }
        total = length + extra;
        if (size > total) {
            OsPathJoin(out, size, storage, s_configDir);
            uint32_t joined = static_cast<uint32_t>(std::strlen(out));
            total = joined + 1;
            out[total] = '\0';
            out[joined] = '/';
        }
    }
    uint32_t needed = total + 0x26;
    if (needed < size) {
        // FUN_1420d8fc0: "xx/yy/<32 hex>"
        char* at = out + total;
        uint32_t room = size - total;
        if (room > 0x26) {
            HexEncode(at + 6, room - 6, key, 16);
            at[0] = at[6];
            at[1] = at[7];
            at[2] = '/';
            at[3] = at[8];
            at[4] = at[9];
            at[5] = '/';
        } else if (room != 0) {
            at[0] = '\0';
        }
        OsPathNormalize(out, size, out);
    } else if (size != 0) {
        out[0] = '\0';
    }
    return needed;
}

}

// ref: FUN_1420edad0
ConfigReadState::ConfigReadState(Instance* instance, uint64_t opId, OPCALLBACK callback, void* context)
    : AsyncState(instance, opId, callback, context) {
    this->m_stage = 0;
    this->m_path.Init(GetBufferAllocator());
    std::memset(this->m_key, 0, sizeof(this->m_key));
    this->m_writeBack = 1;
    this->m_allowDownload = 1;
    this->m_type = 3;
    this->m_kind = 0;
    this->m_buildConfig = nullptr;
    this->m_cdnConfig = nullptr;
    this->m_90 = nullptr;
    OsFileInit(&this->m_file);
    this->m_data.Init(GetBufferAllocator());
    this->m_aioKey = 0;
    this->m_downloadOp = 0;
    this->m_hostIndex = 0xFFFFFFFF;
    this->m_retries = 0;
}

// ref: FUN_1420edba0
ConfigReadState::~ConfigReadState() {
    if (this->m_data.m_data) {
        this->m_data.m_allocator->deallocate(this->m_data.m_data);
    }
    OsFileClose(&this->m_file);
    this->m_path.Destroy();
}

// ref: FUN_1420ee5c0
void ConfigReadState::Destroy() {
    this->~ConfigReadState();
    PackageFree(GetBufferAllocator(), this);
}

// ref: FUN_1420ee6a0
uint8_t ConfigReadState::GetType() {
    return 0x12;
}

// ref: FUN_1420ee730
void ConfigReadState::OnCancel() {
    if (this->m_stage != 1) {
        if (this->m_stage == 3) {
            CancelOp(this->m_instance, this->m_downloadOp);
            return;
        }
        if (this->m_stage != 4) {
            return;
        }
    }
    aio::FileCancel(AioOf(this->m_instance), this->m_aioKey);
}

// Opens the file and starts reading all of it.
// ref: FUN_1420eeac0
uint8_t ConfigReadState::OpenAndRead(aio::CALLBACK_ callback) {
    uint8_t result = aio::FileOpen(AioOf(this->m_instance), &this->m_file, this->m_path.c_str(), 2);
    if (result != aio::RESULT_SUCCESS) {
        if (result != aio::RESULT_NOT_FOUND) {
            blz::log(g_logCategory, 4, s_file, 0x132, "Failed to open the config file for reading. Error(%u) : %s", result,
                aio::ResultString(result));
        }
        return AioToTactError(result);
    }
    this->m_stage = 1;
    uint32_t size = static_cast<uint32_t>(OsFileGetSize(&this->m_file));
    if (this->m_data.m_count <= size) {
        if (this->m_data.m_capacity < size) {
            this->m_data.Reserve(size);
        }
        for (uint32_t i = this->m_data.m_count; i < size; i++) {
            this->m_data.m_data[i] = 0;
        }
    }
    this->m_data.m_count = size;
    this->m_aioKey = aio::FileRead(AioOf(this->m_instance), &this->m_file, this->m_data.m_data, 0, this->m_data.m_count,
        callback, this);
    return this->m_aioKey == 0 ? ERR_UNKNOWN : ERR_SUCCESS;
}

// ref: FUN_1420ee6b0
bool ConfigReadState::Parse() {
    if (this->m_kind == 1) {
        BuildConfigVisitor visitor(this->m_buildConfig);
        return ConfigRead(&visitor, reinterpret_cast<const char*>(this->m_data.m_data), this->m_data.m_count);
    }
    if (this->m_kind == 2 || this->m_kind == 3) {
        // The CDN configuration (0x142128890/0x142129af0) and the third kind
        // (0x142128c60/0x142129bf0) are only read with streaming.
        FROST_UNIMPLEMENTED();
        return false;
    }
    return false;
}

// The data's MD5 is its key.
// ref: FUN_1420eec10
bool ConfigReadState::Verify() {
    blz::md5_context md5;
    blz::md5_init(&md5);
    blz::md5_update(&md5, this->m_data.m_data, this->m_data.m_count);
    uint8_t digest[16];
    blz::md5_final(&md5, digest);
    return std::memcmp(digest, this->m_key, 16) == 0;
}

// Stage 1 checks and parses what was read; a missing or invalid file is downloaded (stages 2
// and 3) and written back (stage 4) when streaming allows; stage 5 is done.
// ref: FUN_1420ede10
void ConfigReadState::Step() {
    if (this->m_cancelled && this->m_stage == 2) {
        this->Complete(ERR_CANCELLED);
        return;
    }
    uint8_t error = ERR_SUCCESS;
    if (this->m_stage == 1) {
        uint32_t transferred = 0;
        uint8_t result = aio::FileResult(AioOf(this->m_instance), this->m_aioKey, &transferred);
        OsFileClose(&this->m_file);
        this->m_aioKey = 0;
        error = AioToTactError(result);
        if (error == ERR_SUCCESS) {
            bool valid = std::memcmp(this->m_key, s_zeroKey, 16) == 0 || this->Verify();
            if (valid && this->Parse()) {
                this->m_stage = 5;
                goto next;
            }
            error = ERR_UNKNOWN;
        }
        if (DownloadQueueStreaming(this->m_instance) && this->m_allowDownload) {
            this->m_stage = 2;
            goto next;
        }
    } else if (this->m_stage == 2 || this->m_stage == 3 || this->m_stage == 4) {
        // DIVERGED: downloading a missing configuration from the CDN (0x1420ed920,
        // 0x142126f40) and writing it back (aio write 0x142168c50) needs streaming, which
        // FrostLite never enables (docs/stubs.md).
        FROST_UNIMPLEMENTED();
        this->Complete(ERR_NOT_AVAILABLE);
        return;
    } else {
        goto next;
    }
    if (error != ERR_SUCCESS) {
        this->Complete(error);
        return;
    }
next:
    if (this->m_stage == 5) {
        this->Complete(ERR_SUCCESS);
    } else {
        this->m_instance->m_scheduler->Post(&StepTask, this, 0);
    }
}

AsyncState* ConfigReadCreate(void* memory, Instance* instance, uint64_t opId, OPCALLBACK callback, void* context) {
    return new (memory) ConfigReadState(instance, opId, callback, context);
}

// Reads the build configuration from `path`, or by default from the storage's config
// directory under its key.
// ref: FUN_1420ee770
void ConfigReadStart(AsyncState* base, uint32_t type, const char* path, const char* storagePath, BUILDCONFIG* config,
    const uint8_t key[16], uint8_t writeBack, uint8_t flags) {
    auto state = static_cast<ConfigReadState*>(base);
    char buffer[0x30C];
    uint8_t error;
    state->m_type = static_cast<uint8_t>(type);
    if (!path || path[0] == '\0') {
        path = s_defaultPath;
        if (type != 2) {
            if (std::memcmp(key, s_zeroKey, 16) == 0) {
                blz::log(g_logCategory, 4, s_file, 0x3b, "The passed in build config path & key are both invalid.");
                error = ERR_INVALID_PARAMETER;
                state->Complete(error);
                return;
            }
            uint32_t needed = ConfigPath(buffer, sizeof(buffer), storagePath, key);
            if (needed > 0x30B) {
                blz::log(g_logCategory, 4, s_file, 0x44, "Path too long to load build config: %s", storagePath);
                error = ERR_INVALID_PARAMETER;
                state->Complete(error);
                return;
            }
            path = buffer;
        }
    } else if (!OsPathIsAbsolute(path)) {
        OsPathJoin(buffer, sizeof(buffer), storagePath, path);
        OsPathNormalize(buffer, sizeof(buffer), buffer);
        path = buffer;
    }
    state->m_buildConfig = config;
    state->m_kind = 1;
    state->m_path.Assign(path, static_cast<uint32_t>(std::strlen(path)));
    std::memcpy(state->m_key, key, 16);
    state->m_writeBack = writeBack;
    state->m_allowDownload = ~(flags >> 4) & 1;
    error = state->OpenAndRead(&OnAioDone);
    if (error == ERR_SUCCESS) {
        return;
    }
    if (DownloadQueueStreaming(state->m_instance) && state->m_allowDownload) {
        state->m_stage = 2;
        state->m_instance->m_scheduler->Post(&StepTask, state, 0);
        return;
    }
    state->Complete(error);
}

}
