#include "ui/OldMovie.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include <windows.h>

#include "blz/Function.hpp"
#include "blz/String.hpp"
#include "blz/Vector.hpp"
#include "cas/CAS.hpp"
#include "client/GxWindow.hpp"
#include "client/LoadingBar.hpp"
#include "console/Var.hpp"
#include "db/DB2Tables.hpp"
#include "db/WowClientDB2.hpp"
#include "db/WowDB2Lookup.hpp"
#include "fileservices/AsyncFile.hpp"
#include "fmod.hpp"
#include "os/Timer.hpp"
#include "services/ClientServices.hpp"
#include "sound/SE3.hpp"
#include "sound/SE3Kit.hpp"
#include "sound/SI3.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "ui/CLayoutFrame.hpp"
#include "ui/CSimpleMovieFrame.hpp"
#include "ui/MovieDecoder.hpp"
#include "util/Unimplemented.hpp"
#include "zlib.h"

using movieplayerv1::CAviParser;
using movieplayerv1::IVideoDecoder;

HSLOG g_movieLog;
// The movies that are open, for the loading screen (DAT_142de0b30).
int32_t g_moviesOpen;  // DAT_142de0b30

// The reference's own, still translated: the focus callbacks (FUN_140237f80 / FUN_1402381d0),
// the audio device listeners (FUN_1412e6330 / FUN_1412e6c70), the glue's audio restart
// (FUN_141917d00), the variation picker (FUN_14062d670) and the world's pending loads
// (FUN_141359dc0, FUN_141359b60).
extern "C" {
int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);
unsigned long long FUN_140237f80(unsigned long long function);
unsigned long long FUN_1402381d0(unsigned long long id);
unsigned long long FUN_1412e6330(unsigned long long restart, unsigned long long shutdown);
unsigned long long FUN_1412e6c70(unsigned long long id);
unsigned long long FUN_141917d00();
unsigned long long FUN_141359dc0();
unsigned long long FUN_141359b60();
unsigned long long FUN_141846870(unsigned long long movieId);
}
// translated: FUN_140237f80
// translated: FUN_1402381d0
// translated: FUN_1412e6330
// translated: FUN_1412e6c70
// translated: FUN_141917d00
// translated: FUN_141359dc0
// translated: FUN_141359b60
// translated: FUN_141846870

namespace {

const char s_oldMovieFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Frame\\OldMovie.cpp";
const char s_handleTag[] = ".PEAUMovieHandle@IOldMoviePlayer@@";
const char s_frameBlocksTag[] = ".?AUSFrameBlockData@MovieHandle@IOldMoviePlayer@@";
const char s_streamBlocksTag[] = ".?AUSStreamingBlock@MovieHandle@IOldMoviePlayer@@";
const char s_captionTag[] = ".?AUMOVIECAPTION@CCaptions@@";
const char s_byteTag[] = ".E";

template <class F>
uint64_t Translated(uint64_t address, F&& body) {
    uint64_t result = 0;
    __try {
        result = body();
    } __except (GenFault(address, GetExceptionInformation())) {
    }
    return result;
}

const char* ResolutionName(int32_t resolution) {
    if (resolution == 0) {
        return "480P";
    }
    if (resolution == 1) {
        return "720P";
    }
    if (resolution == 2) {
        return "1080P";
    }
    return "UNKNOWN";
}

// The default key, "Why hello there, I'm a test key!" with the top bit set (DAT_1426c7c90).
const uint8_t s_defaultKey[0x20] = { 0xd7, 0xc8, 0xd9, 0xa0, 0xc8, 0xc5, 0xcc, 0xcc, 0xcf, 0xa0, 0xd4, 0xc8, 0xc5, 0xd2,
    0xc5, 0xac, 0xa0, 0xc9, 0xa7, 0xcd, 0xa0, 0xc1, 0xa0, 0xd4, 0xc5, 0xd3, 0xd4, 0xa0, 0xcb, 0xc5, 0xd9, 0xa1 };

// Frame rendering on or off (DAT_142cce7cc).
uint8_t s_renderFrames = 1;  // DAT_142cce7cc
// The audio device listeners' id (DAT_142cce7b8).
int32_t s_audioListener;

// The handles that are open (DAT_142e97a70).
blz::vector<CMoviePlayer*, s_handleTag> s_handles;

}  // namespace

// ---- the audio --------------------------------------------------------------------------------

// A movie's audio (IOldMovieAudioStream, vtable 0x1426c7cc0).
class IOldMovieAudioStream {
    public:
    typedef void (*ENDEDPROC)(void* param);

    virtual ~IOldMovieAudioStream() {
    }
    // The file's first block asked for early.
    // ref: FUN_1405fe6b0
    virtual void Prefetch(int32_t fileDataId) {
        AsyncFilePrefetchSpan(fileDataId, 0, 0x40000);
    }
    virtual void Play(int32_t fileDataId, ENDEDPROC ended, void* param, int32_t movieId, bool looping) = 0;
    virtual void Stop() = 0;
    virtual void Restart(uint32_t positionMs) = 0;
    virtual void Shutdown() = 0;
    virtual uint32_t GetPositionMs() = 0;
    virtual void SetPaused(bool paused) = 0;
    // Whether it waits for data (paused by the player).
    virtual bool IsStarved() = 0;
    virtual bool IsPlaying() = 0;
    virtual void Update() = 0;
};

namespace {

// The music, ambience, sfx and dialog buses: muted for the movie, as they were after.
void SaveAndMuteBuses(uint8_t* saved) {
    for (int8_t bus = 0; bus < 4; bus++) {
        saved[bus] = SE3BusIsMuted(bus, false) ? 1 : 0;
    }
    for (int8_t bus = 0; bus < 4; bus++) {
        SE3BusSetMuted(bus, true);
    }
}

void RestoreBuses(const uint8_t* saved) {
    for (int8_t bus = 0; bus < 4; bus++) {
        SE3BusSetMuted(bus, saved[bus] != 0);
    }
}

bool GetDecryptionKey(int32_t movieId, uint8_t* key);

// The audio through the sound engine, a file instance on bus 5 (CSE3AudioStream, 0x30, vtable
// 0x1426c7d18): for movies with a key.
class CSE3AudioStream : public IOldMovieAudioStream {
    public:
    int32_t m_fileDataId = 0;           // 0x08
    int32_t m_handle = 0;               // 0x0C
    int32_t m_movieId = 0;              // 0x10
    uint8_t m_paused = 1;               // 0x14
    uint8_t m_looping = 0;              // 0x15
    ENDEDPROC m_ended = nullptr;        // 0x18
    void* m_endedParam = nullptr;       // 0x20
    uint8_t m_savedMutes[4] = {};       // 0x28

    // ref: FUN_1406008e0
    void Play(int32_t fileDataId, ENDEDPROC ended, void* param, int32_t movieId, bool looping) override {
        this->m_movieId = movieId;
        this->m_fileDataId = fileDataId;
        this->m_looping = looping ? 1 : 0;
        this->m_ended = ended;
        this->m_endedParam = param;
        SaveAndMuteBuses(this->m_savedMutes);
        this->PlayFromPosition(0);
    }

    // ref: FUN_1405fc350
    void Stop() override {
        this->ClearEnded();
        SE3StopNow(static_cast<uint32_t>(this->m_handle), nullptr);
    }

    // ref: FUN_1405fc1b0
    void Restart(uint32_t positionMs) override {
        this->PlayFromPosition(positionMs);
    }

    // ref: FUN_1406009f0
    void Shutdown() override {
        this->ClearEnded();
        SE3StopNow(static_cast<uint32_t>(this->m_handle), nullptr);
        RestoreBuses(this->m_savedMutes);
    }

    // ref: FUN_1405fd2e0
    uint32_t GetPositionMs() override {
        return SE3GetPositionMs(static_cast<uint32_t>(this->m_handle));
    }

    // ref: FUN_1405fe4d0
    void SetPaused(bool paused) override {
        SE3SetPaused(static_cast<uint32_t>(this->m_handle), paused);
        this->m_paused = paused ? 1 : 0;
    }

    // ref: FUN_140317fd0
    bool IsStarved() override {
        return this->m_paused != 0;
    }

    // ref: FUN_14026bea0
    bool IsPlaying() override {
        return true;
    }

    void Update() override {
    }

    // The file played on bus 5 from `positionMs` with the movie's key, its end reported.
    // ref: FUN_1405fe520
    void PlayFromPosition(uint32_t positionMs) {
        uint8_t key[0x20];
        GetDecryptionKey(this->m_movieId, key);
        SE3PLAYPARAMS params;
        SE3PlayParamsConstruct(&params);
        params.m_fileDataId = this->m_fileDataId;
        params.m_bus = 5;
        params.m_135 = 1;
        params.m_136 = 0;
        params.m_13c = 1;
        params.m_24 = 0x5a;
        params.m_duplicates = 2;
        if (this->m_looping != 0) {
            params.m_3dMode = 1;
            params.m_137 = 1;
        }
        params.m_b8 = static_cast<int32_t>(positionMs);
        SE3PlayKit(static_cast<uint32_t>(-0x80), nullptr, &params, &this->m_handle, "CSE3AudioStream::PlayFromPosition",
            true, key);
        blz::function_storage ended;
        ended.m_invoke = reinterpret_cast<void*>(&OnEnded);
        ended.m_manager = &blz::function_trivial_manager;
        CSE3AudioStream* self = this;
        std::memcpy(ended.m_storage, &self, sizeof(self));
        SE3SetOnEnded(static_cast<uint32_t>(this->m_handle), &ended, reinterpret_cast<uint64_t>(this));
        this->m_paused = 0;
        SE3PlayParamsDestroy(&params);
    }

    private:
    void ClearEnded() {
        blz::function_storage none;
        none.m_invoke = nullptr;
        none.m_manager = &blz::function_empty_manager;
        SE3SetOnEnded(static_cast<uint32_t>(this->m_handle), &none, 0);
    }

