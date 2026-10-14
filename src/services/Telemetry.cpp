#include "services/Telemetry.hpp"
#include "util/Unimplemented.hpp"
#include <initializer_list>

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

// Allocates and constructs the downloader (0x4f8 bytes, 0x141555800) into *holder. Offline
// stub: nothing is constructed.
// ref: FUN_141555d80
void** WowDownloadCreate(void** holder) {
    *holder = nullptr;
    FROST_UNIMPLEMENTED();
    return holder;
}

// Sets the downloader's started flag (+0x28). Diverged: null-checked while the downloader is
// a stub.
// ref: FUN_141557640
void WowDownloadStart(void** holder) {
    if (!*holder) {
        return;
    }
    uint8_t* started = static_cast<uint8_t*>(*holder) + 0x28;
    if (*started == 0) {
        *started = 1;
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

// The downloader (0x4f8 bytes) destroyed and freed. Offline there is none.
// ref: FUN_141556340
void WowDownloadDestroy(void** holder) {
    if (*holder != nullptr) {
        FROST_UNIMPLEMENTED();
    }
}
