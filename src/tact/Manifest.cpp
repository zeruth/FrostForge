#include "tact/Manifest.hpp"
#include "tact/Codec.hpp"
#include "tact/Instance.hpp"
#include "util/Unimplemented.hpp"

namespace tact {

// ref: FUN_1420ebf80
uint8_t PatchManifestParse(uint8_t* manifest, uint8_t* data, uint8_t* patchKey, uint32_t size) {
    (void)manifest;
    (void)data;
    (void)patchKey;
    (void)size;
    FROST_UNIMPLEMENTED();
    return ERR_NOT_SUPPORTED;
}

// ref: FUN_14211b0f0
AsyncState* FetchStateCreate(void* memory, Instance* instance, uint64_t opId, OPCALLBACK callback, void* context) {
    (void)memory;
    (void)instance;
    (void)opId;
    (void)callback;
    (void)context;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_14211fb60
void FetchStateStart(AsyncState* state, void* keys, uint32_t count, uint32_t a4, uint32_t a5, uint32_t a6,
    uint32_t a7, void (*progress)(void*, uint64_t, uint64_t), void* context, uint64_t a10, uint64_t a11) {
    (void)state;
    (void)keys;
    (void)count;
    (void)a4;
    (void)a5;
    (void)a6;
    (void)a7;
    (void)progress;
    (void)context;
    (void)a10;
    (void)a11;
    FROST_UNIMPLEMENTED();
}


}