    // The instance ended: the movie's end (LAB_1405fe500 through the argument).
    static void OnEnded(void* object, void* storage) {
        (void)object;
        CSE3AudioStream* self;
        std::memcpy(&self, storage, sizeof(self));
        if (self->m_ended != nullptr) {
            self->m_ended(self->m_endedParam);
        }
    }
};

// The audio streamed by FMOD itself (CFMODAudioStream, 0x68, vtable 0x1426c7d98): opened
// non-blocking through CAS by its file id, started on the main thread once open, muted when
// the game loses focus unless sound plays in the background.
class CFMODAudioStream : public IOldMovieAudioStream {
    public:
    FMOD::Sound* m_sound = nullptr;             // 0x08
    FMOD::Channel* m_channel = nullptr;         // 0x10
    int32_t m_fileDataId = 0;                   // 0x18
    int32_t m_movieId = 0;                      // 0x1C
    uint8_t m_starved = 1;                      // 0x20
    uint8_t m_muted = 0;                        // 0x21
    uint8_t m_looping = 0;                      // 0x22
    uint32_t m_positionMs = 0;                  // 0x24
    int32_t m_focusId = 0;                      // 0x28
    ENDEDPROC m_ended = nullptr;                // 0x30
    void* m_endedParam = nullptr;               // 0x38
    uint8_t m_savedMutes[4] = {};               // 0x40
    // What FMOD's threads report, run by Update on the main thread (0x48, its lock at 0x60).
    struct QUEUED {
        int32_t m_key;
        void (*m_proc)(CFMODAudioStream* stream);
    };
    std::vector<QUEUED> m_queued;
    std::mutex m_queueLock;

    // ref: FUN_1405fc670 (the FMOD half)
    CFMODAudioStream() {
        blz::function_storage focus;
        focus.m_invoke = reinterpret_cast<void*>(&OnFocusInvoke);
        focus.m_manager = &blz::function_trivial_manager;
        CFMODAudioStream* self = this;
        std::memcpy(focus.m_storage, &self, sizeof(self));
        this->m_focusId = static_cast<int32_t>(
            Translated(0x140237f80, [&] { return FUN_140237f80(reinterpret_cast<uint64_t>(&focus)); }));
    }

    // ref: FUN_1405fc0b0
    ~CFMODAudioStream() override {
        int32_t id = this->m_focusId;
        Translated(0x1402381d0, [&] { return FUN_1402381d0(static_cast<uint64_t>(static_cast<int64_t>(id))); });
        this->Stop();
    }

    // ref: FUN_140600740
    void Play(int32_t fileDataId, ENDEDPROC ended, void* param, int32_t movieId, bool looping) override {
        this->m_fileDataId = fileDataId;
        this->m_looping = looping ? 1 : 0;
        this->m_movieId = movieId;
        this->m_ended = ended;
        this->m_endedParam = param;
        SaveAndMuteBuses(this->m_savedMutes);
        FMOD_CREATESOUNDEXINFO info;
        std::memset(&info, 0, sizeof(info));
        info.cbsize = sizeof(info);
        info.nonblockcallback = &OnOpened;
        info.userdata = this;
        info.suggestedsoundtype = static_cast<FMOD_SOUND_TYPE>(0xc);
        info.useropen = &FileOpen;
        info.userclose = &FileClose;
        info.userread = &FileRead;
        info.userseek = &FileSeek;
        char name[32];
        std::snprintf(name, sizeof(name), "%d", fileDataId);
        FMOD_MODE mode = static_cast<FMOD_MODE>((looping ? 0u : 1u) + 0x820180c8u);
        FMOD_RESULT result = g_fmodSystem->createSound(name, mode, &info, &this->m_sound);
        this->m_starved = 0;
        if (result != FMOD_OK) {
            this->m_sound = nullptr;
        }
    }

    // ref: FUN_1405fc300
    void Stop() override {
        if (this->m_channel != nullptr) {
            this->m_channel->setCallback(nullptr);
            this->m_channel->stop();
        }
        if (this->m_sound != nullptr) {
            this->m_sound->setUserData(nullptr);
            this->m_sound->release();
        }
        this->m_channel = nullptr;
        this->m_sound = nullptr;
    }

    // ref: FUN_1405fc180
    void Restart(uint32_t positionMs) override {
        this->m_positionMs = positionMs;
        this->Play(this->m_fileDataId, this->m_ended, this->m_endedParam, this->m_movieId, this->m_looping != 0);
    }

    // ref: FUN_140600970
    void Shutdown() override {
        RestoreBuses(this->m_savedMutes);
        this->Stop();
    }

    // ref: FUN_1405fd2b0
    uint32_t GetPositionMs() override {
        unsigned int position = 0;
        if (this->m_channel != nullptr && this->m_channel->getPosition(&position, FMOD_TIMEUNIT_MS) == FMOD_OK) {
            return position;
        }
        return 0;
    }

    // ref: FUN_1405fe4c0
    void SetPaused(bool paused) override {
        this->m_starved = paused ? 1 : 0;
        if (this->m_channel != nullptr) {
            this->m_channel->setPaused(paused);
        }
    }

    // ref: FUN_1402b47c0
    bool IsStarved() override {
        return this->m_starved != 0;
    }

    // ref: FUN_1405fdf10
    bool IsPlaying() override {
        return this->m_sound != nullptr;
    }

    // What FMOD reported, run in key order.
    // ref: FUN_140600c80
    void Update() override {
        std::vector<QUEUED> queued;
        {
            std::lock_guard<std::mutex> lock(this->m_queueLock);
            queued.swap(this->m_queued);
        }
        std::stable_sort(queued.begin(), queued.end(), [](const QUEUED& a, const QUEUED& b) {
            return a.m_key < b.m_key;
        });
        for (const QUEUED& each : queued) {
            each.m_proc(this);
        }
    }

    // ref: FUN_1405fea20
    void Queue(void (*proc)(CFMODAudioStream* stream), int32_t key) {
        std::lock_guard<std::mutex> lock(this->m_queueLock);
        this->m_queued.push_back({ key, proc });
    }

    // Open: the channel started paused, at the bus's volume and the saved position, muted if
    // sound is off or the game is not in front, then let go.
    // ref: FUN_1405fcd30
    static void StartChannel(CFMODAudioStream* self) {
        if (self->m_channel != nullptr || self->m_sound == nullptr) {
            return;
        }
        if (g_fmodSystem->playSound(FMOD_CHANNEL_FREE, self->m_sound, true, &self->m_channel) != FMOD_OK) {
            return;
        }
        float volume = SE3BusGetChainVolume(5);
        if (self->m_channel->setVolume(volume) != FMOD_OK) {
            return;
        }
        self->m_channel->setUserData(self);
        self->m_channel->setCallback(&OnChannel);
        if (self->m_positionMs != 0) {
            self->m_channel->setPosition(self->m_positionMs, FMOD_TIMEUNIT_MS);
        }
        if (!SE3BusIsEnabled(0x16) || self->m_muted != 0) {
            self->SetMuted(true);
        }
        self->m_channel->setPaused(false);
    }

    // ref: FUN_1406000c0
    void SetMuted(bool muted) {
        float volume = SE3BusGetChainVolume(5);
        this->m_muted = muted ? 1 : 0;
        if (this->m_channel != nullptr) {
            this->m_channel->setMute(muted);
            this->m_channel->setVolume(volume);
        }
    }

    // ref: FUN_1405fce70
    void OnFocus(bool focused) {
        SLogWrite(g_movieLog, "WowMoviePlayer FMODAudioStreamUpdateFocus");
        bool background = g_soundEnableInBackground != nullptr && g_soundEnableInBackground->m_intValue != 0;
        bool muted = !SE3BusIsEnabled(0x16);
        float volume = SE3BusGetChainVolume(5);
        if (!focused && !background) {
            this->m_muted = 1;
            muted = true;
        } else {
            this->m_muted = muted ? 1 : 0;
        }
        if (this->m_channel == nullptr) {
            return;
        }
        this->m_channel->setMute(muted);
        this->m_channel->setVolume(volume);
    }

    private:
    static void OnFocusInvoke(bool focused, void* storage) {
        CFMODAudioStream* self;
        std::memcpy(&self, storage, sizeof(self));
        self->OnFocus(focused);
    }

    // ref: FUN_1405fcdf0
    static FMOD_RESULT F_CALLBACK OnOpened(FMOD_SOUND* sound, FMOD_RESULT result) {
        if (result != FMOD_OK && result != static_cast<FMOD_RESULT>(0x16)) {
            return static_cast<FMOD_RESULT>(0x13);
        }
        void* data = nullptr;
        reinterpret_cast<FMOD::Sound*>(sound)->getUserData(&data);
        if (data == nullptr) {
            return static_cast<FMOD_RESULT>(0x16);
        }
        static_cast<CFMODAudioStream*>(data)->Queue(&StartChannel, 0);
        return FMOD_OK;
    }

    // ref: FUN_1405fcf50
    static FMOD_RESULT F_CALLBACK OnChannel(FMOD_CHANNEL* channel, FMOD_CHANNEL_CALLBACKTYPE type, void* a, void* b) {
        (void)a;
        (void)b;
        if (type == FMOD_CHANNEL_CALLBACKTYPE_END) {
            void* data = nullptr;
            reinterpret_cast<FMOD::Channel*>(channel)->getUserData(&data);
            if (data != nullptr) {
                // LAB_1405fd180
                static_cast<CFMODAudioStream*>(data)->Queue(&Ended, 1);
            }
        }
        return FMOD_OK;
    }

    static void Ended(CFMODAudioStream* self) {
        if (self->m_ended != nullptr) {
            self->m_ended(self->m_endedParam);
        }
    }

    // FMOD's file system over CAS: the name is the file's id.
    struct FILESTATE {
        int32_t m_fileDataId;
        int32_t m_position;
        int32_t m_prefetched;
    };

