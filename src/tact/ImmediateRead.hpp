#ifndef TACT_IMMEDIATE_READ_HPP
#define TACT_IMMEDIATE_READ_HPP

// pkg/tact/3.6.39/src/immediate_read.cpp: synchronous reads from the container, decoded on
// the calling thread. 7 means the data is not (all) local.

#include <cstdint>

namespace tact {

class Instance;
struct FILEKEY;

// flags: 1 streamed decode, 2 no hash check.
uint8_t ImmediateReadByName(Instance* instance, const char* name, void* dest, uint32_t offset, uint32_t size,
    uint32_t* read, uint32_t flags);
uint8_t ImmediateReadByKey(Instance* instance, const FILEKEY* key, void* dest, uint32_t offset, uint32_t size,
    uint32_t* read, uint32_t flags);
uint8_t ImmediateReadUncompressed(Instance* instance, void* dest, const FILEKEY* key);
uint8_t ImmediateReadWhole(Instance* instance, const FILEKEY* key, uint32_t codec, void* dest, uint32_t flags);
uint8_t ImmediateReadSpan(Instance* instance, const FILEKEY* key, void* dest, uint32_t offset, uint32_t size,
    uint32_t flags);

}

#endif
