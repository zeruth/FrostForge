#include "tact/Instance.hpp"
#include "aio/Aio.hpp"
#include "tact/Container.hpp"
#include "tact/DownloadQueue.hpp"
#include "tact/DynamicInit.hpp"
#include "tact/FileRead.hpp"
#include "blz/Log.hpp"
#include "os/Os.hpp"
#include "util/Unimplemented.hpp"
#include <cstring>
#include <new>

namespace tact {

namespace {

const char s_instanceFile[] = "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\instance.cpp";
const char s_scratchFile[] = "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\scratch_buffer.cpp";
const char s_bcUtilsFile[] = "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\bc_ext\\bc_utils.cpp";
const char s_noTag[] = "";  // DAT_1426178ba

// The encoding spec a record carries until the configuration names one (PTR_DAT_1428e98f8).
const char s_defaultESpec[] = "b:256K*=z";

// Where the container lives under the storage path (PTR_DAT_1428e98e0).
const char s_containerDir[] = "data";

const uint32_t SCRATCH_PAGE = 0x20000;
const uint32_t SCRATCH_USABLE = 0x1FFF0;

struct WAITSTATE {
    HANDLE m_event;                 // 0x00
    uint8_t m_result;               // 0x08
    int32_t m_detail;               // 0x0c
    OPCALLBACK m_callback;          // 0x10
    void* m_context;                // 0x18
};

// Stands in for a pending operation's callback while someone waits on it: records the result,
// chains to the original callback and wakes the waiter.
// ref: FUN_1420dc400
void WaitCallback(void* context, Instance* instance, uint64_t opId) {
    auto wait = static_cast<WAITSTATE*>(context);
    OPHANDLE handle;
    instance->m_ops.FindShared(&handle, opId);
    AsyncState* state = handle.m_state;
    wait->m_result = state->m_result;
    wait->m_detail = state->GetDetail();
    if (handle.m_lock) {
        InterlockedIncrement64(handle.m_lock);
    }
    state->Release();
    if (wait->m_callback) {
        wait->m_callback(wait->m_context, instance, opId);
    }
    SetEvent(wait->m_event);
}

// blz's sync event (FUN_142366350): a failure to create one is fatal in the reference.
HANDLE SyncEventCreate() {
    return CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

// Hooks WaitCallback in front of a pending operation's own callback, drops the handle and
// blocks until the operation completes.
void WaitPending(OPHANDLE* handle, WAITSTATE* wait) {
    AsyncState* state = handle->m_state;
    wait->m_event = SyncEventCreate();
    wait->m_callback = state->m_callback;
    wait->m_context = state->m_context;
    state->m_callback = &WaitCallback;
    state->m_context = wait;
    if (handle->m_lock) {
        *handle->m_lock = 0;
    }
    state->Release();
    WaitForSingleObject(wait->m_event, INFINITE);
    CloseHandle(wait->m_event);
}

}

// --- CONFIGRECORD / BUILDCONFIG ----------------------------------------------------------

// ref: FUN_1420de760
void CONFIGRECORD::Init() {
    std::memset(this->m_ckey, 0, sizeof(this->m_ckey));
    this->m_csize = 0;
    std::memset(this->m_ekey, 0, sizeof(this->m_ekey));
    this->m_esize = 0;
    this->m_eSpec.Init(GetAllocator());
    this->m_eSpec.Insert(0, s_defaultESpec, static_cast<uint32_t>(std::strlen(s_defaultESpec)));
}

// ref: FUN_1420def90
void CONFIGRECORD::CopyKeyTo(FILEKEY* out) const {
    std::memcpy(out->m_ckey, this->m_ckey, sizeof(out->m_ckey));
    out->m_csize = this->m_csize;
    std::memcpy(out->m_ekey, this->m_ekey, sizeof(out->m_ekey));
    out->m_esize = this->m_esize;
    out->m_eSpecFlags = 0x10;
    out->m_eSpec = this->m_eSpec.c_str();
    out->m_31 = 0;
    out->m_38 = 0;
}

// ref: FUN_1420dea20
void BUILDCONFIG::Init() {
    std::memset(this->m_patchKey, 0, sizeof(this->m_patchKey));
    this->m_patchSize = 0;
    std::memset(this->m_patchConfig, 0, sizeof(this->m_patchConfig));
    std::memset(this->m_rootKey, 0, sizeof(this->m_rootKey));
    for (auto& record : this->m_records) {
        record.Init();
    }
    this->m_keyLayoutIndexBits = 0;
    this->m_keyLayouts.Init(GetAllocator());
    this->m_vfs.Init(GetAllocator());
    this->m_partialPriority.Init(GetAllocator());
    this->m_buildLines.Init(GetAllocator());
    this->m_250 = 0;
}

// ref: FUN_1420d3ed0
void BUILDCONFIG::Destroy() {
    if (this->m_buildLines.m_data) {
        this->m_buildLines.m_allocator->deallocate(this->m_buildLines.m_data);
    }
    if (this->m_partialPriority.m_data) {
        this->m_partialPriority.m_allocator->deallocate(this->m_partialPriority.m_data);
    }
    if (this->m_vfs.m_data) {
        for (uint32_t i = 0; i < this->m_vfs.m_count; i++) {
            this->m_vfs.m_data[i].m_eSpec.Destroy();
        }
        this->m_vfs.m_allocator->deallocate(this->m_vfs.m_data);
    }
    if (this->m_keyLayouts.m_data) {
        this->m_keyLayouts.m_allocator->deallocate(this->m_keyLayouts.m_data);
    }
    for (int32_t i = 5; i >= 0; i--) {
        this->m_records[i].m_eSpec.Destroy();
    }
}

// --- OPMAP / OPREGISTRY ------------------------------------------------------------------

uint32_t OPMAP::Hash(uint64_t id) {
    uint64_t h = (id >> 33 ^ id) * 0xFF51AFD7ED558CCDull;
    h = (h >> 33 ^ h) * 0xC4CEB9FE1A85EC53ull;
    return static_cast<uint32_t>(h >> 33) ^ static_cast<uint32_t>(h);
}

// ref: FUN_1420d4260
void OPMAP::Rehash(uint32_t buckets) {
    if ((buckets & (buckets - 1)) != 0) {
        int32_t bit = 31;
        while (((buckets | 1) >> bit) == 0) {
            bit--;
        }
        buckets = 1u << ((bit + 1) & 31);
    }
    auto table = static_cast<uint32_t*>(this->m_allocator->allocate(uint64_t(buckets) * 4, 0x10));
    if (buckets != 0) {
        std::memset(table, 0xFF, uint64_t(buckets) * 4);
    }
    if (this->m_entries) {
        for (uint32_t i = 0; i < this->m_count; i++) {
            uint32_t slot = Hash(this->m_entries[i].m_id) & (buckets - 1);
            this->m_next[i] = table[slot];
            table[slot] = i;
        }
    }
    this->m_allocator->deallocate(this->m_buckets);
    this->m_bucketCount = buckets;
    this->m_buckets = table;
}

// ref: FUN_1420d4370
void OPMAP::Grow(uint32_t capacity) {
    if (!this->m_buckets) {
        this->Rehash(this->m_bucketCount);
    }
    if (capacity < 8) {
        capacity = 8;
    }
    auto entries = static_cast<OPENTRY*>(this->m_allocator->allocate(uint64_t(capacity) * sizeof(OPENTRY), 0x10));
    auto next = static_cast<uint32_t*>(this->m_allocator->allocate(uint64_t(capacity) * 4, 0x10));
    for (uint32_t i = 0; i < this->m_count; i++) {
        entries[i].m_id = this->m_entries[i].m_id;
        entries[i].m_state = this->m_entries[i].m_state;
        this->m_entries[i].m_state = nullptr;
        next[i] = this->m_next[i];
    }
    this->m_allocator->deallocate(this->m_entries);
    this->m_allocator->deallocate(this->m_next);
    this->m_entries = entries;
    this->m_next = next;
    this->m_capacity = capacity;
}

// ref: FUN_1420d3260
OPENTRY* OPMAP::Insert(uint64_t* id, AsyncState** state, bool* inserted) {
    uint32_t slot = Hash(*id) & (this->m_bucketCount - 1);
    if (this->m_buckets) {
        for (uint32_t i = this->m_buckets[slot]; i != 0xFFFFFFFF; i = this->m_next[i]) {
            if (this->m_entries[i].m_id == *id) {
                *inserted = false;
                return &this->m_entries[i];
            }
        }
    }
    uint32_t index = this->m_count;
    if (index == this->m_capacity) {
        this->Grow(this->m_capacity * 2);
    }
    this->m_next[index] = this->m_buckets[slot];
    this->m_buckets[slot] = index;
    this->m_count++;
    OPENTRY* entry = &this->m_entries[index];
    entry->m_id = *id;
    entry->m_state = *state;
    if (*state) {
        (*state)->AddRef();
    }
    if (this->m_bucketCount < this->m_count) {
        this->Rehash(this->m_bucketCount * 2);
    }
    *inserted = true;
    return entry;
}

// Removes by swapping the last entry into the hole.
// ref: FUN_1420dbc90
bool OPMAP::Remove(const uint64_t* id) {
    if (!this->m_buckets) {
        return false;
    }
    uint64_t key = *id;
    uint32_t* link = &this->m_buckets[Hash(key) & (this->m_bucketCount - 1)];
    for (uint32_t i = *link; i != 0xFFFFFFFF; i = *link) {
        if (this->m_entries[i].m_id == key) {
            *link = this->m_next[i];
            uint32_t last = --this->m_count;
            OPENTRY* entry = &this->m_entries[i];
            if (i != last) {
                OPENTRY* moved = &this->m_entries[last];
                uint64_t movedKey = moved->m_id;
                uint32_t* movedLink = &this->m_buckets[Hash(movedKey) & (this->m_bucketCount - 1)];
                uint32_t j = *movedLink;
                while (j != 0xFFFFFFFF && this->m_entries[j].m_id != movedKey) {
                    movedLink = &this->m_next[j];
                    j = this->m_next[j];
                }
                if (entry->m_state) {
                    entry->m_state->Release();
                }
                entry->m_id = moved->m_id;
                entry->m_state = moved->m_state;
                moved->m_state = nullptr;
                *movedLink = i;
                this->m_next[i] = this->m_next[this->m_count];
                return true;
            }
            if (entry->m_state) {
                entry->m_state->Release();
            }
            return true;
        }
        link = &this->m_next[i];
    }
    return false;
}

// ref: FUN_1420dbed0
void OPMAP::Destroy() {
    if (this->m_buckets) {
        for (uint32_t i = 0; i < this->m_bucketCount; i++) {
            this->m_buckets[i] = 0xFFFFFFFF;
        }
    }
    for (uint32_t i = 0; i < this->m_count; i++) {
        if (AsyncState* state = this->m_entries[i].m_state) {
            state->Release();
        }
    }
    this->m_count = 0;
    this->m_allocator->deallocate(this->m_buckets);
    this->m_allocator->deallocate(this->m_entries);
    this->m_allocator->deallocate(this->m_next);
}

// ref: FUN_1420dbe80
void OPREGISTRY::Init(Instance* instance) {
    this->m_instance = instance;
    this->m_nextId = 1;
    this->m_map.m_allocator = GetAllocator();
    this->m_map.m_buckets = nullptr;
    this->m_map.m_entries = nullptr;
    this->m_map.m_next = nullptr;
    this->m_map.m_count = 0;
    this->m_map.m_capacity = 0;
    this->m_map.m_bucketCount = 0x10;
    InitializeSRWLock(&this->m_lock);
}

// ref: FUN_1420dbf80
void OPREGISTRY::Destroy() {
    this->WaitAll();
    this->m_map.Destroy();
}

// The operation under `id`, referenced and with its lock taken exclusively.
// ref: FUN_1420d3d20
OPHANDLE* OPREGISTRY::Find(OPHANDLE* out, uint64_t id) {
    AcquireSRWLockShared(&this->m_lock);
    if (this->m_map.m_buckets) {
        uint32_t slot = OPMAP::Hash(id) & (this->m_map.m_bucketCount - 1);
        for (uint32_t i = this->m_map.m_buckets[slot]; i != 0xFFFFFFFF; i = this->m_map.m_next[i]) {
            if (this->m_map.m_entries[i].m_id == id) {
                AsyncState* state = this->m_map.m_entries[i].m_state;
                if (state) {
                    state->AddRef();
                    ReleaseSRWLockShared(&this->m_lock);
                    int64_t* lock = SpinLockExclusive(&state->m_lock);
                    state->AddRef();
                    out->m_lock = lock;
                    out->m_state = state;
                    state->Release();
                    return out;
                }
                // DIVERGED: the reference takes the lock of the null state here (address
                // 0x38); entries always hold a state, so it never gets this far.
                ReleaseSRWLockShared(&this->m_lock);
                out->m_state = nullptr;
                out->m_lock = nullptr;
                return out;
            }
        }
    }
    ReleaseSRWLockShared(&this->m_lock);
    out->m_state = nullptr;
    out->m_lock = nullptr;
    return out;
}

// As Find, with the operation's lock taken shared.
// ref: FUN_1420d3600
OPHANDLE* OPREGISTRY::FindShared(OPHANDLE* out, uint64_t id) {
    AcquireSRWLockShared(&this->m_lock);
    if (this->m_map.m_buckets) {
        uint32_t slot = OPMAP::Hash(id) & (this->m_map.m_bucketCount - 1);
        for (uint32_t i = this->m_map.m_buckets[slot]; i != 0xFFFFFFFF; i = this->m_map.m_next[i]) {
            if (this->m_map.m_entries[i].m_id == id) {
                AsyncState* state = this->m_map.m_entries[i].m_state;
                if (state) {
                    state->AddRef();
                    ReleaseSRWLockShared(&this->m_lock);
                    int64_t* lock = SpinLockShared(&state->m_lock);
                    state->AddRef();
                    out->m_lock = lock;
                    out->m_state = state;
                    state->Release();
                    return out;
                }
                // DIVERGED: as in Find.
                ReleaseSRWLockShared(&this->m_lock);
                out->m_state = nullptr;
                out->m_lock = nullptr;
                return out;
            }
        }
    }
    ReleaseSRWLockShared(&this->m_lock);
    out->m_state = nullptr;
    out->m_lock = nullptr;
    return out;
}

// Unregisters the operation and drops the handle.
// ref: FUN_1420dc110
void OPREGISTRY::Release(OPHANDLE* handle) {
    if (handle->m_state) {
        AcquireSRWLockExclusive(&this->m_lock);
        uint64_t id = handle->m_state->m_opId;
        this->m_map.Remove(&id);
        ReleaseSRWLockExclusive(&this->m_lock);
    }
    if (handle->m_lock) {
        *handle->m_lock = 0;
    }
    if (handle->m_state) {
        handle->m_state->Release();
    }
}

// Waits for the operation to finish if it is still pending, unregisters it and returns its
// result (and its detail code).
// ref: FUN_1420dbfb0
uint8_t OPREGISTRY::WaitResult(uint64_t id, int32_t* detail) {
    OPHANDLE handle;
    this->Find(&handle, id);
    if (!handle.m_state) {
        if (handle.m_lock) {
            *handle.m_lock = 0;
        }
        return ERR_INVALID_PARAMETER;
    }
    if (handle.m_state->m_pending) {
        WAITSTATE wait;
        WaitPending(&handle, &wait);
        OPHANDLE done;
        this->Find(&done, id);
        this->Release(&done);
        if (detail) {
            *detail = wait.m_detail;
        }
        return wait.m_result;
    }
    uint8_t result = handle.m_state->m_result;
    if (detail) {
        *detail = handle.m_state->GetDetail();
    }
    this->Release(&handle);
    return result;
}

// Cancels every operation and waits for each to finish.
// ref: FUN_1420dc1b0
void OPREGISTRY::WaitAll() {
    AcquireSRWLockExclusive(&this->m_lock);
    while (this->m_map.m_count != 0) {
        uint64_t id = this->m_map.m_entries[0].m_id;
        AsyncState* state = this->m_map.m_entries[0].m_state;
        int64_t* lock;
        if (!state) {
            ReleaseSRWLockExclusive(&this->m_lock);
            // DIVERGED: as in Find.
            lock = nullptr;
        } else {
            state->AddRef();
            ReleaseSRWLockExclusive(&this->m_lock);
            lock = SpinLockExclusive(&state->m_lock);
            state->AddRef();
            state->Release();
        }
        state->Cancel();
        if (lock) {
            *lock = 0;
        }
        if (state) {
            state->Release();
        }

        OPHANDLE handle;
        this->Find(&handle, id);
        if (!handle.m_state) {
            if (handle.m_lock) {
                *handle.m_lock = 0;
            }
        } else {
            if (handle.m_state->m_pending) {
                WAITSTATE wait;
                WaitPending(&handle, &wait);
                this->Find(&handle, id);
            }
            this->Release(&handle);
        }
        AcquireSRWLockExclusive(&this->m_lock);
    }
    ReleaseSRWLockExclusive(&this->m_lock);
}

// The registration every operation constructor goes through, with the registry lock already
// held and the id already taken: the map keeps a reference, and the operation's lock is taken
// for the caller, who starts it and then releases both.
uint64_t OPREGISTRY::Register(AsyncState* state) {
    uint64_t id = state->m_opId;
    bool inserted;
    this->m_map.Insert(&id, &state, &inserted);
    return id;
}

// --- SCRATCHBUFFER -------------------------------------------------------------------------

// ref: FUN_1420dd7d0
void SCRATCHBUFFER::Init() {
    this->m_current = nullptr;
    this->m_first = nullptr;
    InitializeSRWLock(&this->m_lock);
    this->m_first = NewPage(this, SCRATCH_PAGE);
}

// ref: FUN_1420dd810
void SCRATCHBUFFER::Destroy() {
    int32_t* first = this->m_first;
    if (first) {
        PackageFree(GetBufferAllocator(), first);
    }
}

// A page: reference count, bytes used, and a 0xdeadbeef marker, then the slices.
// ref: FUN_1420dda10
int32_t* SCRATCHBUFFER::NewPage(SCRATCHBUFFER* buffer, uint32_t size) {
    (void)buffer;
    auto page = static_cast<int32_t*>(PackageAlloc(GetBufferAllocator(), size, 0x10, false, s_noTag, 0));
    if (!page) {
        blz::log(g_logCategory, 4, s_scratchFile, 0x8c, "Failed to allocate page in scratch buffer. Size: %u", size);
        return nullptr;
    }
    page[0] = 0;
    page[1] = 0;
    page[3] = static_cast<int32_t>(0xDEADBEEF);
    return page;
}

// Each slice has a 16-byte header: 0xbaadf00d, its offset in the page, 0x600df00d.
// ref: FUN_1420dd9c0
void* SCRATCHBUFFER::Slice(SCRATCHBUFFER* buffer, int32_t* page, uint32_t size) {
    (void)buffer;
    page[0]++;
    auto header = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(page) + static_cast<uint32_t>(page[1]) + 0x10);
    header[0] = 0xBAADF00D;
    header[1] = 0;
    header[2] = 0;
    header[3] = 0x600DF00D;
    header[1] = static_cast<uint32_t>(page[1]);
    page[1] += static_cast<int32_t>(size);
    return header;
}

// ref: FUN_1420dd860
void* SCRATCHBUFFER::Allocate(uint32_t size) {
    uint32_t aligned = (size + 0xF) & ~0xFu;
    uint32_t needed = aligned + 0x10;
    if (needed < SCRATCH_USABLE) {
        AcquireSRWLockExclusive(&this->m_lock);
        int32_t* page = this->m_current;
        uint32_t available = page ? SCRATCH_USABLE - static_cast<uint32_t>(page[1]) : 0;
        if (needed <= available) {
            auto header = static_cast<uint8_t*>(Slice(this, page, needed));
            ReleaseSRWLockExclusive(&this->m_lock);
            return header + 0x10;
        }
        if (available < SCRATCH_USABLE - needed) {
            page = this->m_first;
            if (page[0] != 0) {
                page = NewPage(this, SCRATCH_PAGE);
            }
            this->m_current = page;
            auto header = static_cast<uint8_t*>(Slice(this, page, needed));
            ReleaseSRWLockExclusive(&this->m_lock);
            return header + 0x10;
        }
        ReleaseSRWLockExclusive(&this->m_lock);
    }
    // Too big for a shared page: a page of its own.
    int32_t* page = NewPage(this, aligned + 0x20);
    auto header = static_cast<uint8_t*>(Slice(this, page, needed));
    return header + 0x10;
}

// ref: FUN_1420ddab0
void SCRATCHBUFFER::Free(void* data) {
    if (!data) {
        return;
    }
    auto bytes = static_cast<uint8_t*>(data);
    uint32_t offset = *reinterpret_cast<uint32_t*>(bytes - 0xC);
    auto page = reinterpret_cast<int32_t*>(bytes - offset - 0x20);
    AcquireSRWLockExclusive(&this->m_lock);
    if (--page[0] == 0) {
        if (page != this->m_first) {
            if (this->m_current == page) {
                this->m_current = this->m_first;
            }
            ReleaseSRWLockExclusive(&this->m_lock);
            PackageFree(GetBufferAllocator(), page);
            return;
        }
        page[1] = 0;
    }
    ReleaseSRWLockExclusive(&this->m_lock);
}

// --- Members not ported yet: their constructors and destructors ----------------------------

namespace {

// ref: FUN_1420deb80
void HostListInit(uint8_t* list) {
    FieldAt<blz::allocator*>(list, 0x00) = GetAllocator();
    FieldAt<uint64_t>(list, 0x08) = 0;
    FieldAt<uint64_t>(list, 0x10) = 0;
    FieldAt<uint64_t>(list, 0x18) = 0;
    FieldAt<uint64_t>(list, 0x20) = 0;
    FieldAt<uint32_t>(list, 0x28) = 0;
    FieldAt<uint8_t>(list, 0x2c) = 0;
}

// Entries are 0x80 bytes: two strings and an array.
// ref: FUN_1420d4060
void HostListDestroy(uint8_t* list) {
    auto data = FieldAt<uint8_t*>(list, 0x08);
    if (!data) {
        return;
    }
    uint32_t count = FieldAt<uint32_t>(list, 0x10);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t* entry = data + i * 0x80;
        if (FieldAt<void*>(entry, 0x70)) {
            FieldAt<blz::allocator*>(entry, 0x68)->deallocate(FieldAt<void*>(entry, 0x70));
        }
        FieldAt<blz::alloc_string>(entry, 0x48).Destroy();
        FieldAt<blz::alloc_string>(entry, 0x00).Destroy();
    }
    FieldAt<blz::allocator*>(list, 0x00)->deallocate(data);
}

// The aio-backed file accessor the download queue's index set reads through.
// ref: FUN_1420fb050
void FileAccessorInit(uint8_t* accessor) {
    FieldAt<uint64_t>(accessor, 0x00) = 0;
    FieldAt<uint64_t>(accessor, 0x08) = 0;
    FieldAt<uint64_t>(accessor, 0x10) = 0;
    OsFileInit(&FieldAt<OSFILE>(accessor, 0x18));
    FieldAt<uint32_t>(accessor, 0x20) = 0;
    FieldAt<uint64_t>(accessor, 0x24) = 0;
    FieldAt<blz::allocator*>(accessor, 0x38) = GetBufferAllocator();
    FieldAt<uint64_t>(accessor, 0x40) = 0;
    FieldAt<uint64_t>(accessor, 0x48) = 0;
    FieldAt<uint64_t>(accessor, 0x50) = 0;
    FieldAt<uint64_t>(accessor, 0x58) = 0;
    FieldAt<uint64_t>(accessor, 0x60) = 0;
    FieldAt<uint32_t>(accessor, 0x68) = 0xFFFFFFFF;
    FieldAt<uint64_t>(accessor, 0x70) = 0;
}

// ref: FUN_142101cd0
void DownloaderStateInit(uint8_t* state, Instance* instance) {
    FieldAt<Instance*>(state, 0x00) = instance;
    FieldAt<uint64_t>(state, 0x08) = 0;
    FieldAt<blz::alloc_string>(state, 0x20).Init(GetAllocator());
    FieldAt<uint32_t>(state, 0x50) = 0;
    std::memset(state + 0x40, 0, 0x10);
    FieldAt<blz::allocator*>(state, 0x58) = GetAllocator();
    FieldAt<uint64_t>(state, 0x60) = 0;
    FieldAt<uint64_t>(state, 0x68) = 0;
    FieldAt<blz::allocator*>(state, 0x88) = GetAllocator();
    FieldAt<uint64_t>(state, 0x70) = 0;
    FieldAt<uint64_t>(state, 0x78) = 0;
    FieldAt<uint64_t>(state, 0x80) = 0;
    FieldAt<uint64_t>(state, 0x94) = 0;
    FieldAt<uint32_t>(state, 0x90) = 0x10;
    FieldAt<blz::object_pool>(state, 0xa0).Init(GetAllocator(), 0x90, 8, 0x20, 2);
    FieldAt<uint64_t>(state, 0x138) = 0;
    FieldAt<uint64_t>(state, 0x140) = 0;
    FieldAt<uint64_t>(state, 0x148) = 0;
    FieldAt<uint32_t>(state, 0x158) = 0;
    FieldAt<blz::allocator*>(state, 0x160) = GetAllocator();
    FieldAt<uint64_t>(state, 0x168) = 0;
    FieldAt<uint64_t>(state, 0x170) = 0;
    FieldAt<blz::allocator*>(state, 0x1a0) = GetBufferAllocator();
    FieldAt<uint64_t>(state, 0x188) = 0;
    FieldAt<uint64_t>(state, 0x190) = 0;
    FieldAt<uint64_t>(state, 0x198) = 0;
    FieldAt<uint64_t>(state, 0x1ac) = 0;
    FieldAt<uint32_t>(state, 0x1a8) = 0x10;
    FieldAt<uint32_t>(state, 0x1b8) = 1;
    FieldAt<uint64_t>(state, 0x1c8) = 0;
    FieldAt<uint64_t>(state, 0x1d0) = 0;
    FieldAt<uint8_t>(state, 0x1d8) = 0;
    InitializeConditionVariable(&FieldAt<CONDITION_VARIABLE>(state, 0x150));
    InitializeSRWLock(&FieldAt<SRWLOCK>(state, 0x178));
    InitializeSRWLock(&FieldAt<SRWLOCK>(state, 0x180));
    InitializeSRWLock(&FieldAt<SRWLOCK>(state, 0x1c0));
}

// The CDN index set (0x340 bytes at download queue +0x1f0).
// ref: FUN_1420df1f0
void IndexSetInit(uint8_t* set) {
    FieldAt<uint64_t>(set, 0x28) = 0;
    FieldAt<blz::alloc_string>(set, 0x30).Init(GetAllocator());
    std::memset(set + 0x50, 0, 0x10);
    FileAccessorInit(set + 0x60);
    FieldAt<uint64_t>(set, 0xd8) = 0;
    FieldAt<uint32_t>(set, 0xe0) = 0;
    FieldAt<blz::allocator*>(set, 0xe8) = GetAllocator();
    FieldAt<uint64_t>(set, 0xf0) = 0;
    FieldAt<uint64_t>(set, 0xf8) = 0;
    FileAccessorInit(set + 0x100);
    FieldAt<uint64_t>(set, 0x178) = 0;
    FieldAt<uint32_t>(set, 0x180) = 0;
    FieldAt<blz::allocator*>(set, 0x188) = GetAllocator();
    FieldAt<uint64_t>(set, 0x190) = 0;
    FieldAt<uint64_t>(set, 0x198) = 0;
    FileAccessorInit(set + 0x1a0);
    FileAccessorInit(set + 0x218);
    FieldAt<uint8_t>(set, 0x290) = 0;
    FieldAt<uint32_t>(set, 0x294) = 0;
    FieldAt<uint16_t>(set, 0x298) = 0;
    FieldAt<uint32_t>(set, 0x29c) = 0;
    FieldAt<uint32_t>(set, 0x2a0) = 0xB;
    FieldAt<uint64_t>(set, 0x2a8) = 0;
    FieldAt<uint64_t>(set, 0x2b0) = 0;
    FieldAt<uint64_t>(set, 0x2b8) = 0;
    FieldAt<uint64_t>(set, 0x2c0) = 0;
    FieldAt<blz::allocator*>(set, 0x2e0) = GetBufferAllocator();
    FieldAt<uint64_t>(set, 0x2c8) = 0;
    FieldAt<uint64_t>(set, 0x2d0) = 0;
    FieldAt<uint64_t>(set, 0x2d8) = 0;
    FieldAt<uint64_t>(set, 0x2ec) = 0;
    FieldAt<uint32_t>(set, 0x2e8) = 0x10;
    FieldAt<uint64_t>(set, 0x2f8) = 0;
    FieldAt<uint64_t>(set, 0x300) = 0;
    FieldAt<uint64_t>(set, 0x308) = 0;
    FieldAt<uint64_t>(set, 0x310) = 0;
    FieldAt<blz::allocator*>(set, 0x318) = GetAllocator();
    FieldAt<uint64_t>(set, 0x320) = 0;
    FieldAt<uint64_t>(set, 0x328) = 0;
    InitializeCriticalSection(&FieldAt<CRITICAL_SECTION>(set, 0x00));
}

// ref: FUN_1420e5f70
void DownloadQueueInit(uint8_t* queue, Instance* instance) {
    FieldAt<Instance*>(queue, 0x00) = instance;
    FieldAt<uint64_t>(queue, 0x08) = 0;
    DownloaderStateInit(queue + 0x10, instance);
    IndexSetInit(queue + 0x1f0);
    FieldAt<uint8_t>(queue, 0x568) = 0;
    FieldAt<uint8_t*>(queue, 0x530) = queue + 0x530;
    FieldAt<uint8_t*>(queue, 0x538) = queue + 0x530;
    FieldAt<uint64_t>(queue, 0x56c) = 0x20;
    FieldAt<uint16_t>(queue, 0x574) = 0;
    FieldAt<uint8_t>(queue, 0x576) = 0;
    FieldAt<uint64_t>(queue, 0x578) = 0;
    FieldAt<uint64_t>(queue, 0x580) = 0;
    FieldAt<uint64_t>(queue, 0x588) = 0;
    InitializeCriticalSection(&FieldAt<CRITICAL_SECTION>(queue, 0x540));
}

// Drops the downloader and turns streaming off.
// ref: FUN_1420e6aa0
void DownloadQueueShutdown(uint8_t* queue) {
    FieldAt<uint8_t>(queue, 0x574) = 0;
    blz::allocator* allocator = GetAllocator();
    auto downloader = FieldAt<TaskSchedulerBase*>(queue, 0x08);
    if (downloader) {
        downloader->~TaskSchedulerBase();
        allocator->deallocate(downloader);
    }
    FieldAt<void*>(queue, 0x08) = nullptr;
}

// ref: FUN_1420e6000
void DownloadQueueDestroy(uint8_t* queue) {
    (void)queue;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1420db390
void HeaderCacheInit(uint8_t* cache, uint32_t capacity) {
    FieldAt<uint32_t>(cache, 0x08) = capacity;
    FieldAt<uint8_t*>(cache, 0x10) = cache + 0x10;
    FieldAt<uint8_t*>(cache, 0x18) = cache + 0x10;
    FieldAt<blz::allocator*>(cache, 0x38) = GetAllocator();
    FieldAt<uint64_t>(cache, 0x20) = 0;
    FieldAt<uint64_t>(cache, 0x28) = 0;
    FieldAt<uint64_t>(cache, 0x30) = 0;
    FieldAt<uint64_t>(cache, 0x44) = 0;
    FieldAt<uint32_t>(cache, 0x40) = 0x10;
    InitializeSRWLock(&FieldAt<SRWLOCK>(cache, 0x00));
}

// ref: FUN_1420db3e0
void HeaderCacheDestroy(uint8_t* cache) {
    (void)cache;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1420eb760
void PatchManifestInit(uint8_t* manifest, Instance* instance, uint8_t owns) {
    FieldAt<Instance*>(manifest, 0x00) = instance;
    FieldAt<uint8_t>(manifest, 0x08) = owns;
    FieldAt<uint64_t>(manifest, 0x18) = 0;
    FieldAt<uint64_t>(manifest, 0x20) = 0;
    FieldAt<uint32_t>(manifest, 0x28) = 0;
    FieldAt<uint16_t>(manifest, 0x2c) = 0;
    FieldAt<uint32_t>(manifest, 0x2e) = 0;
    FieldAt<uint32_t>(manifest, 0x34) = 0;
    FieldAt<blz::allocator*>(manifest, 0x38) = GetAllocator();
    for (uint64_t offset = 0x40; offset <= 0x68; offset += 8) {
        FieldAt<uint64_t>(manifest, offset) = 0;
    }
    FieldAt<uint32_t>(manifest, 0x70) = 0;
    FieldAt<uint64_t>(manifest, 0x78) = 0;
    FieldAt<uint64_t>(manifest, 0x80) = 0;
    FieldAt<uint8_t>(manifest, 0x88) = 0;
    InitializeSRWLock(&FieldAt<SRWLOCK>(manifest, 0x10));
}

// ref: FUN_1420eb7e0
void PatchManifestDestroy(uint8_t* manifest) {
    if (FieldAt<uint8_t>(manifest, 0x08)) {
        GetAllocator()->deallocate(FieldAt<void*>(manifest, 0x80));
    }
    if (FieldAt<void*>(manifest, 0x40)) {
        FieldAt<blz::allocator*>(manifest, 0x38)->deallocate(FieldAt<void*>(manifest, 0x40));
    }
}

}

// --- Instance ------------------------------------------------------------------------------

// ref: FUN_1420d33f0
Instance* Instance::Construct(blz::allocator* allocator, const char* tag, int32_t line) {
    auto instance = static_cast<Instance*>(PackageAlloc(allocator, sizeof(Instance), 8, true, tag, line));
    if (!instance) {
        return nullptr;
    }
    instance->m_ioContext = nullptr;
    instance->m_ops.Init(instance);
    instance->m_buildConfig.Init();
    HostListInit(instance->m_2a8);
    instance->m_cdnCallbacks.m_instance = instance;
    instance->m_cdnCallbacks.m_08 = nullptr;
    instance->m_cdnCallbacks.m_10 = nullptr;
    instance->m_cdnCallbacks.m_18 = nullptr;
    instance->m_cdnCallbacks.m_20 = nullptr;
    instance->m_fileManifest.Init(instance);
    instance->m_readQueue.Init(instance);
    DownloadQueueInit(instance->m_downloadQueue, instance);
    instance->m_scratch.Init();
    HeaderCacheInit(instance->m_a28, 0x10);
    instance->m_blockCache.Init();
    instance->m_encodingTable.Init(GetAllocator());
    instance->m_container = nullptr;
    instance->m_scheduler = nullptr;
    instance->m_memoryLimit = nullptr;
    instance->m_fileReadPool.Init(GetAllocator(), 0x268, 8, 0x20, 2);
    InitializeSRWLock(&instance->m_fileReadPoolLock);
    instance->m_eb8.Init(GetAllocator(), 0x110, 8, 0x20, 2);
    InitializeSRWLock(&instance->m_eb8Lock);
    instance->m_f58 = nullptr;
    instance->m_f60 = nullptr;
    for (auto& value : instance->m_f68) {
        value = 0;
    }
    PatchManifestInit(instance->m_patchManifest, instance, 1);
    instance->m_patchIndex.Init();
    instance->m_storagePath.Init(GetAllocator());
    instance->m_initialized = 0;
    instance->m_ownsIoContext = 0;
    return instance;
}

// Creates the aio context the container reads through.
// ref: FUN_1420d45c0
uint8_t Instance::CreateIoContext(uint8_t owns) {
    aio::PARAMS params;
    params.m_affinity = ~uint64_t(0);
    params.m_18 = 0;
    params.m_20 = 0;
    params.m_priority = 4;
    params.m_0c = 1;
    params.m_stackSize = 0;
    params.m_buffered = owns;
    params.m_allocator = GetBufferAllocator();
    uint8_t result;
    this->m_ioContext = aio::ContextCreate(&params, &result);
    if (!this->m_ioContext) {
        return AioToTactError(result);
    }
    this->m_ownsIoContext = 1;
    return ERR_SUCCESS;
}

// The aio results as tact errors.
// ref: FUN_1420f91b0
uint8_t AioToTactError(uint8_t result) {
    switch (result) {
        case 0:
            return ERR_SUCCESS;
        case 1:
            return ERR_INVALID_PARAMETER;
        case 2:
            return ERR_NOT_SUPPORTED;
        case 3:
            return ERR_CANCELLED;
        case 4:
            return ERR_NOT_AVAILABLE;
        case 6:
            return ERR_TRUNCATED;
        default:
            return ERR_UNKNOWN;
    }
}

// Resolves a storage path against the working directory.
// ref: FUN_1420f7870
bool NormalizeStoragePath(char* out, uint64_t size, const char* path) {
    if (!OsPathIsAbsolute(path)) {
        char cwd[0x30C];
        uint32_t error = OsGetCurrentDirectory(cwd, sizeof(cwd));
        if (error != 0) {
            char text[0x100];
            OsErrorString(text, sizeof(text), error);
            blz::log(g_logCategory, 4, s_bcUtilsFile, 0x4b, "Failed to get current working directory. Error (%u): %s",
                error, text);
            return false;
        }
        OsPathJoin(out, size, cwd, path);
    } else {
        OsPathNormalize(out, size, path);
    }
    return true;
}

// The length of a UTF-8 path in UTF-16 units, terminator included.
// ref: FUN_1420f7960
int64_t WidePathLength(const char* path) {
    return MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
}

// Starts building an instance: the aio context, the scheduler and the storage path, then the
// initialization operation for the mode. Returns the operation's id; the instance is in *out.
// ref: FUN_1420d5410
uint64_t Instance::InitializeAsync(Instance** out, INSTANCEPARAMS* params, OPCALLBACK callback, void* context) {
    *out = Construct(GetAllocator(), s_noTag, 0);
    (*out)->m_blockCache.m_enabled = params->m_65;
    Instance* instance = *out;
    uint8_t error;
    if (!params->m_ioContext) {
        error = instance->CreateIoContext(1);
        if (error != 0) {
            goto storage;
        }
    } else {
        instance->m_ioContext = params->m_ioContext;
        instance->m_ownsIoContext = 0;
    }
    if (!params->m_post) {
        instance = *out;
        uint64_t affinity = params->m_affinity;
        int32_t priority = params->m_priority;
        uint32_t threads = params->m_threads;
        if (instance->m_scheduler) {
            blz::allocator* allocator = GetAllocator();
            instance->m_scheduler->~TaskSchedulerBase();
            allocator->deallocate(instance->m_scheduler);
            instance->m_scheduler = nullptr;
        }
        TaskScheduler* scheduler = nullptr;
        void* memory = PackageAlloc(GetAllocator(), sizeof(TaskScheduler), 8, true, s_noTag, 0);
        if (memory) {
            scheduler = new (memory) TaskScheduler();
        }
        if (!scheduler->Start(threads, affinity, priority)) {
            blz::allocator* allocator = GetAllocator();
            if (scheduler) {
                scheduler->~TaskScheduler();
                allocator->deallocate(scheduler);
            }
            error = ERR_UNKNOWN;
            goto storage;
        }
        instance->m_scheduler = scheduler;
    } else {
        instance = *out;
        if (instance->m_scheduler) {
            blz::allocator* allocator = GetAllocator();
            instance->m_scheduler->~TaskSchedulerBase();
            allocator->deallocate(instance->m_scheduler);
            instance->m_scheduler = nullptr;
        }
        ClientTaskScheduler* scheduler = nullptr;
        void* memory = PackageAlloc(GetAllocator(), sizeof(ClientTaskScheduler), 8, true, s_noTag, 0);
        if (memory) {
            scheduler = new (memory) ClientTaskScheduler(params->m_post, params->m_postContext, params->m_postRelease,
                params->m_postReleaseContext);
        }
        instance->m_scheduler = scheduler;
    }
    error = ERR_SUCCESS;

storage:
    if (params->m_storagePath && error == ERR_SUCCESS) {
        char path[0x30C];
        if (!NormalizeStoragePath(path, sizeof(path), params->m_storagePath)) {
            blz::log(g_logCategory, 4, s_instanceFile, 0x17b, "Failed to normalize storage path '%s'",
                params->m_storagePath);
            error = ERR_UNKNOWN;
        } else {
            (*out)->m_storagePath.Assign(path, static_cast<uint32_t>(std::strlen(path)));
            if (static_cast<uint64_t>(WidePathLength((*out)->m_storagePath.c_str())) > 0xD4) {
                blz::log(g_logCategory, 4, s_instanceFile, 0x175,
                    "Storage directory path '%s' is too long to open all tact files.", params->m_storagePath);
                error = ERR_PATH_TOO_LONG;
            }
        }
    }
    (*out)->m_f68[2] = params->m_100[2];
    (*out)->m_f68[4] = params->m_100[4];
    (*out)->m_f68[0] = params->m_100[0];
    (*out)->m_f68[3] = params->m_100[3];
    (*out)->m_f68[5] = params->m_100[5];
    (*out)->m_f68[1] = params->m_100[1];

    if (params->m_mode == 0) {
        instance = *out;
        AcquireSRWLockExclusive(&instance->m_ops.m_lock);
        uint64_t id = instance->m_ops.m_nextId++;
        AsyncState* state = DynamicInitCreate(instance->m_ops.m_instance, id, callback, context);
        if (state) {
            state->AddRef();
        }
        instance->m_ops.Register(state);
        int64_t* lock = SpinLockExclusive(&state->m_lock);
        state->AddRef();
        state->Release();
        ReleaseSRWLockExclusive(&instance->m_ops.m_lock);
        uint64_t opId = state->m_opId;
        DynamicInitStart(state, error, params);
        if (lock) {
            *lock = 0;
        }
        state->Release();
        return opId;
    }
    if (params->m_mode == 1 || params->m_mode == 2) {
        // Modes 1 (0x1420f5290, 0x2e8 bytes) and 2 (0x1420f6560, 0x2a8 bytes) are not used by
        // the client's CAS.
        FROST_UNIMPLEMENTED();
        return 0;
    }
    return 0;
}

// Tears an instance down: every operation is cancelled and waited for first.
// ref: FUN_1420d5bb0
void Instance::Destroy(Instance* instance) {
    if (!instance) {
        blz::log(g_logCategory, 3, s_instanceFile, 0x1ac,
            "Instance not created or already destroyed. Invalid tact instance was passed.");
        return;
    }
    instance->m_ops.WaitAll();
    DownloadQueueShutdown(instance->m_downloadQueue);
    if (instance->m_container) {
        ContainerClose(instance->m_container);
        ContainerDelete(instance->m_container);
        instance->m_container = nullptr;
    }
    if (instance->m_scheduler) {
        blz::allocator* allocator = GetAllocator();
        instance->m_scheduler->~TaskSchedulerBase();
        allocator->deallocate(instance->m_scheduler);
        instance->m_scheduler = nullptr;
    }
    if (instance->m_ownsIoContext) {
        aio::ContextDestroy(static_cast<aio::CONTEXT*>(instance->m_ioContext));
    }
    blz::allocator* allocator = GetAllocator();
    instance->m_storagePath.Destroy();
    instance->m_patchIndex.Destroy();
    PatchManifestDestroy(instance->m_patchManifest);
    instance->m_eb8.Destroy();
    instance->m_fileReadPool.Destroy();
    instance->m_encodingTable.Destroy();
    instance->m_blockCache.Destroy();
    HeaderCacheDestroy(instance->m_a28);
    instance->m_scratch.Destroy();
    DownloadQueueDestroy(instance->m_downloadQueue);
    instance->m_readQueue.Destroy();
    instance->m_fileManifest.Destroy();
    HostListDestroy(instance->m_2a8);
    instance->m_buildConfig.Destroy();
    instance->m_ops.Destroy();
    allocator->deallocate(instance);
}

// Creates an instance and waits for its initialization; on failure the instance is torn down
// and null returned with the error in *error.
// ref: FUN_1420d5160
Instance* Instance::Create(INSTANCEPARAMS* params, uint8_t* error) {
    Instance* instance = nullptr;
    uint8_t local;
    uint8_t* result = error ? error : &local;
    uint64_t id = InitializeAsync(&instance, params, nullptr, nullptr);
    if (!instance) {
        blz::log(g_logCategory, 4, s_instanceFile, 0x5d6, "Unable to get the result of async op. Invalid tact instance.");
        *result = ERR_INVALID_PARAMETER;
    } else {
        *result = instance->m_ops.WaitResult(id, nullptr);
        if (*result == ERR_SUCCESS) {
            return instance;
        }
    }
    Destroy(instance);
    return nullptr;
}

// ref: FUN_1420d4680
uint8_t InstanceGetOpResult(Instance* instance, uint64_t id, int32_t* detail) {
    if (!instance) {
        blz::log(g_logCategory, 4, s_instanceFile, 0x5d6, "Unable to get the result of async op. Invalid tact instance.");
        return ERR_INVALID_PARAMETER;
    }
    return instance->m_ops.WaitResult(id, detail);
}

// ref: FUN_1420d46d0
uint8_t InstanceStatByName(Instance* instance, const char* name, FILESTAT* stat, uint64_t flags) {
    if (!instance || !instance->m_initialized) {
        blz::log(g_logCategory, 4, s_instanceFile, 0x43d, "Unable to acquire file path meta data. Invalid tact instance.");
        return ERR_INVALID_PARAMETER;
    }
    uint8_t result = instance->m_fileManifest.Lookup(name, &stat->m_key, 1);
    if (result == 0) {
        stat->m_state = 1;
        if ((flags & 2) != 0) {
            // The residency query (container slot 0x58) is not ported.
            FROST_UNIMPLEMENTED();
        }
    } else {
        if (result != 6) {
            return result;
        }
        stat->m_state = 0;
    }
    return 0;
}

// ref: FUN_1420d4b10
uint8_t InstanceIsResident(Instance* instance, const char* name) {
    if (!instance || !instance->m_initialized) {
        blz::log(g_logCategory, 4, s_instanceFile, 0x4bf, "Unable to check file residency. Invalid tact instance.");
        return ERR_INVALID_PARAMETER;
    }
    FILEKEY key;
    uint8_t result = instance->m_fileManifest.Lookup(name, &key, 1);
    if (result != 0) {
        return result;
    }
    return instance->m_container->Contains(key.m_ekey) ? 0 : 7;
}

// ref: FUN_1420d4c70
uint64_t InstanceStartReadByName(Instance* instance, const char* name, void* dest, uint32_t size,
    OPCALLBACK callback, void* context, uint8_t priority, uint32_t flags) {
    if (!instance || !instance->m_initialized) {
        blz::log(g_logCategory, 4, s_instanceFile, 0x25a, "Unable to start an async file read. Invalid tact instance.");
        return 0;  // DAT_1428e4ca0
    }
    if ((flags & 0x20) != 0) {
        // The immediate-first path (an already-completed op, FUN_1420d3be0 / FUN_1420f7810) is not ported.
        FROST_UNIMPLEMENTED();
    }
    OPHANDLE handle;
    CreateFileRead(instance, &handle, callback, context);
    AsyncState* state = handle.m_state;
    FileReadStartByName(state, name, dest, size, priority, flags);
    uint64_t id = state->m_opId;
    if (handle.m_lock) {
        *handle.m_lock = 0;
    }
    state->Release();
    return id;
}

// ref: FUN_1420d4640
void InstanceCancelOp(Instance* instance, uint64_t id) {
    if (!instance) {
        blz::log(g_logCategory, 4, s_instanceFile, 0x5bc, "Unable to cancel async op. Invalid tact instance.");
        return;
    }
    CancelOp(instance, id);
}

// Opens the local storage container under <storage>/data.
// ref: FUN_1420d5dd0
uint8_t Instance::InitializeContainer(const char* path, int32_t streaming, uint32_t a4, uint32_t a5, uint32_t a6,
    uint8_t a7, uint64_t a8, uint8_t a9) {
    if (!this->m_ioContext) {
        blz::log(g_logCategory, 4, s_instanceFile, 0xb7, "Failed to initialize container. Aio must be initialized first.");
        return ERR_INVALID_PARAMETER;
    }
    Container* container = nullptr;
    if (this->m_container) {
        ContainerDelete(this->m_container);
        this->m_container = nullptr;
    }
    char directory[0x30C];
    OsPathJoin(directory, sizeof(directory), path, s_containerDir);
    OsPathNormalize(directory, sizeof(directory), directory);
    OsCreateDirectoryTree(directory, true);
    void* memory = PackageAlloc(GetAllocator(), 0x48, 8, true, s_noTag, 0);
    if (memory) {
        container = ContainerConstruct(memory, &this->m_cdnCallbacks);
    }
    CONTAINERPARAMS params;
    params.m_directory = directory;
    params.m_a6 = a6;
    params.m_ioContext = this->m_ioContext;
    params.m_a7 = a7;
    params.m_a8 = a8;
    params.m_a9 = a9;
    params.m_accessMode = streaming;
    params.m_a4 = a4;
    params.m_a5 = a5;
    uint8_t error = ContainerOpen(container, &params);
    if (error == ERR_SUCCESS) {
        this->m_container = container;
        return ERR_SUCCESS;
    }
    blz::allocator* allocator = GetAllocator();
    if (container) {
        ContainerDestruct(container);
        allocator->deallocate(container);
    }
    return error;
}

}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_1420fb050 = (void*)&tact::FileAccessorInit;
// ---- end of generated globals ----