    // ref: FUN_1405fcfe0
    static FMOD_RESULT F_CALLBACK FileOpen(const char* name, int unicode, unsigned int* size, void** handle,
        void** userData) {
        (void)unicode;
        int32_t fileDataId = std::atoi(name);
        cas::CASFILE* file = nullptr;
        if (!cas::CASFileOpenById(fileDataId, &file, 0)) {
            return static_cast<FMOD_RESULT>(0x24);
        }
        auto state = static_cast<FILESTATE*>(
            SMemAlloc(sizeof(FILESTATE), 1, s_oldMovieFile, SStrHashFnv1a(s_oldMovieFile), 0x17a, 0));
        state->m_fileDataId = fileDataId;
        state->m_position = 0;
        state->m_prefetched = 0;
        *handle = file;
        *size = static_cast<unsigned int>(cas::CASFileGetSize(file));
        *userData = state;
        return FMOD_OK;
    }

    // ref: FUN_1405fcfb0
    static FMOD_RESULT F_CALLBACK FileClose(void* handle, void* userData) {
        auto file = static_cast<cas::CASFILE*>(handle);
        cas::CASFileClose(&file);
        SMemFree(userData, s_oldMovieFile, -2, 0);
        return FMOD_OK;
    }

    // Read on, and the next 256KB asked for ahead of the reader.
    // ref: FUN_1405fd0c0
    static FMOD_RESULT F_CALLBACK FileRead(void* handle, void* buffer, unsigned int size, unsigned int* read,
        void* userData) {
        uint32_t got = 0;
        if (!cas::CASFileReadEx(static_cast<cas::CASFILE*>(handle), buffer, size, &got, 0, -1)) {
            return static_cast<FMOD_RESULT>(0x21);
        }
        *read = got;
        if (size != got) {
            return static_cast<FMOD_RESULT>(0x16);
        }
        auto state = static_cast<FILESTATE*>(userData);
        state->m_position += static_cast<int32_t>(got);
        int32_t block = state->m_position / 0x40000 + 1;
        if (block != state->m_prefetched) {
            AsyncFilePrefetchSpan(state->m_fileDataId, static_cast<uint32_t>(block * 0x40000), 0x40000);
            state->m_prefetched = block;
        }
        return FMOD_OK;
    }

    // ref: FUN_1405fd150
    static FMOD_RESULT F_CALLBACK FileSeek(void* handle, unsigned int position, void* userData) {
        cas::CASFileSetPosition(static_cast<cas::CASFILE*>(handle), position, 0);
        static_cast<FILESTATE*>(userData)->m_position = static_cast<int32_t>(position);
        return FMOD_OK;
    }
};

// A movie's audio: through the sound engine when its record has a key, otherwise FMOD.
// ref: FUN_1405fc670
IOldMovieAudioStream* AudioStreamCreate(int32_t movieId) {
    bool local = false;
    auto movie = static_cast<const uint8_t*>(g_movieDB.GetRecord(static_cast<uint32_t>(movieId), false, &local));
    if (movie != nullptr && DB2FieldU8(movie, 1, 0, &g_movieDB) != 0) {
        void* memory = SMemAlloc(sizeof(CSE3AudioStream), 1, s_oldMovieFile, SStrHashFnv1a(s_oldMovieFile), 0x2c3, 0);
        return new (memory) CSE3AudioStream();
    }
    void* memory = SMemAlloc(sizeof(CFMODAudioStream), 1, s_oldMovieFile, SStrHashFnv1a(s_oldMovieFile), 0x2c4, 0);
    return new (memory) CFMODAudioStream();
}

void AudioStreamFree(IOldMovieAudioStream* stream) {
    stream->~IOldMovieAudioStream();
    SMemFree(stream, s_oldMovieFile, -2, 0);
}

}  // namespace

// ---- the handle ------------------------------------------------------------------------------

namespace {

// A frame's blocks: the first and how many (SFrameBlockData, 8).
struct FRAMEBLOCKS {
    uint32_t m_first;
    uint32_t m_count;
};

// A block of the file (SStreamingBlock, 0x18): flag 1 on disk, 2 being read.
struct STREAMBLOCK {
    uint32_t m_offset;
    uint32_t m_size;
    uint8_t m_flags;
    uint8_t m_pad09[7];
    CAsyncObject* m_request;
};
static_assert(sizeof(STREAMBLOCK) == 0x18, "STREAMBLOCK is 0x18 bytes");

// One encoding of the movie (0xd8).
struct MOVIERESOLUTION {
    CAviParser m_parser;                                    // 0x00
    IVideoDecoder* m_decoder = nullptr;                     // 0x98
    blz::vector<FRAMEBLOCKS, s_frameBlocksTag> m_frames;    // 0xA0
    blz::vector<STREAMBLOCK, s_streamBlocksTag> m_blocks;   // 0xB8
    uint8_t m_preventSwitching = 0;                         // 0xD0
};

// A caption (CCaptions::MOVIECAPTION, 0x38): shown from start to end (ms), even with captions
// off when forced.
struct MOVIECAPTION {
    uint8_t m_force;
    int32_t m_start;
    int32_t m_end;
    blz::string m_text;
};

}  // namespace

const uint8_t* MovieGetVariation(int32_t movieId, bool localOnly);

struct CMoviePlayer {
    CSimpleMovieFrame* m_frame = nullptr;                   // 0x000
    float m_fps = 0.0f;                                     // 0x008
    int32_t m_frameCount = 0;                               // 0x00C
    CAviParser m_overlay;                                   // 0x010
    IOldMovieAudioStream* m_audio = nullptr;                // 0x0A8
    blz::vector<MOVIECAPTION, s_captionTag> m_captions;     // 0x0B0
    int32_t m_caption = -1;                                 // 0x0C8
    CSimpleMovieFrame* m_captionFrame = nullptr;            // 0x0D0
    MOVIERESOLUTION m_resolutions[3];                       // 0x0D8
    uint8_t m_key[0x20] = {};                               // 0x360
    uint32_t m_keyId = 0;                                   // 0x380
    uint8_t m_keyValid = 0;                                 // 0x384
    uint8_t m_keyFailed = 0;                                // 0x385
    int32_t m_keyDeadline = 0;                              // 0x388
    int32_t m_startTime = 0;                                // 0x38C
    int32_t m_movieId = 0;                                  // 0x390
    int32_t m_frameIndex = 0;                               // 0x394
    int32_t m_resolution = 0;                               // 0x398
    int32_t m_pendingResolution = 0;                        // 0x39C
    int32_t m_skipped = 0;                                  // 0x3A8
    int32_t m_switchFrame = 0;                              // 0x3AC
    uint8_t m_looping = 0;                                  // 0x3B0
    blz::vector<uint8_t, s_byteTag> m_buffer;               // 0x3B8 a frame's bytes
};

