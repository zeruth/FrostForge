#include "services/Telemetry.hpp"
#include "util/Unimplemented.hpp"
#include <initializer_list>

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
