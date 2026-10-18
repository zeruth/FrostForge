#include "storm/thread/SThread.hpp"

#include <process.h>
#include <cstring>
#include <malloc.h>

#include "blz/Sync.hpp"
#include "os/Sync.hpp"
#include "storm/Memory.hpp"
#include "storm/Unicode.hpp"
#include "storm/list/TSList.hpp"

namespace {

const char s_threadFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Storm\\Source\\W32\\SThread.cpp";
const char s_trackTag[] = ".?AV?$TSList@USThreadTrack@@V?$TSGetLink@USThreadTrack@@@@$07@@";
const char s_atExitTag[] = ".?AV?$TSList@USThreadAtExitCallback@@V?$TSGetLink@USThreadAtExitCallback@@@@$07@@";

// A running thread (0x58 bytes), listed while it runs.
struct SThreadTrack {
    TSLink<SThreadTrack> m_link;    // 0x00
    uint32_t m_threadId;            // 0x10
    char m_name[0x40];              // 0x14
};
static_assert(sizeof(SThreadTrack) == 0x58, "SThreadTrack is 0x58 bytes");

// A callback the thread runs as it ends (0x20 bytes).
struct SThreadAtExitCallback {
    TSLink<SThreadAtExitCallback> m_link;   // 0x00
    void (*m_callback)(void* param);        // 0x10
    void* m_param;                          // 0x18
};

// What the trampoline is handed (0x20 bytes).
struct THREADSTART {
    STHREADPROC m_proc;             // 0x00
    void* m_param;                  // 0x08
    SThreadTrack* m_track;          // 0x10
    uintptr_t m_handle;             // 0x18
};

OSLAZYCRITSECT s_threadLock;                                            // DAT_142e1c4c8
TSList<SThreadTrack, TSGetLink<SThreadTrack>, 8> s_threads;             // DAT_142cc8a50
// SetThreadDescription, or -1 when kernel32 has none (DAT_142e1c500).
HRESULT(WINAPI* volatile s_setThreadDescription)(HANDLE, PCWSTR);

// The calling thread's at-exit callbacks (TLS +0x20d8, live while TLS +0x20f0 is set).
thread_local TSList<SThreadAtExitCallback, TSGetLink<SThreadAtExitCallback>, 8>* t_atExit;

void UnlinkTrack(SThreadTrack* track) {
    track->m_link.Unlink();
}

// Names the calling thread: SetThreadDescription where kernel32 has it, and the debugger's
// MS_VC_EXCEPTION (0x406d1388) while one is attached.
// ref: FUN_140255cb0
void SetThreadName(uint32_t threadId, const char* name) {
    using SETDESCRIPTION = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    if (s_setThreadDescription == nullptr) {
        auto found = reinterpret_cast<SETDESCRIPTION>(static_cast<intptr_t>(-1));
        if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll")) {
            if (auto proc = reinterpret_cast<SETDESCRIPTION>(GetProcAddress(kernel, "SetThreadDescription"))) {
                found = proc;
            }
        }
        InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(&s_setThreadDescription),
            reinterpret_cast<void*>(found), nullptr);
    }
    SETDESCRIPTION set = s_setThreadDescription;
    if (reinterpret_cast<uintptr_t>(set) - 1 < static_cast<uintptr_t>(-2)) {
        uint32_t length = static_cast<uint32_t>(std::strlen(name)) + 1;
        auto wide = static_cast<wchar_t*>(_alloca(length * sizeof(wchar_t)));
        UnicodeConvertUtf8ToUtf16(wide, length, name, length, nullptr, nullptr);
        set(GetCurrentThread(), wide);
    }
    if (IsDebuggerPresent()) {
        struct {
            uint32_t m_type;
            const char* m_name;
            uint32_t m_threadId;
            uint32_t m_flags;
        } info = { 0x1000, name, threadId, 0 };
        __try {
            RaiseException(0x406d1388, 0, sizeof(info) / sizeof(ULONG_PTR), reinterpret_cast<ULONG_PTR*>(&info));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

}

void (*g_sthreadStartHook)() = nullptr;

namespace {

// ref: FUN_140255de0
unsigned __stdcall ThreadEntry(void* argument) {
    auto start = static_cast<THREADSTART*>(argument);
    OsLazyCritSectEnter(&s_threadLock);
    SThreadTrack* track = start->m_track;
    STHREADPROC proc = start->m_proc;
    void* param = start->m_param;
    SetThreadName(static_cast<uint32_t>(-1), track->m_name);
    OsLazyCritSectLeave(&s_threadLock);
    if (g_sthreadStartHook != nullptr) {
        g_sthreadStartHook();  // (FROST: a debug aid, FROST_WATCH's per-thread arming)
    }
    // "Calling thread proc %p\n" goes to a log that is compiled out (FUN_140255c60).
    uint32_t result = proc(param);
    // "Thread %lu (%s:%p) is stopping.\n", likewise.
    OsLazyCritSectEnter(&s_threadLock);
    UnlinkTrack(track);
    UnlinkTrack(track);
    SMemFree(track, s_trackTag, -2, 0);
    OsLazyCritSectLeave(&s_threadLock);
    if (auto callbacks = t_atExit) {
        for (SThreadAtExitCallback* callback = callbacks->Head(); callback != nullptr;
            callback = callbacks->Next(callback)) {
            callback->m_callback(callback->m_param);
        }
        while (SThreadAtExitCallback* callback = callbacks->Head()) {
            callback->m_link.Unlink();
            SMemFree(callback, s_atExitTag, -2, 0);
        }
        // FUN_140255530: what is left is unlinked and the list's own terminator with it.
        callbacks->~TSList();
    }
    SMemFree(start, s_threadFile, 0x1a6, 0);
    return result;
}

// ref: FUN_1402559e0
uintptr_t CreateThreadInternal(uint32_t stackSize, STHREADPROC proc, void* param, uint32_t flags, uint32_t* threadId,
    const char* name) {
    auto start = static_cast<THREADSTART*>(SMemAlloc(sizeof(THREADSTART), 7, reinterpret_cast<const char*>(proc), 0, -7, 8));
    start->m_proc = proc;
    start->m_param = param;
    auto track = static_cast<SThreadTrack*>(SMemAlloc(sizeof(SThreadTrack), 8, s_trackTag, 0, -2, 8));
    track->m_link.m_prevlink = nullptr;
    track->m_link.m_next = nullptr;
    start->m_track = track;
    track->m_threadId = 0;
    const char* source = name != nullptr ? name : "";
    char* dest = start->m_track->m_name;
    for (int64_t left = 0x3f;;) {
        char c = *source++;
        *dest++ = c;
        if (c == '\0') {
            break;
        }
        if (--left == 0) {
            *dest = '\0';
            break;
        }
    }
    unsigned id;
    uintptr_t handle = _beginthreadex(nullptr, stackSize, &ThreadEntry, start, flags | CREATE_SUSPENDED, &id);
    track = start->m_track;
    if (handle == 0) {
        UnlinkTrack(track);
        UnlinkTrack(track);
        SMemFree(track, s_trackTag, -2, 0);
        SMemFree(start, s_threadFile, 0x1d0, 0);
        return 0;
    }
    start->m_handle = handle;
    track->m_threadId = id;
    OsLazyCritSectEnter(&s_threadLock);
    SThreadTrack* listed = start->m_track;
    UnlinkTrack(listed);
    s_threads.LinkToTail(listed);
    OsLazyCritSectLeave(&s_threadLock);
    if ((flags & CREATE_SUSPENDED) == 0) {
        ResumeThread(reinterpret_cast<HANDLE>(handle));
    }
    if (threadId != nullptr) {
        *threadId = id;
    }
    return handle;
}

}

// ref: FUN_140256030
uintptr_t SCreateThread(STHREADPROC proc, void* param, uint32_t* threadId, void* reserved, const char* name,
    uint32_t stackSize) {
    (void)reserved;
    return CreateThreadInternal(stackSize, proc, param, 0, threadId, name);
}

// ref: FUN_14025cc10
SSyncObject::SSyncObject() {
    this->m_opaqueData = nullptr;
}

// ref: FUN_14025cc20
void SSyncObject::Close() {
    if (this->m_opaqueData != nullptr) {
        CloseHandle(this->m_opaqueData);
        this->m_opaqueData = nullptr;
    }
}

// ref: FUN_14025cd80
uint32_t SSyncObject::Wait(uint32_t timeout) {
    return WaitForSingleObject(this->m_opaqueData, timeout);
}

// ref: FUN_14025cba0
SEvent::SEvent(bool manualReset, bool initialState) {
    this->m_opaqueData = nullptr;
    this->m_opaqueData = CreateEventA(nullptr, manualReset, initialState, nullptr);
}

void SEvent::Set() {
    blz::sync_event_set(&this->m_opaqueData);   // FUN_14025cd50
}

void SEvent::Reset() {
    blz::sync_event_reset(&this->m_opaqueData); // FUN_14025cd40
}

// ref: FUN_14025cc50
bool SThread::Create(STHREADPROC proc, void* param, SThread* thread, const char* name, uint32_t stackSize) {
    (void)name;
    uint32_t threadId;
    uintptr_t handle = SCreateThread(proc, param, &threadId, nullptr, nullptr, stackSize);
    thread->m_opaqueData = reinterpret_cast<HANDLE>(handle);
    return handle != 0;
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14025cd40=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14025cd50=GenForwardMissing")
// ---- end of link fallbacks ----

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142cc8a50 = (void*)&s_threads;
extern "C" void* const gen_p_142e1c4c8 = (void*)&s_threadLock;
// ---- end of generated globals ----