namespace {

void StopMovie(CMoviePlayer* player);
void RenderFrame(CMoviePlayer* player);
void WaitForKey(CMoviePlayer* player);

// The frame's render callback: {procedure, handle} (LAB_140601040 over FUN_140601070's storage).
struct RENDERCLOSURE {
    void (*m_proc)(CMoviePlayer* player);
    CMoviePlayer* m_player;
};

void InvokeRender(void* storage) {
    RENDERCLOSURE closure;
    std::memcpy(&closure, storage, sizeof(closure));
    if (closure.m_proc != nullptr) {
        closure.m_proc(closure.m_player);
    }
}

// ref: FUN_140600120
void SetRenderCallback(CSimpleMovieFrame* frame, void (*proc)(CMoviePlayer* player), CMoviePlayer* player) {
    frame->m_renderFrame.m_manager(frame->m_renderFrame.m_storage, nullptr, 0);
    if (proc == nullptr) {
        frame->m_renderFrame.m_invoke = nullptr;
        frame->m_renderFrame.m_manager = &blz::function_empty_manager;
        return;
    }
    RENDERCLOSURE closure = { proc, player };
    std::memcpy(frame->m_renderFrame.m_storage, &closure, sizeof(closure));
    frame->m_renderFrame.m_invoke = reinterpret_cast<void*>(&InvokeRender);
    frame->m_renderFrame.m_manager = &blz::function_trivial_manager;
}

// The key a movie's record names in Keychain, or the default key; false when the record names
// one that is not there.
// ref: FUN_1405fd2f0
bool GetDecryptionKey(int32_t movieId, uint8_t* key) {
    std::memcpy(key, s_defaultKey, sizeof(s_defaultKey));
    bool local = false;
    auto movie = static_cast<const uint8_t*>(g_movieDB.GetRecord(static_cast<uint32_t>(movieId), false, &local));
    if (movie != nullptr && DB2FieldU8(movie, 1, 0, &g_movieDB) != 0) {
        uint8_t keyId = DB2FieldU8(movie, 1, 0, &g_movieDB);
        auto record = static_cast<const uint8_t*>(g_keychainDB.GetRecord(keyId, false, &local));
        if (record == nullptr) {
            return false;
        }
        std::memcpy(key, record, 0x20);
    }
    return true;
}

// The handle's key: the default, or the record's from Keychain (asked of the server when it is
// not local). True when it is known now.
// ref: FUN_1405fd3b0
bool LoadKey(CMoviePlayer* player) {
    std::memcpy(player->m_key, s_defaultKey, sizeof(s_defaultKey));
    bool local = false;
    auto movie = static_cast<const uint8_t*>(g_movieDB.GetRecord(static_cast<uint32_t>(player->m_movieId), false, &local));
    if (movie == nullptr || DB2FieldU8(movie, 1, 0, &g_movieDB) == 0) {
        return true;
    }
    uint8_t keyId = DB2FieldU8(movie, 1, 0, &g_movieDB);
    player->m_keyId = keyId;
    auto record = static_cast<const uint8_t*>(g_keychainDB.GetRecord(keyId, true, &local));
    if (!local) {
        FROST_UNIMPLEMENTED();  // FUN_1405fb950: Keychain's record asked of the server (offline)
        player->m_keyFailed = 1;
        return false;
    }
    if (record != nullptr) {
        std::memcpy(player->m_key, record, 0x20);
        return true;
    }
    player->m_keyFailed = 1;
    return false;
}

// A movie's file for a resolution: the variation whose file is that wide (640, 1280, 1920),
// or for 720P one 1024 wide, when the file is there.
// ref: FUN_1405fd5c0
bool FindMovieFile(int32_t movieId, int32_t resolution, int32_t* fileDataId) {
    static const uint16_t s_widths[3] = { 640, 1280, 1920 };  // DAT_1426c7cb0
    uint16_t width = s_widths[resolution];
    if (DB2ParentCount(&g_movieVariationDB, movieId) == 0) {
        return false;
    }
    WowDB2ParentLookup* lookup = g_movieVariationDB.m_parentLookup;
    if (lookup == nullptr) {
        return false;
    }
    for (int32_t pass = 0; pass < (resolution == 1 ? 2 : 1); pass++) {
        uint16_t want = pass == 0 ? width : 0x400;
        DB2PARENTITER it;
        DB2PARENTITER end;
        lookup->Begin(&it, movieId);
        lookup->End(&end);
        while (std::memcmp(&it, &end, sizeof(it)) != 0) {
            auto variation = static_cast<const uint8_t*>(lookup->Record(&it));
            uint32_t file = DB2FieldU32(variation, 0, 0, &g_movieVariationDB);
            bool local = false;
            auto data = static_cast<const uint8_t*>(g_movieFileDataDB.GetRecord(file, false, &local));
            if (data != nullptr && DB2FieldU16(data, 0, 0, &g_movieFileDataDB) == want
                && cas::CASFileExists(static_cast<int32_t>(file))) {
                *fileDataId = static_cast<int32_t>(file);
                return true;
            }
            lookup->Next(&it);
        }
    }
    return false;
}

// Each encoding's blocks, whether on disk, and each frame's run of them.
// ref: FUN_1405fd900
void InitStreamingBlocks(CMoviePlayer* player) {
    SLogWrite(g_movieLog, "WowMoviePlayer MovieHandle::InitStreamingBlocks");
    for (MOVIERESOLUTION& res : player->m_resolutions) {
        CAviParser& parser = res.m_parser;
        if (parser.m_file == nullptr) {
            continue;
        }
        res.m_blocks.resize(parser.m_blocks.size());
        for (uint64_t i = 0; i < parser.m_blocks.size(); i++) {
            STREAMBLOCK& block = res.m_blocks[i];
            block.m_offset = parser.m_blocks[i].m_offset;
            block.m_size = parser.m_blocks[i].m_size;
            block.m_flags = cas::CASFileGetSpanResidency(parser.m_file, block.m_offset, block.m_size) == 2 ? 1 : 0;
            block.m_request = nullptr;
        }
        res.m_frames.resize(static_cast<uint64_t>(parser.m_frameCount));
        uint32_t b = 0;
        for (int32_t f = 0; f < parser.m_frameCount; f++) {
            uint32_t start = static_cast<uint32_t>(parser.m_moviOffset) + 4 + parser.m_index[f].m_offset;
            while (res.m_blocks[b].m_offset + res.m_blocks[b].m_size < start) {
                b++;
            }
            res.m_frames[f].m_first = b;
            res.m_frames[f].m_count = 1;
            while (res.m_blocks[b].m_offset + res.m_blocks[b].m_size < start + parser.m_index[f].m_size) {
                b++;
                res.m_frames[f].m_count++;
            }
        }
    }
}

// Whether every block of a frame of an encoding is on disk.
// ref: FUN_1405fdec0
bool IsFrameResident(CMoviePlayer* player, int32_t resolution, int32_t frame) {
    MOVIERESOLUTION& res = player->m_resolutions[resolution];
    uint32_t first = res.m_frames[frame].m_first;
    for (uint32_t b = first; b < first + res.m_frames[frame].m_count; b++) {
        if ((res.m_blocks[b].m_flags & 1) == 0) {
            return false;
        }
    }
    return true;
}

// An encoding's parser and decoder: 4 when the file is not a movie, 3 for a codec it has no
// decoder for, 1 when the decoder does not start.
// ref: FUN_1405fdd10
int32_t InitVideoDecoder(CMoviePlayer* player, int32_t fileDataId, int32_t resolution) {
    SLogWrite(g_movieLog, "WowMoviePlayer MovieHandle::InitVideoDecoder(%d, %s)", fileDataId, ResolutionName(resolution));
    MOVIERESOLUTION& res = player->m_resolutions[resolution];
    res.m_parser.SetFile(fileDataId);
    if (!res.m_parser.Open()) {
        return 4;
    }
    res.m_decoder = movieplayerv1::VideoDecoderCreate(res.m_parser.m_fourcc);
    if (res.m_decoder == nullptr) {
        res.m_parser.Close();
        return 3;
    }
    return res.m_decoder->SetSize(res.m_parser.m_width, res.m_parser.m_height) ? 0 : 1;
}

// The frames of an encoding from `frame` on, asked for from CAS where not on disk.
// ref: FUN_1405fe800
void PrefetchVideo(CMoviePlayer* player, int32_t resolution, int32_t frame) {
    MOVIERESOLUTION& res = player->m_resolutions[resolution];
    SLogWrite(g_movieLog, "WowMoviePlayer MovieHandle::PrefetchVideo isFirstFrameKeyFrame = %s",
        (res.m_parser.m_index[frame].m_flags >> 4 & 1) != 0 ? "true" : "false");
    for (int32_t f = frame; f < res.m_parser.m_frameCount; f++) {
        uint32_t first = res.m_frames[f].m_first;
        uint32_t count = res.m_frames[f].m_count;
        bool resident = true;
        for (uint32_t b = first; b < first + count; b++) {
            if ((res.m_blocks[b].m_flags & 1) == 0) {
                resident = false;
                break;
            }
        }
        if (resident) {
            continue;
        }
        for (uint32_t b = first; b < first + count; b++) {
            STREAMBLOCK& block = res.m_blocks[b];
            if ((block.m_flags & 3) != 0) {
                continue;
            }
            if (cas::CASFileGetSpanResidency(res.m_parser.m_file, block.m_offset, block.m_size) == 2) {
                block.m_flags |= 1;
            } else {
                block.m_flags = 2;
                block.m_request = AsyncFileReadFromFile(res.m_parser.m_file, nullptr, block.m_offset, block.m_size, 0,
                    &block, nullptr, nullptr,
                    // LAB_1405fea00
                    [](void* data, void*, uint32_t) {
                        auto done = static_cast<STREAMBLOCK*>(data);
                        done->m_request = nullptr;
                        done->m_flags = static_cast<uint8_t>((done->m_flags & 0xfd) | 1);
                    },
                    // LAB_1405fe9f0
                    [](void* data, void*, uint32_t, const char*) {
                        auto failed = static_cast<STREAMBLOCK*>(data);
                        failed->m_flags &= 0xfd;
                        failed->m_request = nullptr;
                    },
                    [](void* data, void*, uint32_t) {
                        auto canceled = static_cast<STREAMBLOCK*>(data);
                        canceled->m_flags &= 0xfd;
                        canceled->m_request = nullptr;
                    });
            }
        }
    }
}

// The reads still out for an encoding cancelled from `frame` on (and waited for).
// ref: FUN_1405fe6c0
void PrefetchCancel(CMoviePlayer* player, int32_t resolution, int32_t frame, bool wait) {
    SLogWrite(g_movieLog, "WowMoviePlayer MovieHandle::PrefetchCancel(%s, %d, %s)", ResolutionName(resolution), frame,
        wait ? "true" : "false");
    MOVIERESOLUTION& res = player->m_resolutions[resolution];
    if (res.m_parser.m_frameCount <= frame) {
        return;
    }
    uint32_t first = res.m_frames[frame].m_first;
    for (uint64_t b = first; b < res.m_blocks.size(); b++) {
        if (res.m_blocks[b].m_request != nullptr) {
            AsyncFileCancel(res.m_blocks[b].m_request);
        }
    }
    if (wait) {
        for (uint64_t b = first; b < res.m_blocks.size(); b++) {
            if (res.m_blocks[b].m_request != nullptr) {
                AsyncFileWait(res.m_blocks[b].m_request);
            }
        }
    }
}

// The first frame at or after `frame` that is not on disk in the current encoding.
uint32_t FirstMissingFrame(CMoviePlayer* player, uint32_t frame) {
    MOVIERESOLUTION& res = player->m_resolutions[player->m_resolution];
    int32_t count = res.m_parser.m_frameCount;
    if (static_cast<uint32_t>(count - 1) < frame) {
        frame = static_cast<int32_t>(frame) < 0 ? 0 : static_cast<uint32_t>(count - 1);
    }
    for (; static_cast<int32_t>(frame) < count; frame++) {
        if (!IsFrameResident(player, player->m_resolution, static_cast<int32_t>(frame))) {
            break;
        }
    }
    return frame;
}

// The switch to another encoding asked for at `frame`: the current one's reads from where it is
// short cancelled, the new one's frames asked for.
// ref: FUN_1405ffa00
void RequestResolution(CMoviePlayer* player, int32_t resolution, uint32_t frame) {
    SLogWrite(g_movieLog, "WowMoviePlayer MovieHandle::RequestResolution(%s, %d)", ResolutionName(resolution), frame);
    uint32_t missing = FirstMissingFrame(player, frame);
    PrefetchCancel(player, player->m_resolution, static_cast<int32_t>(missing), false);
    PrefetchVideo(player, resolution, static_cast<int32_t>(frame));
    player->m_pendingResolution = resolution;
}

// The next encoding down, from its keyframe at or before the first frame the current one lacks.
// ref: FUN_1405ff880
void RequestLowerResolution(CMoviePlayer* player) {
    SLogWrite(g_movieLog, "WowMoviePlayer MovieHandle::RequestLowerResolution");
    int32_t lower = player->m_resolution - 1;
    if (lower < 0 || player->m_resolutions[lower].m_parser.m_file == nullptr) {
        return;
    }
    uint32_t frame = FirstMissingFrame(player, static_cast<uint32_t>(player->m_frameIndex));
    uint32_t last = static_cast<uint32_t>(player->m_resolutions[lower].m_parser.m_frameCount - 1);
    if (last < frame) {
        frame = static_cast<int32_t>(frame) < 0 ? 0 : last;
    }
    CAviParser& current = player->m_resolutions[player->m_resolution].m_parser;
    while (frame != 0 && (current.m_index[frame].m_flags >> 4 & 1) == 0) {
        frame--;
    }
    RequestResolution(player, lower, frame);
}

// Whether the bytes still to come for an encoding, at the bandwidth CAS measures, arrive before
// they are needed from `frame` on.
// ref: FUN_1405fdf20
bool IsStreamPlayable(CMoviePlayer* player, int32_t resolution, int32_t frame) {
    MOVIERESOLUTION& res = player->m_resolutions[resolution];
    uint32_t bandwidth = static_cast<uint32_t>(static_cast<int64_t>(cas::CASGetBytesPerSecond()));
    if (res.m_preventSwitching != 0) {
        SLogWrite(g_movieLog, "WowMoviePlayer MovieHandle::IsStreamPlayable(%s, %d) PreventSwitching is true",
            ResolutionName(resolution), frame);
        return false;
    }
    uint32_t missingBytes = 0;
    for (uint64_t b = res.m_frames[frame].m_first; b < res.m_blocks.size(); b++) {
        if ((res.m_blocks[b].m_flags & 1) == 0) {
            missingBytes += res.m_blocks[b].m_size;
        }
    }
    int32_t count = res.m_parser.m_frameCount;
    uint32_t required = static_cast<uint32_t>(static_cast<int64_t>(
        (static_cast<float>(missingBytes) * res.m_parser.m_fps) / static_cast<float>(count - player->m_frameIndex)));
    if (bandwidth < required) {
        SLogWrite(g_movieLog,
            "WowMoviePlayer MovieHandle::IsStreamPlayable(%s, %d) failed Frame %d, BytesPerSec %d, BandwidthRequired %d",
            ResolutionName(resolution), frame, player->m_frameIndex, bandwidth, required);
        return false;
    }
    float seconds = 0.0f;
    uint32_t b = res.m_frames[frame].m_first;
    for (int32_t f = frame + 1, n = 1; f < count; f++, n++) {
        uint32_t end = res.m_frames[f].m_first + res.m_frames[f].m_count;
        for (; b < end; b++) {
            if ((res.m_blocks[b].m_flags & 1) == 0) {
                seconds += static_cast<float>(res.m_blocks[b].m_size / bandwidth);
            }
        }
        if (static_cast<float>(n) * (1.0f / res.m_parser.m_fps) < seconds) {
            SLogWrite(g_movieLog,
                "WowMoviePlayer MovieHandle::IsStreamPlayable(%s, %d) specific check failed Frame %d, BytesPerSec %d, "
                "BandwidthRequired %d",
                ResolutionName(resolution), frame, player->m_frameIndex, bandwidth, required);
            return false;
        }
    }
    return true;
}

// The frame from which the next encoding up could be played without running short, or -1.
// ref: FUN_1406001a0
int32_t ShouldRequestHigherResolution(CMoviePlayer* player) {
    if (player->m_pendingResolution != -1) {
        return -1;
    }
    int32_t current = player->m_resolution;
    if (current == 2 || player->m_resolutions[current + 1].m_parser.m_file == nullptr) {
        return -1;
    }
    MOVIERESOLUTION& here = player->m_resolutions[current];
    MOVIERESOLUTION& up = player->m_resolutions[current + 1];
    if (here.m_parser.m_pviv != nullptr || up.m_preventSwitching != 0) {
        return -1;
    }
    uint32_t bandwidth = static_cast<uint32_t>(static_cast<int64_t>(cas::CASGetBytesPerSecond()));
    if (bandwidth == 0) {
        bandwidth = 1;
    }
    int32_t count = up.m_parser.m_frameCount;
    int32_t waited = 0;
    for (int32_t frame = player->m_frameIndex; frame < count; frame++, waited++) {
        if ((here.m_blocks[here.m_frames[frame].m_first].m_flags & 1) == 0) {
            continue;
        }
        if ((up.m_parser.m_index[frame].m_flags >> 4 & 1) == 0) {
            continue;
        }
        float half = static_cast<float>(bandwidth) * 0.5f;
        float seconds = 0.0f;
        uint32_t b = up.m_frames[frame].m_first;
        uint32_t end = b + up.m_frames[frame].m_count;
        for (; b < end; b++) {
            if ((up.m_blocks[b].m_flags & 1) == 0) {
                seconds += static_cast<float>(up.m_blocks[b].m_size) / half;
            }
        }
        float fps = up.m_parser.m_fps;
        if (static_cast<float>(waited) / fps < seconds) {
            continue;
        }
        bool fits = true;
        int32_t n = frame - player->m_frameIndex + 1;
        for (int32_t f = frame + 1; f < count; f++, n++) {
            uint32_t fend = up.m_frames[f].m_first + up.m_frames[f].m_count;
            for (; b < fend; b++) {
                if ((up.m_blocks[b].m_flags & 1) == 0) {
                    seconds += (1.25f / static_cast<float>(bandwidth)) * static_cast<float>(up.m_blocks[b].m_size);
                }
            }
            if (static_cast<float>(n) * (1.0f / fps) < seconds) {
                fits = false;
                break;
            }
        }
        if (fits) {
            SLogWrite(g_movieLog,
                "WowMoviePlayer MovieHandle::ShouldRequestHigherResolution Frame %d res %s (%d bytes/sec) returns true at "
                "Frame %d",
                player->m_frameIndex, ResolutionName(current + 1), bandwidth, frame);
            return frame;
        }
    }
    return -1;
}

// The encoding to start in: the one with the least still to stream, then down while it decodes
// too slowly (a keyframe over 70ms, or ten frames over 200ms, in the first hundred).
// ref: FUN_1405ffb40
void SelectResolution(CMoviePlayer* player) {
    int32_t best = -1;
    int32_t bestMissing = 0x7fffffff;
    for (int32_t r = 2; r >= 0; r--) {
        MOVIERESOLUTION& res = player->m_resolutions[r];
        if (res.m_parser.m_file == nullptr) {
            continue;
        }
        int32_t total = 0;
        int32_t resident = 0;
        for (uint64_t b = 0; b < res.m_blocks.size(); b++) {
            total += static_cast<int32_t>(res.m_blocks[b].m_size);
            if ((res.m_blocks[b].m_flags & 1) != 0) {
                resident += static_cast<int32_t>(res.m_blocks[b].m_size);
            }
        }
        int32_t size = static_cast<int32_t>(cas::CASFileGetSize(res.m_parser.m_file));
        int32_t missing = size - (size + (resident - total));
        if (missing < bestMissing) {
            best = r;
            bestMissing = missing;
        }
    }
    player->m_resolution = best;
    PrefetchVideo(player, best, 0);
    for (;;) {
        int32_t current = player->m_resolution;
        if (current == 0) {
            break;
        }
        MOVIERESOLUTION& res = player->m_resolutions[current];
        bool slow = false;
        if (res.m_parser.m_file != nullptr) {
            int32_t times[10] = {};
            int32_t limit = res.m_parser.m_frameCount < 100 ? res.m_parser.m_frameCount : 100;
            for (int32_t f = 0; f < limit; f++) {
                if (!IsFrameResident(player, current, f)) {
                    break;
                }
                uint32_t read = 0;
                const movieplayerv1::AVIINDEXENTRY& entry = res.m_parser.m_index[f];
                cas::CASFileReadAtOffset(res.m_parser.m_file, player->m_buffer.data(),
                    static_cast<uint32_t>(res.m_parser.m_moviOffset) + 4 + entry.m_offset, entry.m_size, &read, 0, -1);
                uint32_t before = OsGetAsyncTimeMs();
                res.m_decoder->DecodeFrame(player->m_buffer.data(), read);
                uint32_t took = OsGetAsyncTimeMs() - before;
                if ((entry.m_flags >> 4 & 1) != 0 && 0x46 < took) {
                    slow = true;
                    break;
                }
                times[f % 10] = static_cast<int32_t>(took);
                int32_t sum = 0;
                for (int32_t t : times) {
                    sum += t;
                }
                if (200 < sum) {
                    slow = true;
                    break;
                }
            }
        }
        bool lower = false;
        for (int32_t r = current - 1; r >= 0; r--) {
            if (player->m_resolutions[r].m_parser.m_file != nullptr) {
                lower = true;
            }
        }
        if (!slow || !lower) {
            break;
        }
        res.m_preventSwitching = 1;
        PrefetchCancel(player, current, 0, true);
        player->m_resolution = current - 1;
        PrefetchVideo(player, player->m_resolution, 0);
    }
    SLogWrite(g_movieLog, "WowMoviePlayer MovieHandle::SelectResolution %s selected", ResolutionName(player->m_resolution));
}

// ---- captions --------------------------------------------------------------------------------

// A timecode at `text`: hours, minutes, seconds and a last field (each 0x-prefixed hex or
// decimal), the last scaled by `lastScale` into milliseconds.
int32_t ParseTimecode(const char* text, int32_t lastScale) {
    auto field = [](const char* at) {
        int32_t base = at[0] == '0' && (at[1] == 'x' || at[1] == 'X') ? 16 : 10;
        return static_cast<int32_t>(std::strtol(at, nullptr, base));
    };
    return field(text + 9) * lastScale + (field(text + 6) + (field(text + 3) + field(text) * 60) * 60) * 1000;
}

// The next line of `cursor` into `line` (SStrTokenize over line breaks).
bool NextLine(const char** cursor, char* line, uint32_t size) {
    SStrTokenize(cursor, line, size, "\r\n", nullptr);
    return line[0] != '\0';
}

void AddCaption(blz::vector<MOVIECAPTION, s_captionTag>* captions, uint8_t force, int32_t start, int32_t end,
    const char* text) {
    MOVIECAPTION& caption = captions->emplace_back();
    caption.m_force = force;
    caption.m_start = start;
    caption.m_end = end;
    caption.m_text.assign(text);
}

// Captions as "HH:MM:SS:FF ... HH:MM:SS:FF" lines (frames at the movie's rate), each followed by
// its text, which "[FORCE_SHOW]" may precede.
// ref: FUN_1406010a0
void ParseFrameCaptions(blz::vector<MOVIECAPTION, s_captionTag>* captions, const char* text, float fps) {
    char line[0x400];
    const char* cursor = text;
    do {
        NextLine(&cursor, line, sizeof(line));
        if (line[0] == '0') {
            int32_t perFrame = 1000 / static_cast<int32_t>(fps);
            int32_t start = ParseTimecode(line, perFrame);
            int32_t end = ParseTimecode(line + 14, perFrame);
            NextLine(&cursor, line, sizeof(line));
            uint8_t force = 0;
            if (_strnicmp("[FORCE_SHOW]", line, 0xc) == 0) {
                force = 1;
                NextLine(&cursor, line, sizeof(line));
            }
            AddCaption(captions, force, start, end, line);
        }
    } while (line[0] != '\0' && *cursor != '\0');
}

// Captions in SRT: a number, "HH:MM:SS,mmm --> HH:MM:SS,mmm", the text lines.
// ref: FUN_140601630
void ParseSrtCaptions(blz::vector<MOVIECAPTION, s_captionTag>* captions, const char* text) {
    char line[0x400];
    const char* cursor = text;
    int32_t state = 1;
    bool open = false;
    int32_t start = 0;
    int32_t end = 0;
    std::string body;
    for (;;) {
        NextLine(&cursor, line, sizeof(line));
        if (std::atoi(line) == 0 || state == 0) {
            if (open) {
                if (state == 0) {
                    start = ParseTimecode(line, 1);
                    end = ParseTimecode(line + 17, 1);
                } else if (state == 1) {
                    body = line;
                } else {
                    body += "\n";
                    body += line;
                }
                state++;
            }
        } else {
            state = 0;
            if (open) {
                AddCaption(captions, 0, start, end, body.c_str());
            }
            open = true;
            start = 0;
            end = 0;
            body.clear();
        }
        if (line[0] == '\0' || *cursor == '\0') {
            if (open) {
                AddCaption(captions, 0, start, end, body.c_str());
            }
            return;
        }
    }
}

// A movie's caption file, decrypted and inflated when packed ('MNPZ' / 'MNPF'), past a UTF-8
// mark, parsed by its format (7 timecoded, 0x76 SRT).
// ref: FUN_1405fe2d0
void LoadCaptions(CMoviePlayer* player, int32_t fileDataId, float fps, int32_t movieId, CSimpleMovieFrame* frame,
    int32_t format) {
    player->m_captionFrame = frame;
    uint8_t key[0x20];
    GetDecryptionKey(movieId, key);
    uint8_t* data = nullptr;
    uint32_t size = 0;
    if (!cas::CASFileLoadById(fileDataId, &data, &size, 2, 0x22)) {
        return;
    }
    uint32_t magic;
    std::memcpy(&magic, data, 4);
    char* text = reinterpret_cast<char*>(data);
    char* owned = nullptr;
    if (magic == 0x5a504e4d || magic == 0x46504e4d) {
        uint32_t length;
        std::memcpy(&length, data + 0x24, 4);
        owned = static_cast<char*>(SMemAlloc(length + 1, 1, s_oldMovieFile, SStrHashFnv1a(s_oldMovieFile), 0x2f0, 0));
        uint32_t packed = size - 0x28;
        FROST_UNIMPLEMENTED();  // FUN_1420bd960: the caption bytes decrypted with the key and the header's IV
        if (magic == 0x5a504e4d) {
            uLongf out = length;
            uncompress(reinterpret_cast<Bytef*>(owned), &out, data + 0x28, packed);
        } else {
            std::memcpy(owned, data + 0x28, packed);
        }
        owned[length] = '\0';
        text = owned;
    }
    if (static_cast<uint8_t>(text[0]) == 0xef) {
        text += 3;
    }
    if (format == 7) {
        ParseFrameCaptions(&player->m_captions, text, fps);
    } else if (format == 0x76) {
        ParseSrtCaptions(&player->m_captions, text);
    }
    cas::CASFileFree(data);
    SMemFree(owned, s_oldMovieFile, 0x311, 0);
}

// A caption shown (2) or hidden (3) on the frame's next update.
// ref: FUN_1405fee40
void QueueCaption(CSimpleMovieFrame* frame, int32_t type, const char* text) {
    CSimpleMovieFrame::QueuedScript& script = frame->m_queuedScripts.emplace_back();
    script.m_type = type;
    script.m_text.assign(text);
}

// ---- playing ---------------------------------------------------------------------------------

// The movie under way: captions loaded, the encoding chosen, the audio started from the
// record's file, the clock taken, and the frame's callback switched to rendering.
// ref: FUN_1405fc390
void BeginPlaying(CMoviePlayer* player) {
    SLogWrite(g_movieLog, "WowMoviePlayer MovieHandle::BeginPlaying");
    uint8_t key[0x20];
    if (!GetDecryptionKey(player->m_movieId, key)) {
        SLogWrite(g_movieLog, "WowMoviePlayer MovieHandle::BeginPlaying GetDecryptionKey failed");
        StopMovie(player);
        return;
    }
    for (MOVIERESOLUTION& res : player->m_resolutions) {
        if (res.m_parser.m_pviv != nullptr) {
            res.m_decoder->SetKey(key, res.m_parser.m_pviv);
        }
    }
    SelectResolution(player);
    for (MOVIERESOLUTION& res : player->m_resolutions) {
        if (res.m_parser.m_pviv != nullptr) {
            res.m_decoder->SetKey(key, res.m_parser.m_pviv);
        }
    }
    bool local = false;
    auto movie = static_cast<const uint8_t*>(g_movieDB.GetRecord(static_cast<uint32_t>(player->m_movieId), false, &local));
    if (movie != nullptr) {
        int32_t captions = static_cast<int32_t>(DB2FieldU32(movie, 3, 0, &g_movieDB));
        if (cas::CASFileExists(captions)) {
            SLogWrite(g_movieLog, "WowMoviePlayer MovieHandle::BeginPlaying Opening Subtitles");
            LoadCaptions(player, captions, player->m_fps, player->m_movieId, player->m_frame, 7);
        }
        int32_t audio = static_cast<int32_t>(DB2FieldU32(movie, 2, 0, &g_movieDB));
        if (0 < audio) {
            SLogWrite(g_movieLog, "WowMoviePlayer MovieHandle::BeginPlaying Starting Audio");
            // LAB_1405fe2c0: the audio's end is the movie's
            player->m_audio->Play(audio,
                [](void* frame) {
                    static_cast<CSimpleMovieFrame*>(frame)->OnMovieFinished();
                },
                player->m_frame, player->m_movieId, player->m_looping != 0);
        }
    }
    player->m_startTime = static_cast<int32_t>(OsGetAsyncTimeMs());
    SetRenderCallback(player->m_frame, &RenderFrame, player);
    player->m_frame->m_329 = 1;
}

// Before playing: the key known (or given up on after the deadline) and the first frame's
// bytes readable.
// ref: FUN_140600e60
void WaitForKey(CMoviePlayer* player) {
    SLogWrite(g_movieLog, "WowMoviePlayer WaitForKey");
    SLogWrite(g_movieLog, "WowMoviePlayer MovieHandle::WaitForKey m_KeyValid = %s", player->m_keyValid ? "true" : "false");
    if (player->m_keyValid != 0) {
        SLogWrite(g_movieLog, "WowMoviePlayer MovieHandle::GetTactKey");
        int32_t r = 0;
        while (r < 3 && player->m_resolutions[r].m_parser.m_file == nullptr) {
            r++;
        }
        CAviParser& parser = player->m_resolutions[r].m_parser;
        uint32_t offset = static_cast<uint32_t>(parser.m_moviOffset) + 4 + parser.m_index[0].m_offset;
        uint32_t size = parser.m_index[0].m_size;
        if (1 < parser.m_frameCount) {
            size = parser.m_index[1].m_offset - offset + 4 + static_cast<uint32_t>(parser.m_moviOffset);
        }
        uint32_t read = 0;
        if (cas::CASFileReadAtOffset(parser.m_file, player->m_buffer.data(), offset, size, &read, 0, -1)) {
            BeginPlaying(player);
            return;
        }
        uint32_t error = cas::CASGetErrorClass();
        if (error != 7) {
            player->m_keyFailed = 1;
        }
        SLogWrite(g_movieLog, "WowMoviePlayer MovieHandle::GetTactKey failed CASError = %d", error);
    }
    bool pending = Translated(0x141359b60, [] { return FUN_141359b60(); }) & 0xff;
    if (!pending && player->m_keyFailed == 0
        && static_cast<int32_t>(OsGetAsyncTimeMs()) - player->m_keyDeadline < 0) {
        return;
    }
    SLogWrite(g_movieLog, "WowMoviePlayer MovieHandle::WaitForKey timeout");
    player->m_frame->OnMovieFinished();
}

// The encoding's frames played to the audio's clock (or the wall clock): captions kept up,
// frames before a keyframe skipped, a pending switch made at a keyframe, the frames read and
// decoded, the newest drawn into the frame's rectangle. A frame not on disk pauses the audio
// behind the loading bar until it is.
// ref: FUN_1405fef20
void RenderFrame(CMoviePlayer* player) {
    IOldMovieAudioStream* audio = player->m_audio;
    if (audio != nullptr && audio->IsStarved()) {
        int32_t count = player->m_frameCount;
        int32_t next = player->m_frameIndex + 1;
        int32_t ahead = static_cast<int32_t>(player->m_fps * 6.0f + static_cast<float>(next));
        if (count < ahead) {
            ahead = count;
        }
        for (int32_t f = next, n = 0; f < ahead; f++, n++) {
            if (!IsFrameResident(player, player->m_resolution, f)) {
                LoadingBarSetProgress(static_cast<float>(n) * (1.0f / static_cast<float>(count - next)));
                return;
            }
        }
        LoadingBarSetProgress(1.0f);
        audio->SetPaused(false);
        LoadingBarHide();
    }
    uint32_t elapsed;
    bool audioClock = audio != nullptr && audio->IsPlaying() && SE3BusIsEnabled(0x16);
    if (audioClock) {
        elapsed = audio->GetPositionMs();
    } else {
        elapsed = OsGetAsyncTimeMs() - static_cast<uint32_t>(player->m_startTime);
    }
    int32_t target = static_cast<int32_t>(static_cast<float>(elapsed) * player->m_fps * 0.001f + 0.5f);
    if (player->m_looping != 0) {
        if (audioClock) {
            if (target < player->m_frameIndex) {
                player->m_frameIndex = -1;
            }
        } else if (player->m_frameCount <= target) {
            player->m_startTime = static_cast<int32_t>(OsGetAsyncTimeMs());
            target = 0;
            player->m_frameIndex = -1;
        }
    }
    if (!SE3BusIsEnabled(0x16) && player->m_frameCount <= target) {
        player->m_frame->OnMovieFinished();
        return;
    }
    if (player->m_frameCount - 1 <= target) {
        target = player->m_frameCount - 1;
    }
    if (player->m_frameIndex < target) {
        player->m_skipped += target - player->m_frameIndex - 1;
    }
    // captions
    if (player->m_captionFrame != nullptr) {
        bool keep = false;
        if (player->m_caption >= 0) {
            if (elapsed < static_cast<uint32_t>(player->m_captions[player->m_caption].m_end)) {
                keep = true;
            } else {
                QueueCaption(player->m_captionFrame, 3, "");
                player->m_caption = -1;
            }
        }
        if (!keep) {
            for (uint64_t i = 0; i < player->m_captions.size(); i++) {
                MOVIECAPTION& caption = player->m_captions[i];
                if (static_cast<uint32_t>(caption.m_start) <= elapsed && elapsed < static_cast<uint32_t>(caption.m_end)
                    && (player->m_captionFrame->m_subtitlesEnabled != 0 || caption.m_force != 0)) {
                    QueueCaption(player->m_captionFrame, 2, caption.m_text.m_data);
                    player->m_caption = static_cast<int32_t>(i);
                    break;
                }
            }
        }
    }
    // frames before a keyframe that is due need no decoding (unless encrypted: the key stream runs on)
    {
        int32_t at = player->m_frameIndex;
        for (int32_t f = player->m_frameIndex + 1; f < target; f++, at++) {
            MOVIERESOLUTION& res = player->m_resolutions[player->m_resolution];
            if ((res.m_parser.m_index[f].m_flags >> 4 & 1) == 0) {
                continue;
            }
            if (res.m_parser.m_pviv == nullptr) {
                player->m_frameIndex = at;
            } else {
                while (player->m_frameIndex < at) {
                    player->m_frameIndex++;
                    const movieplayerv1::AVIINDEXENTRY& entry = res.m_parser.m_index[player->m_frameIndex];
                    res.m_decoder->Skip(player->m_buffer.data(), entry.m_size);
                }
            }
        }
    }
    int32_t at = player->m_frameIndex;
    for (;;) {
        if (target <= at) {
            int32_t current = player->m_resolution;
            MOVIERESOLUTION& res = player->m_resolutions[current];
            if (res.m_parser.m_pviv != nullptr || player->m_pendingResolution != -1 || current == 0
                || static_cast<float>(at - player->m_switchFrame) < res.m_parser.m_fps || IsStreamPlayable(player, current, at)) {
                int32_t higher = ShouldRequestHigherResolution(player);
                if (higher != -1) {
                    SLogWrite(g_movieLog, "WowMoviePlayer MovieHandle::RequestHigherResolution CurrentRes %s",
                        ResolutionName(player->m_resolution));
                    int32_t now = player->m_resolution;
                    if (now != 2 && player->m_resolutions[now + 1].m_parser.m_file != nullptr) {
                        RequestResolution(player, now + 1, static_cast<uint32_t>(higher));
                    }
                }
            } else {
                RequestLowerResolution(player);
            }
            if (s_renderFrames != 0) {
                CRect rect = {};
                player->m_frame->GetRect(&rect);
                player->m_resolutions[player->m_resolution].m_decoder->Render(&rect.minY);
            }
            return;
        }
        int32_t pending = player->m_pendingResolution;
        int32_t next = at + 1;
        player->m_frameIndex = next;
        if (pending >= 0) {
            bool switchNow = !IsFrameResident(player, player->m_resolution, next);
            if (switchNow) {
                SLogWrite(g_movieLog,
                    "WowMoviePlayer MovieHandle::ShouldSwitchResolution Frame %d: frame data missing for res %s", next,
                    ResolutionName(player->m_resolution));
            } else if ((player->m_resolutions[pending].m_parser.m_index[next].m_flags >> 4 & 1) != 0
                && IsFrameResident(player, pending, next)) {
                switchNow = true;
            }
            if (switchNow) {
                MOVIERESOLUTION& to = player->m_resolutions[pending];
                int32_t key = player->m_frameIndex;
                while ((to.m_parser.m_index[key].m_flags >> 4 & 1) == 0) {
                    key--;
                }
                for (int32_t f = key; f < player->m_frameIndex; f++) {
                    const movieplayerv1::AVIINDEXENTRY& entry = to.m_parser.m_index[f];
                    uint32_t read = 0;
                    cas::CASFileReadAtOffset(to.m_parser.m_file, player->m_buffer.data(),
                        static_cast<uint32_t>(to.m_parser.m_moviOffset) + 4 + entry.m_offset, entry.m_size, &read, 0, -1);
                    to.m_decoder->DecodeFrame(player->m_buffer.data(), read);
                }
                SLogWrite(g_movieLog, "WowMoviePlayer MovieHandle::SwitchResolution Frame %d: Switched to resolution %s",
                    player->m_frameIndex, ResolutionName(pending));
                player->m_resolution = pending;
                player->m_pendingResolution = -1;
                player->m_skipped = 0;
                player->m_switchFrame = player->m_frameIndex;
            }
        }
        if (!IsFrameResident(player, player->m_resolution, player->m_frameIndex)) {
            audio->SetPaused(true);
            LoadingBarShow();
            player->m_frameIndex = at;
            return;
        }
        MOVIERESOLUTION& res = player->m_resolutions[player->m_resolution];
        const movieplayerv1::AVIINDEXENTRY& entry = res.m_parser.m_index[player->m_frameIndex];
        uint32_t position = static_cast<uint32_t>(res.m_parser.m_moviOffset) + 4 + entry.m_offset;
        uint32_t read = 0;
        bool ok = cas::CASFileReadAtOffset(res.m_parser.m_file, player->m_buffer.data(), position, entry.m_size, &read, 0, -1);
        if (!ok || read != entry.m_size) {
            SLogWrite(g_movieLog,
                "WowMoviePlayer MovieHandle::RenderFrame CASReadFile failed Movie %d, position %d, requested %d bytes, read %d "
                "bytes, returned %s",
                player->m_movieId, position, entry.m_size, read, ok ? "true" : "false");
            player->m_frame->OnMovieFinished();
            return;
        }
        if (!res.m_decoder->DecodeFrame(player->m_buffer.data(), read)) {
            SLogWrite(g_movieLog,
                "WowMoviePlayer MovieHandle::RenderFrame AVIDecode failed Movie %d, position %d, requested %d bytes, read %d "
                "bytes, returned %s",
                player->m_movieId, position, entry.m_size, read, "false");
            player->m_frame->OnMovieFinished();
            return;
        }
        at = player->m_frameIndex;
    }
}

// The movie's encodings ready: the frame rate and length from 720P, the blocks laid out, the
// frame's callback waiting for the key, a deadline of one second (six with ClientServices).
// ref: FUN_1405fd1a0
int32_t FinalizeQueueMovie(CMoviePlayer* player, int32_t resolution) {
    SLogWrite(g_movieLog, "WowMoviePlayer MovieHandle::FinalizeQueueMovie(%s)", ResolutionName(resolution));
    MOVIERESOLUTION& res = player->m_resolutions[resolution];
    if (res.m_parser.m_frameCount == 0) {
        return 4;
    }
    player->m_fps = res.m_parser.m_fps;
    player->m_frameCount = res.m_parser.m_frameCount;
    InitStreamingBlocks(player);
    SetRenderCallback(player->m_frame, &WaitForKey, player);
    player->m_frame->m_329 = 0;
    player->m_frame->m_32aDirtied = 0;
    int32_t wait = ClientServicesGet() != nullptr ? 6000 : 1000;
    player->m_keyDeadline = static_cast<int32_t>(OsGetAsyncTimeMs()) + wait;
    return 0;
}

// A movie opened: its audio, its key, each encoding the record has, the overlay file, and the
// buffer for its largest frame.
// ref: FUN_1405fec60
int32_t QueueMovie(CMoviePlayer* player, int32_t movieId, bool looping) {
    Translated(0x141359dc0, [] { return FUN_141359dc0(); });
    player->m_movieId = movieId;
    player->m_skipped = 0;
    player->m_switchFrame = 0;
    player->m_frameIndex = -1;
    player->m_resolution = -1;
    player->m_pendingResolution = -1;
    player->m_looping = looping ? 1 : 0;
    player->m_audio = AudioStreamCreate(movieId);
    bool local = false;
    auto movie = static_cast<const uint8_t*>(g_movieDB.GetRecord(static_cast<uint32_t>(movieId), false, &local));
    int32_t audio = movie != nullptr ? static_cast<int32_t>(DB2FieldU32(movie, 2, 0, &g_movieDB)) : 0;
    player->m_audio->Prefetch(audio);
    player->m_keyValid = LoadKey(player) ? 1 : 0;
    bool found = false;
    uint32_t largest = 0;
    for (int32_t r = 0; r < 3; r++) {
        int32_t file = 0;
        if (!FindMovieFile(movieId, r, &file)) {
            continue;
        }
        found = true;
        int32_t result = InitVideoDecoder(player, file, r);
        if (result != 0 && result != 3) {
            return result;
        }
        CAviParser& parser = player->m_resolutions[r].m_parser;
        for (int32_t i = 0; i < parser.m_frameCount && static_cast<uint64_t>(i) < parser.m_index.size(); i++) {
            if (largest < parser.m_index[i].m_size) {
                largest = parser.m_index[i].m_size;
            }
        }
    }
    player->m_buffer.resize(largest);
    if (!found) {
        return 2;
    }
    if (const uint8_t* record = MovieGetVariation(movieId, false)) {
        int32_t overlay = static_cast<int32_t>(DB2FieldU32(record, 1, 0, &g_movieVariationDB));
        if (cas::CASFileExists(overlay)) {
            player->m_overlay.SetFile(overlay);
            if (!player->m_overlay.Open()) {
                player->m_overlay.Close();
            }
        }
    }
    return FinalizeQueueMovie(player, 1);
}

// The movie stopped: the frame's callback cleared, each encoding's reads, parser and decoder let
// go, the captions dropped, the audio shut down and the loading bar hidden.
// ref: FUN_140600a90
void StopMovie(CMoviePlayer* player) {
    SLogWrite(g_movieLog, "WowMoviePlayer MovieHandle::StopMovie");
    SetRenderCallback(player->m_frame, nullptr, nullptr);
    for (int32_t r = 0; r < 3; r++) {
        MOVIERESOLUTION& res = player->m_resolutions[r];
        if (res.m_blocks.size() != 0) {
            PrefetchCancel(player, r, 0, true);
        }
        if (res.m_parser.m_file != nullptr) {
            res.m_parser.Close();
        }
        if (res.m_decoder != nullptr) {
            res.m_decoder->Close();
            res.m_decoder->~IVideoDecoder();
            SMemFree(res.m_decoder, s_oldMovieFile, -2, 0);
        }
        res.m_decoder = nullptr;
    }
    player->m_captions.clear();
    if (player->m_audio != nullptr) {
        player->m_audio->Shutdown();
    }
    LoadingBarHide();
    Translated(0x141846870, [&] { return FUN_141846870(static_cast<uint64_t>(static_cast<int64_t>(player->m_movieId))); });
}

// ref: FUN_1405fc1c0
void AudioDeviceRestart(void* storage) {
    (void)storage;
    SLogWrite(g_movieLog, "WowMoviePlayer AudioDeviceChanged_Restart");
    for (uint64_t i = 0; i < s_handles.size(); i++) {
        CMoviePlayer* player = s_handles[i];
        SLogWrite(g_movieLog, "WowMoviePlayer MovieHandle::RestartAudio");
        if (player->m_audio != nullptr) {
            player->m_audio->Restart(static_cast<uint32_t>(static_cast<int64_t>(
                static_cast<float>(player->m_frameIndex * 1000) / player->m_fps)));
        }
    }
    Translated(0x141917d00, [] { return FUN_141917d00(); });
}

// ref: FUN_1405fc270
void AudioDeviceShutdown(void* storage) {
    (void)storage;
    SLogWrite(g_movieLog, "WowMoviePlayer AudioDeviceChanged_ShutDown");
    for (uint64_t i = 0; i < s_handles.size(); i++) {
        CMoviePlayer* player = s_handles[i];
        SLogWrite(g_movieLog, "WowMoviePlayer MovieHandle::ShutdownAudio");
        if (player->m_audio != nullptr) {
            player->m_audio->Stop();
        }
    }
}

}  // namespace

