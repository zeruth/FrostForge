#ifndef TACT_CONFIG_READ_HPP
#define TACT_CONFIG_READ_HPP

// pkg/tact/3.6.39/src/async_state/async_config_read_state.cpp: reads the build (and CDN)
// configuration text from <storage>/config/xx/yy/<key> - or from the CDN when it is not
// local - and parses it into the instance's BUILDCONFIG.

#include "tact/AsyncState.hpp"
#include <cstdint>

namespace tact {

struct BUILDCONFIG;

// Constructs the 0xd0-byte state in memory from the buffer allocator.
AsyncState* ConfigReadCreate(void* memory, Instance* instance, uint64_t opId, OPCALLBACK callback, void* context);

// Starts it: (type, explicit path, storage path, destination, key, a6, a7).
void ConfigReadStart(AsyncState* state, uint32_t type, const char* path, const char* storagePath, BUILDCONFIG* config,
    const uint8_t key[16], uint8_t a6, uint8_t a7);

}

#endif
