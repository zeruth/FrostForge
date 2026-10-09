#ifndef SERVICES_TELEMETRY_HPP
#define SERVICES_TELEMETRY_HPP

#include "blz/Function.hpp"
#include <cstdint>

// Telemetry (pkg/telemetry-sdk-cpp and its protobuf messages, sent over curl). Offline for
// now (docs/stubs.md): nothing is created and nothing is sent.

// Registers the telemetryTargetPackage CVar, makes a session id and starts the telemetry
// service with four context providers (ids, client, character, world); china picks the CN
// endpoint. Offline stub.
void TelemetryInitialize(blz::function_storage* ids, blz::function_storage* client, blz::function_storage* character,
    blz::function_storage* world, bool china);

// WoW/Source/WowServices/WowDownload.cpp: the background downloader (CDN). Offline stub:
// *holder is left null.
void** WowDownloadCreate(void** holder);
void WowDownloadStart(void** holder);

#endif