// ---- the player ------------------------------------------------------------------------------

// The movie's variation as wide as the window, or the nearest one: the widest below the window,
// or the narrowest above it when none is below. With `localOnly` only variations whose file (and
// overlay) are on disk count. Null for a movie with no variations.
// ref: FUN_14062d670
const uint8_t* MovieGetVariation(int32_t movieId, bool localOnly) {
    bool local = false;
    if (g_movieDB.GetRecord(static_cast<uint32_t>(movieId), false, &local) == nullptr
        || DB2ParentCount(&g_movieVariationDB, movieId) == 0) {
        return nullptr;
    }
    GXSIZE size;
    WindowGetResolution(&size);
    uint16_t width = size.m_width;
    WowDB2ParentLookup* lookup = g_movieVariationDB.m_parentLookup;
    if (lookup == nullptr) {
        return nullptr;
    }
    const uint8_t* best = nullptr;
    uint32_t bestWidth = 0;
    DB2PARENTITER it;
    DB2PARENTITER end;
    lookup->Begin(&it, movieId);
    lookup->End(&end);
    for (; std::memcmp(&it, &end, sizeof(it)) != 0; lookup->Next(&it)) {
        auto variation = static_cast<const uint8_t*>(lookup->Record(&it));
        int32_t file = static_cast<int32_t>(DB2FieldU32(variation, 0, 0, &g_movieVariationDB));
        if (localOnly) {
            if (cas::CASFileGetResidency(file, 0, 0) != 2) {
                continue;
            }
            int32_t overlay = static_cast<int32_t>(DB2FieldU32(variation, 1, 0, &g_movieVariationDB));
            if (overlay != 0 && cas::CASFileGetResidency(overlay, 0, 0) != 2) {
                continue;
            }
        }
        auto data = static_cast<const uint8_t*>(g_movieFileDataDB.GetRecord(static_cast<uint32_t>(file), false, &local));
        if (data == nullptr) {
            continue;
        }
        uint16_t each = DB2FieldU16(data, 0, 0, &g_movieFileDataDB);
        if (each == width) {
            return variation;
        }
        if (best == nullptr) {
            best = variation;
            bestWidth = each;
        } else if (bestWidth < width) {
            if (bestWidth < each && each < width) {
                best = variation;
                bestWidth = each;
            }
        } else if (each < bestWidth) {
            best = variation;
            bestWidth = each;
        }
    }
    return best;
}


