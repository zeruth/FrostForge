// SE3Cache (Engine/Source/SoundEngine/SE3Cache.cpp): every FMOD sound the engine made, keyed
// by file and mode. FMOD opens the files through the callbacks here, which read them from CAS;
// sounds load without blocking and report through a message the frame drains.

#include "sound/SE3Kit.hpp"

#include <cstdlib>
#include <cstring>

#include "base/DataAllocator.hpp"
#include "cas/CAS.hpp"
#include "fileservices/AsyncFile.hpp"
#include "fmod.hpp"
#include "os/Sync.hpp"
#include "sound/SE3.hpp"
#include "sound/SE3Internal.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "storm/thread/SCritSect.hpp"
#include "util/Unimplemented.hpp"

using cas::CASFILE;

int32_t g_se3CacheMaxCacheable;         // DAT_142cfcfbc

// The read-ahead of a file (0x550): prefetches of the 16 KB blocks ahead of FMOD's reads, and
// for an encrypted voice file the key and the cipher (SE3AsyncData).
struct SE3ASYNCDATA {
    int32_t m_blocks;                   // 0x00 blocks prefetched so far (the estimate at open)
    uint8_t m_pad04[4];
    CASFILE* m_file;                    // 0x08
    int32_t m_pending;                  // 0x10 prefetches in flight
    uint8_t m_closing;                  // 0x14 FMOD closed the file while prefetches ran
    uint8_t m_pad15[3];
    TSList<struct SE3PREFETCH, TSGetLink<struct SE3PREFETCH>, 8> m_prefetches;  // 0x18 (link +0x10)
    uint8_t m_keyed;                    // 0x30
    uint8_t m_pad31[3];
    int32_t m_frameOffset;              // 0x34 the first MP3 frame
    int32_t m_patchState;               // 0x38 1 the frame sync still to restore, 3 its second byte
    uint8_t m_key[0x20];                // 0x3C
    uint8_t m_iv[0x20];                 // 0x5C
    uint8_t m_cipher[0x4d0];            // 0x80
};
static_assert(sizeof(SE3ASYNCDATA) == 0x550, "SE3ASYNCDATA is 0x550 bytes");

// One prefetch (0x20).
struct SE3PREFETCH {
    CAsyncObject* m_object;             // 0x00
    SE3ASYNCDATA* m_async;              // 0x08
    TSLink<SE3PREFETCH> m_link;         // 0x10
};

