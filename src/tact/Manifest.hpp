#ifndef TACT_MANIFEST_HPP
#define TACT_MANIFEST_HPP

// The manifests DynamicInit loads: the patch index (instance+0x1028), the patch manifest
// (instance+0xf98), the root/file manifest (instance+0x300) and the encoding table used for
// eSpec look-ups (instance+0xd90).

#include "tact/AsyncState.hpp"
#include "tact/PatchIndex.hpp"
#include <cstdint>

namespace tact {

struct BUILDCONFIG;
struct CONFIGRECORD;


// patch manifest: instance+0xf98
uint8_t PatchManifestParse(uint8_t* manifest, uint8_t* data, uint8_t* patchKey, uint32_t size);

// file manifest: instance+0x300
struct RECORDLIST {
    CONFIGRECORD* m_records;
    uint64_t m_count;
};


// The streaming fetch state (0x500 bytes) DynamicInit's stage 9 runs.
AsyncState* FetchStateCreate(void* memory, Instance* instance, uint64_t opId, OPCALLBACK callback, void* context);
void FetchStateStart(AsyncState* state, void* keys, uint32_t count, uint32_t a4, uint32_t a5, uint32_t a6,
    uint32_t a7, void (*progress)(void*, uint64_t, uint64_t), void* context, uint64_t a10, uint64_t a11);

}

#endif