// ref: FUN_1405fc880
CMoviePlayer* MoviePlayerCreate(CSimpleMovieFrame* frame) {
    SLogWrite(g_movieLog, "WowMoviePlayer CreateStream");
    void* memory = SMemAlloc(sizeof(CMoviePlayer), 1, s_oldMovieFile, SStrHashFnv1a(s_oldMovieFile), 0x96b, 0);
    auto player = new (memory) CMoviePlayer();
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_moviesOpen));
    player->m_frame = frame;
    s_handles.push_back(player);
    return player;
}

// ref: FUN_1405fcc20
void MoviePlayerDestroy(CMoviePlayer* player) {
    SLogWrite(g_movieLog, "WowMoviePlayer DestroyStream");
    for (uint64_t i = 0; i < s_handles.size(); i++) {
        if (s_handles[i] == player) {
            for (uint64_t j = i + 1; j < s_handles.size(); j++) {
                s_handles[j - 1] = s_handles[j];
            }
            s_handles.pop_back();
            break;
        }
    }
    if (player == nullptr) {
        return;
    }
    if (player->m_audio != nullptr) {
        AudioStreamFree(player->m_audio);
        player->m_audio = nullptr;
    }
    // FUN_14057ac30: the Keychain watch dropped (none is made offline)
    InterlockedDecrement(reinterpret_cast<volatile LONG*>(&g_moviesOpen));
    player->~CMoviePlayer();
    SMemFree(player, s_oldMovieFile, -2, 0);
}