namespace {

const char s_file[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\SoundEngine\\SE3Cache.cpp";
const char s_prefetchTag[] = ".?AV?$TSList@USE3AsyncData@@V?$TSGetExplicitLink@USE3AsyncData@@@@$07@@";
const char s_doneLoadingTag[] = ".?AVSE3DoneLoadingMsg@@";

using CACHELIST = TSList<SE3CacheNode, TSGetLink<SE3CacheNode>, 8>;
CACHELIST s_samplesLoading;             // 0x142cfcfe0
CACHELIST s_samplesReady;               // 0x142cfcff8
CACHELIST s_streamsLoading;             // 0x142cfd010
CACHELIST s_streamsReady;               // 0x142cfd028
CACHELIST s_idle;                       // 0x142cfd040 (link +0x48) unused sounds, newest first
CACHELIST s_released;                   // 0x142cfd058 (link +0x58) waiting for FMOD to let go
SCritSect s_samplesLock;                // DAT_142ddced0
SCritSect s_streamsLock;                // DAT_142ddcef8
SCritSect s_messageLock;                // DAT_142ddcea8
OSLAZYCRITSECT s_asyncLock;             // DAT_1430a9750
int32_t s_serial;                       // DAT_142cfcfb8
int32_t s_maxSize = 0x8000000;          // DAT_142cfcfc0
int32_t s_size;                         // DAT_1430a9720 the cached samples' bytes
int32_t s_releaseWait;                  // DAT_1430a9758

// The finished loads FMOD reported (SE3DoneLoadingMsg, 0x20).
struct DONELOADINGMSG {
    TSLink<DONELOADINGMSG> m_link;
    FMOD::Sound* m_sound;
    int32_t m_result;
};
CDataAllocator s_doneLoadingMessages(sizeof(DONELOADINGMSG), 0x20);    // DAT_1430a9728
TSList<DONELOADINGMSG, TSGetLink<DONELOADINGMSG>, 8> s_doneLoading;     // 0x142cfcfc8

// The static initializers: the lists' link offsets and the locks.
struct LISTSINIT {
    LISTSINIT() {
        s_idle.m_linkoffset = 0x48;
        s_released.m_linkoffset = 0x58;
        s_samplesLock.Initialize();
        s_streamsLock.Initialize();
        s_messageLock.Initialize();
    }
} s_listsInit;

void Unlink(TSLink<SE3CacheNode>* link) {
    link->Unlink();
}

// The cache's handlers (SE3SoundKit.cpp holds them).
void CallEvent(int32_t event, SE3CacheNode* node);

}

// ---- the FMOD file callbacks --------------------------------------------------------------

namespace {

// How far ahead to prefetch an MP3 stream: from its first frame's bitrate against the disk's
// read time. A keyed file has its frame sync restored and its first block decrypted.
// ref: FUN_1412dacd0
bool EstimatePrefetch(SE3ASYNCDATA* async) {
    static const int32_t s_bitrates[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    uint8_t block[0x4000];
    uint32_t read = 0;
    if (!cas::CASFileReadEx(async->m_file, block, 0x4000, &read, 0, -1) || read != 0x4000) {
        return false;
    }
    cas::CASFileSetPosition(async->m_file, 0, 0);
    if (async->m_keyed != 0) {
        uint32_t frame = 0;
        if (block[0] == 'I' && block[1] == 'D' && block[2] == '3') {
            frame = ((((static_cast<uint32_t>(block[6]) << 7 | block[7]) << 7) | block[8]) << 7 | block[9]) + 10;
        }
        if (0x3fff < frame) {
            return false;
        }
        uint32_t size = static_cast<uint32_t>(cas::CASFileGetSize(async->m_file));
        uint32_t sync = (static_cast<uint32_t>(block[frame]) << 8 | block[frame + 1]) >> 5;
        if (sync == 0x550) {
            uint32_t got = 0;
            if (!cas::CASFileReadAtOffset(async->m_file, async->m_iv, size - 0x20, 0x20, &got, 0, -1)) {
                return false;
            }
            cas::CASFileSetPosition(async->m_file, 0, 0);
            async->m_keyed = 1;
            FROST_UNIMPLEMENTED();  // FUN_1420bdae0 / FUN_1420bdb20 / FUN_1420bdb10: the stream cipher over the block
            block[frame + 1] |= 0xe0;
            block[frame] = 0xff;
        } else {
            async->m_keyed = 0;
            if (sync != 0x7ff) {
                return false;
            }
        }
        async->m_frameOffset = static_cast<int32_t>(frame);
        async->m_patchState = 1;
    }
    for (uint32_t i = 0; i < 0x3ffd; i++) {
        const uint8_t* p = block + i;
        if (p[0] == 0xff && (p[1] & 0xe0) == 0xe0 && (p[2] & 0xc) != 0xc) {
            int32_t bitrate = s_bitrates[p[2] >> 4];
            if (0 < bitrate) {
                uint32_t blocks = (static_cast<uint32_t>(cas::CASFileGetSize(async->m_file)) << 3 >> 10)
                    / static_cast<uint32_t>(bitrate);
                int32_t latency = static_cast<int32_t>(AsyncGetDiskReadTime() * 1000.0f);
                if (latency < 1) {
                    latency = 0x2ee;
                }
                if (bitrate * 2 < latency) {
                    async->m_blocks = 2;
                } else if (bitrate <= latency) {
                    async->m_blocks = 3;
                } else {
                    async->m_blocks = static_cast<int32_t>(
                        ((static_cast<uint32_t>(bitrate - latency) * blocks * 0x80 + 0x3fff) >> 14) + 3);
                }
                return true;
            }
        }
    }
    return false;
}

// Frees the read-ahead.
// ref: FUN_1412d9a30
void FreeAsyncData(SE3ASYNCDATA* async) {
    while (SE3PREFETCH* prefetch = async->m_prefetches.Head()) {
        prefetch->m_link.Unlink();
    }
    async->m_prefetches.m_terminator.Unlink();
    SMemFree(async, s_file, -2, 0);
}

// A prefetch finished (or was canceled): the last one closes a file FMOD already closed.
// ref: FUN_1412da5e0
void PrefetchDone(void* userData, void* buffer, uint32_t size) {
    (void)buffer;
    (void)size;
    auto prefetch = static_cast<SE3PREFETCH*>(userData);
    SE3ASYNCDATA* async = prefetch->m_async;
    OsLazyCritSectEnter(&s_asyncLock);
    async->m_pending--;
    prefetch->m_link.Unlink();
    SMemFree(prefetch, s_prefetchTag, -2, 0);
    if (async->m_pending == 0 && async->m_closing != 0) {
        cas::CASFileClose(&async->m_file);
        FreeAsyncData(async);
    }
    OsLazyCritSectLeave(&s_asyncLock);
}

// LAB_1412da5d0
void PrefetchFailed(void* userData, void* buffer, uint32_t size, const char* error) {
    (void)error;
    PrefetchDone(userData, buffer, size);
}

// FMOD opens a file: the name is the cache key; the node waiting for it hands over its file.
// ref: FUN_1412da460
FMOD_RESULT F_CALLBACK FileOpen(const char* name, int unicode, unsigned int* size, void** handle, void** userData) {
    (void)unicode;
    int32_t key = std::atoi(name);
    SE3CacheNode* found = nullptr;
    SCritSect* lock = &s_samplesLock;
    lock->Enter();
    for (SE3CacheNode* node = s_samplesLoading.Head(); node != nullptr; node = s_samplesLoading.Next(node)) {
        if (node->m_key == key && (node->m_flags & 2) != 0) {
            found = node;
        }
    }
    FMOD_RESULT result = FMOD_OK;
    if (found == nullptr) {
        lock->Leave();
        lock = &s_streamsLock;
        lock->Enter();
        for (SE3CacheNode* node = s_streamsLoading.Head(); node != nullptr; node = s_streamsLoading.Next(node)) {
            if (node->m_key == key && (node->m_flags & 2) != 0) {
                found = node;
                break;
            }
        }
        if (found == nullptr) {
            result = FMOD_ERR_FILE_NOTFOUND;
        }
    }
    if (found != nullptr) {
        *size = static_cast<unsigned int>(cas::CASFileGetSize(found->m_file));
        *handle = found->m_file;
        *userData = found->m_async;
        found->m_flags &= 0xfd;
        found->m_async = nullptr;
    }
    lock->Leave();
    if (*userData != nullptr) {
        EstimatePrefetch(static_cast<SE3ASYNCDATA*>(*userData));
    }
    return result;
}

// FMOD closes a file: the file closes now, or when the last prefetch finishes.
// ref: FUN_1412da3a0
FMOD_RESULT F_CALLBACK FileClose(void* handle, void* userData) {
    if (userData != nullptr) {
        auto async = static_cast<SE3ASYNCDATA*>(userData);
        bool waiting = false;
        OsLazyCritSectEnter(&s_asyncLock);
        for (SE3PREFETCH* prefetch = async->m_prefetches.Head(); prefetch != nullptr;
             prefetch = async->m_prefetches.Next(prefetch)) {
            AsyncFileCancel(prefetch->m_object);
            async->m_closing = 1;
            waiting = true;
        }
        OsLazyCritSectLeave(&s_asyncLock);
        if (waiting) {
            return FMOD_OK;
        }
        FreeAsyncData(async);
    }
    auto file = static_cast<CASFILE*>(handle);
    cas::CASFileClose(&file);
    return FMOD_OK;
}

// FMOD reads: the next 16 KB block is prefetched ahead of the read; a keyed file is decrypted
// as it is read.
// ref: FUN_1412da6a0
FMOD_RESULT F_CALLBACK FileRead(void* handle, void* buffer, unsigned int size, unsigned int* bytesRead,
    void* userData) {
    auto file = static_cast<CASFILE*>(handle);
    auto async = static_cast<SE3ASYNCDATA*>(userData);
    uint32_t fileSize = static_cast<uint32_t>(cas::CASFileGetSize(file));
    if (async != nullptr) {
        uint32_t offset = static_cast<uint32_t>(async->m_blocks) * 0x4000;
        if (offset < fileSize) {
            OsLazyCritSectEnter(&s_asyncLock);
            auto prefetch = static_cast<SE3PREFETCH*>(SMemAlloc(sizeof(SE3PREFETCH), 8, s_prefetchTag, 0, -2, 8));
            prefetch->m_link.m_prevlink = nullptr;
            prefetch->m_link.m_next = nullptr;
            async->m_prefetches.LinkToTail(prefetch);
            prefetch->m_async = async;
            async->m_pending++;
            uint32_t length = fileSize - offset;
            if (0x4000 < length) {
                length = 0x4000;
            }
            prefetch->m_object = AsyncFileReadFromFile(file, nullptr, offset, length, 2, prefetch, nullptr, nullptr,
                &PrefetchDone, &PrefetchFailed, &PrefetchDone);
            async->m_blocks++;
            OsLazyCritSectLeave(&s_asyncLock);
        }
    }
    uint32_t position = static_cast<uint32_t>(cas::CASFileSetPosition(file, 0, 1));
    uint32_t read = 0;
    bool ok = cas::CASFileReadEx(file, buffer, size, &read, 0, -1);
    *bytesRead = read;
    if (!ok) {
        SE3Log(0x423, s_file, 0, "=> Failure: requested %u bytes but read %u", size, read);
        return FMOD_ERR_FILE_EOF;
    }
    if (read == 0) {
        SE3Log(0x428, s_file, 0, "=> Requested %u bytes and read 0 -- success: %d", size, ok);
    }
    if (async == nullptr || async->m_keyed == 0) {
        return size != read ? FMOD_ERR_FILE_EOF : FMOD_OK;
    }
    FROST_UNIMPLEMENTED();  // the keyed path: the cipher over the block and the frame sync restored (FUN_1420bd960)
    (void)position;
    return size != read ? FMOD_ERR_FILE_EOF : FMOD_OK;
}

// ref: FUN_1412da9f0
FMOD_RESULT F_CALLBACK FileSeek(void* handle, unsigned int position, void* userData) {
    (void)userData;
    cas::CASFileSetPosition(static_cast<CASFILE*>(handle), position, 0);
    return FMOD_OK;
}

// A non-blocking load finished: through the done-loading handler (LAB_1412da380).
FMOD_RESULT F_CALLBACK SoundLoaded(FMOD_SOUND* sound, FMOD_RESULT result) {
    return static_cast<FMOD_RESULT>(SE3CallDoneLoadingHandler(reinterpret_cast<FMOD::Sound*>(sound), result));
}

}

// ---- the cache ----------------------------------------------------------------------------

// ref: FUN_1412daa10
bool SE3CacheFindReady(int32_t key, SE3CacheNode** node) {
    for (SE3CacheNode* each = s_samplesReady.Head(); each != nullptr; each = s_samplesReady.Next(each)) {
        if (each->m_key == key) {
            if (node != nullptr) {
                *node = each;
            }
            return true;
        }
    }
    return false;
}

// ref: FUN_1412dab40
bool SE3CacheFindLoading(int32_t key, SE3CacheNode** node) {
    for (SE3CacheNode* each = s_samplesLoading.Head(); each != nullptr; each = s_samplesLoading.Next(each)) {
        if (each->m_key == key) {
            if (node != nullptr) {
                *node = each;
            }
            return true;
        }
    }
    return false;
}

// The loaded sound of one of an instance's two slots: the slot's index in *slot.
// ref: FUN_1412daa60
int32_t SE3CacheGetSound(SE3SoundKitObject* object, SE3CacheNode** node, uint32_t* slot) {
    for (uint32_t i = 0; i < 2; i++) {
        int32_t serial = object->m_slots[i].m_serial;
        for (SE3CacheNode* each = s_samplesReady.Head(); each != nullptr; each = s_samplesReady.Next(each)) {
            if (each->m_serial == serial) {
                if (node != nullptr) {
                    *node = each;
                }
                *slot = i;
                return 0;
            }
        }
        for (SE3CacheNode* each = s_streamsReady.Head(); each != nullptr; each = s_streamsReady.Next(each)) {
            if (each->m_serial == serial) {
                if (node != nullptr) {
                    *node = each;
                }
                *slot = i;
                return 0;
            }
        }
    }
    return 4;
}

namespace {

// A sound's user count in one list; at no users it goes to the idle list, at one it leaves it.
// ref: FUN_1412db960
bool AddUserIn(int32_t serial, CACHELIST* list, int32_t delta) {
    if (serial == 0) {
        return false;
    }
    SE3CacheNode* node = list->Head();
    while (node != nullptr && node->m_serial != serial) {
        node = list->Next(node);
    }
    if (node == nullptr) {
        return false;
    }
    node->m_users += delta;
    if (delta < 1) {
        if (delta < 0 && node->m_users == 0) {
            Unlink(&node->m_lru);
            s_idle.LinkToHead(node);
        }
    } else if (node->m_users == 1) {
        Unlink(&node->m_lru);
    }
    if (node->m_users < 0) {
        node->m_users = 0;
    }
    return true;
}

}

// ref: FUN_1412db8a0
void SE3CacheAddUser(int32_t serial, bool add) {
    if (serial == 0) {
        return;
    }
    int32_t delta = add ? 1 : -1;
    if (AddUserIn(serial, &s_samplesLoading, delta) || AddUserIn(serial, &s_samplesReady, delta)) {
        return;
    }
    SE3CacheNode* node = nullptr;
    for (SE3CacheNode* each = s_streamsLoading.Head(); each != nullptr; each = s_streamsLoading.Next(each)) {
        if (each->m_serial == serial) {
            node = each;
            break;
        }
    }
    if (node == nullptr) {
        for (SE3CacheNode* each = s_streamsReady.Head(); each != nullptr; each = s_streamsReady.Next(each)) {
            if (each->m_serial == serial) {
                node = each;
                break;
            }
        }
    }
    if (node == nullptr) {
        return;
    }
    node->m_users += delta;
    if (node->m_users < 0) {
        node->m_users = 0;
    }
}

// Retires a sound: its users told, off every list, onto the released list.
// ref: FUN_1412da230
void SE3CacheReleaseNode(SE3CacheNode* node, bool loaded) {
    if (node == nullptr || node->m_releaseLink.m_next != nullptr) {
        return;
    }
    if (node->m_users != 0) {
        CallEvent(loaded + 1, node);
    }
    Unlink(&node->m_link);
    Unlink(&node->m_lru);
    s_released.LinkToTail(node);
    if ((node->m_flags & 1) == 0 && node->m_size != 0) {
        s_size -= node->m_size;
    }
}

// A new sound: FMOD makes it without blocking, through the file callbacks.
// ref: FUN_1412d9bf0
uint32_t SE3CacheCreate(int32_t key, FMOD::SoundGroup* group, uint32_t mode, SE3SoundKitObject* object, int32_t size,
    int32_t fileDataId, cas::CASFILE** file, SE3CacheNode** created, SE3ASYNCDATA* async) {
    FMOD_CREATESOUNDEXINFO info = {};
    info.cbsize = sizeof(info);
    info.nonblockcallback = &SoundLoaded;
    info.userdata = async;
    info.useropen = &FileOpen;
    info.userclose = &FileClose;
    info.userread = &FileRead;
    info.userseek = &FileSeek;
    info.initialsoundgroup = reinterpret_cast<FMOD_SOUNDGROUP*>(group);
    if ((*file)->m_type == 1) {
        mode |= 0x8000;
        info.suggestedsoundtype = FMOD_SOUND_TYPE_UNKNOWN;
    } else if (SE3IsMp3(fileDataId)) {
        mode |= 0x8000;
        info.suggestedsoundtype = FMOD_SOUND_TYPE_MPEG;
    } else {
        info.suggestedsoundtype = FMOD_SOUND_TYPE_OGGVORBIS;
    }
    if (object->m_streamed != 0) {
        int32_t start = object->m_params.m_b8;
        if (start == 0 && object->m_def != nullptr) {
            start = SE3AdvancedStartOffset(object->m_def);
        }
        if (start != 0) {
            info.initialseekposition = static_cast<unsigned int>(start);
            info.initialseekpostype = FMOD_TIMEUNIT_MS;
        }
    }
    auto node = static_cast<SE3CacheNode*>(SMemAlloc(sizeof(SE3CacheNode), 0x16, s_file, SStrHashFnv1a(s_file), 0x4d6, 0));
    std::memset(node, 0, sizeof(*node));
    node->m_serial = ++s_serial;
    node->m_async = async;
    node->m_releaseTimer = 500;
    node->m_file = *file;
    node->m_key = key;
    node->m_fileDataId = fileDataId;
    node->m_size = size;
    node->m_flags = static_cast<uint8_t>((object->m_streamed & 1) | 2);
    if ((object->m_streamed & 1) == 0) {
        s_size += size;
    }
    if (created != nullptr) {
        *created = node;
    }
    char name[0x20];
    SStrPrintf(name, 0x20, "%d", node->m_key);
    SCritSect* lock;
    FMOD_RESULT result;
    if ((node->m_flags & 1) == 0) {
        lock = &s_samplesLock;
        lock->Enter();
        s_samplesLoading.LinkToTail(node);
        result = g_fmodSystem->createSound(name, mode, &info, &node->m_sound);
    } else {
        lock = &s_streamsLock;
        lock->Enter();
        s_streamsLoading.LinkToTail(node);
        result = g_fmodSystem->createStream(name, mode, &info, &node->m_sound);
    }
    if (result != FMOD_OK) {
        cas::CASFileClose(file);
        Unlink(&node->m_releaseLink);
        Unlink(&node->m_lru);
        Unlink(&node->m_link);
        SMemFree(node, s_file, -2, 0);
    }
    lock->Leave();
    return result != FMOD_OK ? 3 : 0;
}

// The read-ahead of a file to stream: allocated empty, keyed when the play brought a key.
SE3ASYNCDATA* SE3AsyncDataCreate(CASFILE* file, const uint32_t* key) {
    auto async = static_cast<SE3ASYNCDATA*>(SMemAlloc(sizeof(SE3ASYNCDATA), 0x16, s_file, SStrHashFnv1a(s_file), 0x1d16, 0));
    std::memset(async, 0, sizeof(*async));
    async->m_prefetches.m_linkoffset = 0x10;
    async->m_prefetches.m_terminator.m_prevlink = &async->m_prefetches.m_terminator;
    async->m_prefetches.m_terminator.m_next =
        reinterpret_cast<SE3PREFETCH*>(reinterpret_cast<uintptr_t>(&async->m_prefetches.m_terminator) | 1);
    // FUN_1420bd880: the cipher's state
    async->m_file = file;
    if (key == nullptr) {
        async->m_keyed = 0;
    } else {
        std::memcpy(async->m_key, key, 0x20);
        async->m_keyed = 1;
    }
    return async;
}

// A load finished: the message FMOD's thread leaves for the frame.
// ref: FUN_1412d9b50
int32_t SE3OnDoneLoading(void* sound, int32_t result) {
    if (g_fmodSystem != nullptr && g_se3ShuttingDown == 0) {
        s_messageLock.Enter();
        auto message = static_cast<DONELOADINGMSG*>(s_doneLoadingMessages.GetData(true, s_doneLoadingTag, -2, 1));
        message->m_link.m_prevlink = nullptr;
        message->m_link.m_next = nullptr;
        message->m_sound = static_cast<FMOD::Sound*>(sound);
        message->m_result = result;
        s_doneLoading.LinkToTail(message);
        s_messageLock.Leave();
    }
    return 0;
}

// The frame's loads: each finished sound moves to its ready list, a failed one is released,
// and the instances waiting on it hear either way.
// ref: FUN_1412db320
void SE3ProcessDoneLoading() {
    TSList<DONELOADINGMSG, TSGetLink<DONELOADINGMSG>, 8> messages;
    s_messageLock.Enter();
    while (DONELOADINGMSG* message = s_doneLoading.Head()) {
        message->m_link.Unlink();
        messages.LinkToTail(message);
    }
    s_messageLock.Leave();
    while (DONELOADINGMSG* message = messages.Head()) {
        FMOD::Sound* sound = message->m_sound;
        int32_t result = message->m_result;
        SE3CacheNode* found = nullptr;
        s_samplesLock.Enter();
        for (SE3CacheNode* node = s_samplesLoading.Head(); node != nullptr; node = s_samplesLoading.Next(node)) {
            if (node->m_sound == sound) {
                if (result == 0) {
                    node->m_state = 2;
                    Unlink(&node->m_link);
                    s_samplesReady.LinkToTail(node);
                } else {
                    CallEvent(0, node);
                    node->m_state = 3;
                    Unlink(&node->m_link);
                    SE3CacheReleaseNode(node, false);
                }
                found = node;
                break;
            }
        }
        s_samplesLock.Leave();
        if (found == nullptr) {
            s_streamsLock.Enter();
            for (SE3CacheNode* node = s_streamsLoading.Head(); node != nullptr; node = s_streamsLoading.Next(node)) {
                if (node->m_sound == sound) {
                    if (result == 0) {
                        node->m_state = 2;
                        Unlink(&node->m_link);
                        s_streamsReady.LinkToTail(node);
                    } else {
                        CallEvent(0, node);
                        node->m_state = 3;
                        Unlink(&node->m_link);
                        SE3CacheReleaseNode(node, false);
                    }
                    found = node;
                    break;
                }
            }
            s_streamsLock.Leave();
        }
        if (found != nullptr) {
            if (found->m_state == 2) {
                CallEvent(3, found);
            } else {
                SE3CacheReleaseNode(found, false);
            }
        }
        s_messageLock.Enter();
        message->m_link.Unlink();
        s_doneLoadingMessages.PutData(message);
        s_messageLock.Leave();
    }
}

// The frame's release pass: retired sounds whose time is up (or whose FMOD load ended) are
// released; `flush` retires everything first.
// ref: FUN_1412daf50
void SE3CacheUpdate(int32_t elapsed, bool flush, bool restart) {
    if (flush) {
        s_samplesLock.Enter();
        for (SE3CacheNode* node = s_samplesLoading.Head(); node != nullptr;) {
            SE3CacheNode* next = s_samplesLoading.Next(node);
            SE3CacheReleaseNode(node, restart);
            node = next;
        }
        s_samplesLock.Leave();
        s_streamsLock.Enter();
        for (SE3CacheNode* node = s_streamsLoading.Head(); node != nullptr;) {
            SE3CacheNode* next = s_streamsLoading.Next(node);
            SE3CacheReleaseNode(node, restart);
            node = next;
        }
        s_streamsLock.Leave();
        for (SE3CacheNode* node = s_samplesReady.Head(); node != nullptr;) {
            SE3CacheNode* next = s_samplesReady.Next(node);
            SE3CacheReleaseNode(node, restart);
            node = next;
        }
        for (SE3CacheNode* node = s_streamsReady.Head(); node != nullptr;) {
            SE3CacheNode* next = s_streamsReady.Next(node);
            SE3CacheReleaseNode(node, restart);
            node = next;
        }
    }
    SE3CacheNode* node = s_released.Head();
    while (node != nullptr) {
        SE3CacheNode* next = s_released.Next(node);
        node->m_releaseTimer -= elapsed;
        FMOD_OPENSTATE state = FMOD_OPENSTATE_READY;
        bool done = flush;
        if (node->m_sound != nullptr) {
            node->m_sound->getOpenState(&state, nullptr, nullptr, nullptr);
        }
        if (!flush && node->m_releaseTimer < 1) {
            // FUN_14158f790 (Sound: whether it may be released now) is an extension of the
            // reference's FMOD build; the public 4.44 has no such call. Its answer is taken as
            // "yes" once the load has ended, which the open state already says.
            done = true;
        }
        if ((!flush && (state & ~FMOD_OPENSTATE_ERROR) != 0) || !done) {
            node = next;
            continue;
        }
        if (!flush) {
            if (s_releaseWait + elapsed < 10) {
                s_releaseWait += elapsed;
                return;
            }
            s_releaseWait = 0;
        }
        if (node->m_sound != nullptr) {
            node->m_sound->release();
            node->m_sound = nullptr;
        }
        if ((node->m_flags & 2) != 0) {
            SE3ASYNCDATA* async = node->m_async;
            CASFILE* file = node->m_file;
            bool waiting = false;
            if (async != nullptr) {
                OsLazyCritSectEnter(&s_asyncLock);
                for (SE3PREFETCH* prefetch = async->m_prefetches.Head(); prefetch != nullptr;
                     prefetch = async->m_prefetches.Next(prefetch)) {
                    AsyncFileCancel(prefetch->m_object);
                    async->m_closing = 1;
                    waiting = true;
                }
                OsLazyCritSectLeave(&s_asyncLock);
                if (!waiting) {
                    FreeAsyncData(async);
                }
            }
            if (!waiting) {
                cas::CASFileClose(&file);
            }
        }
        Unlink(&node->m_releaseLink);
        Unlink(&node->m_lru);
        Unlink(&node->m_link);
        SMemFree(node, s_file, -2, 0);
        node = next;
    }
}

// Releases the oldest idle samples while the cache is over its size.
// ref: FUN_1412dab90
void SE3CacheTrim() {
    while (s_maxSize < s_size) {
        TSLink<SE3CacheNode>* last = s_idle.m_terminator.m_prevlink;
        if (last == &s_idle.m_terminator) {
            return;
        }
        SE3CacheNode* node = last->m_prevlink->m_next;
        if (node == nullptr || (reinterpret_cast<uintptr_t>(node) & 1) != 0) {
            return;
        }
        Unlink(&node->m_lru);
        SE3CacheReleaseNode(node, false);
    }
}

// Releases the streams nothing plays.
// ref: FUN_1412dac70
void SE3CacheReleaseIdle() {
    for (SE3CacheNode* node = s_streamsReady.Head(); node != nullptr;) {
        SE3CacheNode* next = s_streamsReady.Next(node);
        if (node->m_users == 0) {
            SE3CacheReleaseNode(node, false);
        }
        node = next;
    }
}

// ref: FUN_1412db880
void SE3CacheSetMaxSize(int32_t bytes) {
    s_maxSize = bytes;
}

// ref: FUN_1412db890
void SE3CacheSetMaxCacheableSize(int32_t bytes) {
    g_se3CacheMaxCacheable = bytes;
}

// The shutdown's frees: the done-loading messages and the lists (FUN_1412d41d0's middle).
void SE3CacheShutdown() {
    s_messageLock.Enter();
    while (DONELOADINGMSG* message = s_doneLoading.Head()) {
        message->m_link.Unlink();
        s_doneLoadingMessages.PutData(message);
    }
    s_doneLoadingMessages.Clear(s_doneLoadingTag, -2);
    s_messageLock.Leave();
}

namespace {

void CallEvent(int32_t event, SE3CacheNode* node) {
    SE3CallCacheEventHandler(event, node);
}

}

void SE3SamplesLock(bool enter) {
    if (enter) {
        s_samplesLock.Enter();
    } else {
        s_samplesLock.Leave();
    }
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1420bd960=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1420bdae0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1420bdb10=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1420bdb20=GenForwardMissing")
// ---- end of link fallbacks ----

// s_size, reached from SE3.cpp under its own name (one definition per reference global)
int32_t& g_se3CacheSize = s_size;

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142cfcfc8 = (void*)&s_doneLoading;
extern "C" void* const gen_p_142cfcfe0 = (void*)&s_samplesLoading;
extern "C" void* const gen_p_142cfcff8 = (void*)&s_samplesReady;
extern "C" void* const gen_p_142cfd010 = (void*)&s_streamsLoading;
extern "C" void* const gen_p_142cfd028 = (void*)&s_streamsReady;
extern "C" void* const gen_p_142cfd040 = (void*)&s_idle;
extern "C" void* const gen_p_142cfd058 = (void*)&s_released;
extern "C" void* const gen_p_142ddcea8 = (void*)&s_messageLock;
extern "C" void* const gen_p_142ddced0 = (void*)&s_samplesLock;
extern "C" void* const gen_p_142ddcef8 = (void*)&s_streamsLock;
extern "C" void* const gen_p_1430a9720 = (void*)&s_size;
extern "C" void* const gen_p_1430a9728 = (void*)&s_doneLoadingMessages;
extern "C" void* const gen_p_1430a9750 = (void*)&s_asyncLock;
// ---- end of generated globals ----
