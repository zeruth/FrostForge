#include "services/Telemetry.hpp"
#include "util/Unimplemented.hpp"
#include <initializer_list>

#include <windows.h>

namespace {

uint8_t s_telemetryStarted;  // DAT_1430d24c4: set when TelemetryInitialize starts the service

}

// ref: FUN_141565d60
void TelemetryInitialize(blz::function_storage* ids, blz::function_storage* client, blz::function_storage* character,
    blz::function_storage* world, bool china) {
    (void)china;
    // the providers are passed by value: the callee owns and destroys them
    for (blz::function_storage* provider : { ids, client, character, world }) {
        provider->m_manager(provider->m_storage, nullptr, 0);
    }
    FROST_UNIMPLEMENTED();
}

// The downloader runs as translated (tools/translate.py) until it is ported by hand: the client
// loop's FUN_140222a60 polls it every frame (FUN_1415577d0 locks its +0x460), so the offline
// stub that left it null faulted there once that loop step stopped being a stub (2026-10-17).
extern "C" unsigned long long FUN_141555d80(void** holder);
extern "C" unsigned long long FUN_141557640(void** holder);
extern "C" unsigned long long FUN_141556340(void** holder);
extern "C" int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);

// Allocates and constructs the downloader (0x4f8 bytes, 0x141555800) into *holder.
// translated: FUN_141555d80
void** WowDownloadCreate(void** holder) {
    __try {
        FUN_141555d80(holder);
    } __except (GenFault(0x141555d80ull, GetExceptionInformation())) {
    }
    return holder;
}

// Sets the downloader's started flag (+0x28).
// translated: FUN_141557640
void WowDownloadStart(void** holder) {
    __try {
        FUN_141557640(holder);
    } __except (GenFault(0x141557640ull, GetExceptionInformation())) {
    }
}

// The service told to stop (waiting up to 5 seconds for its queue), its two shared objects
// released, the curl layer shut down. Offline the service is never started.
// ref: FUN_141565c10
void TelemetryShutdown() {
    if (s_telemetryStarted) {
        FROST_UNIMPLEMENTED();
    }
}

// The downloader (0x4f8 bytes) destroyed and freed.
// translated: FUN_141556340
void WowDownloadDestroy(void** holder) {
    __try {
        FUN_141556340(holder);
    } __except (GenFault(0x141556340ull, GetExceptionInformation())) {
    }
}