// ref: FUN_1405feba0
int32_t MoviePlayerQueue(CMoviePlayer* player, int32_t movieId, bool looping) {
    int32_t result = QueueMovie(player, movieId, looping);
    const char* text;
    if (result == 0) {
        text = "Success";
    } else if (result == 1) {
        text = "Load DivX Failed";
    } else if (result == 2) {
        text = "Missing Rec";
    } else if (result == 3) {
        text = "Read File Failed";
    } else if (result == 4) {
        text = "Open Video Failed";
    } else {
        text = "Unknown Start Movie Result type";
    }
    SLogWrite(g_movieLog, "WowMoviePlayer QueueMovie ID: %d, Loop: %s, Result: %s", movieId, looping ? "Yes" : "No", text);
    return result;
}

// ref: FUN_140600a60
void MoviePlayerStop(CMoviePlayer* player) {
    SLogWrite(g_movieLog, "WowMoviePlayer StopMovie");
    StopMovie(player);
}

// ref: FUN_1405fcf30
void MoviePlayerUpdate(CMoviePlayer* player) {
    if (player->m_audio != nullptr) {
        player->m_audio->Update();
    }
}

// ref: FUN_1405fde10
void MoviePlayerInitialize() {
    SLogCreate("Logs\\Movie.log", 0, &g_movieLog);
    blz::function_storage restart;
    restart.m_invoke = reinterpret_cast<void*>(&AudioDeviceRestart);
    restart.m_manager = &blz::function_trivial_manager;
    blz::function_storage shutdown;
    shutdown.m_invoke = reinterpret_cast<void*>(&AudioDeviceShutdown);
    shutdown.m_manager = &blz::function_trivial_manager;
    s_audioListener = static_cast<int32_t>(Translated(0x1412e6330, [&] {
        return FUN_1412e6330(reinterpret_cast<uint64_t>(&restart), reinterpret_cast<uint64_t>(&shutdown));
    }));
}

// ref: FUN_1406006a0
void MoviePlayerShutdown() {
    for (uint64_t i = 0; i < s_handles.size(); i++) {
        SLogWrite(g_movieLog, "WowMoviePlayer StopMovie");
        StopMovie(s_handles[i]);
    }
    int32_t id = s_audioListener;
    Translated(0x1412e6c70, [&] { return FUN_1412e6c70(static_cast<uint64_t>(static_cast<int64_t>(id))); });
    SLogWrite(g_movieLog, "WowMoviePlayer Shutdown");
    SLogClose(g_movieLog);
    g_movieLog = nullptr;
}

// ref: FUN_140600090
void MoviePlayerSetRenderEnabled(bool enabled) {
    SLogWrite(g_movieLog, "WowMoviePlayer SetEnableRenderMovieFrame");
    s_renderFrames = enabled ? 1 : 0;
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:GenFault=GenFaultMissing")
#pragma comment(linker, "/alternatename:FUN_140237f80=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1402381d0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1405fb950=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1412e6330=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1412e6c70=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141359b60=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141359dc0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141846870=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141917d00=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1420bd960=GenForwardMissing")
// ---- end of link fallbacks ----

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142cce7cc = (void*)&s_renderFrames;
// ---- end of generated globals ----
