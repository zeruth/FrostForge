#include "blz/Random.hpp"

#include <cstring>
#include <immintrin.h>
#include <intrin.h>
#include <new>

#include <windows.h>
#include <bcrypt.h>

namespace {

// Whether the CPU offers RDRAND (the 0x400 bit of the reference's feature mask).
// ref: FUN_1420c11f0
bool CpuHasRdRand() {
    int regs[4];
    __cpuid(regs, 1);
    return (regs[2] & (1 << 30)) != 0;
}

class RdRandProvider : public ENTROPYPROVIDER {
    public:
    // ref: FUN_1420c3e20
    bool Fill(void* buffer, uint64_t size) override {
        auto out = static_cast<uint64_t*>(buffer);
        int32_t tries = 0;
        for (; size > 7; size -= 8) {
            unsigned long long value;
            while (!_rdrand64_step(&value)) {
                if (++tries > 9) {
                    return false;
                }
            }
            *out++ = value;
        }
        if (size != 0) {
            unsigned long long value;
            int32_t tail = 0;
            while (!_rdrand64_step(&value)) {
                if (++tail > 9) {
                    return false;
                }
            }
            memcpy(out, &value, size);
        }
        return true;
    }
};

class SystemProvider : public ENTROPYPROVIDER {
    public:
    // ref: FUN_1420c3f60
    bool Fill(void* buffer, uint64_t size) override {
        auto out = static_cast<uint8_t*>(buffer);
        for (; size > 0xFFFFFFFF; size -= 0xFFFFFFFF) {
            if (BCryptGenRandom(nullptr, out, 0xFFFFFFFF, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
                return false;
            }
            out += 0xFFFFFFFF;
        }
        if (size == 0) {
            return true;
        }
        return BCryptGenRandom(nullptr, out, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
    }
};

// RDRAND, provided the CPU has it and four draws are not all equal.
// ref: FUN_1420c3e90
ENTROPYPROVIDER* CreateRdRand(void* storage) {
    if (!CpuHasRdRand()) {
        return nullptr;
    }
    unsigned long long draws[4];
    for (auto& draw : draws) {
        int32_t tries = 0;
        while (!_rdrand64_step(&draw)) {
            if (++tries > 9) {
                return nullptr;
            }
        }
    }
    for (auto draw : draws) {
        if (draw != draws[0]) {
            return new (storage) RdRandProvider();
        }
    }
    return nullptr;
}

// ref: FUN_1420c4000
ENTROPYPROVIDER* CreateSystem(void* storage) {
    return new (storage) SystemProvider();
}

// The provider factories (0x1428e1b80) and the order they are tried in (0x1428e1bb0); the
// slots past the first two are empty in this build.
ENTROPYPROVIDER* (*const s_factories[6])(void*) = { CreateRdRand, CreateSystem, nullptr, nullptr, nullptr, nullptr };
const uint8_t s_order[6] = { 0, 1, 4, 2, 3, 5 };

}  // namespace

// ref: FUN_1420bdc90
ENTROPYSOURCE* EntropySourceInit(ENTROPYSOURCE* source) {
    source->m_provider = nullptr;
    source->m_created = false;
    source->m_index = -1;
    return source;
}

// ref: FUN_1420bdd00
void EntropySourceFill(ENTROPYSOURCE* source, void* buffer, uint64_t size) {
    if (!source->m_created) {
        if (source->m_index == -1) {
            for (uint8_t index : s_order) {
                if (s_factories[index]) {
                    if (ENTROPYPROVIDER* provider = s_factories[index](source->m_storage)) {
                        source->m_provider = provider;
                        source->m_created = true;
                        break;
                    }
                }
            }
        } else {
            source->m_provider = s_factories[source->m_index](source->m_storage);
            source->m_created = source->m_provider != nullptr;
        }
        if (!source->m_created) {
            return;
        }
    }
    source->m_provider->Fill(buffer, size);
}

// ref: FUN_1420bdcc0
void EntropySourceRelease(ENTROPYSOURCE* source) {
    if (source->m_created) {
        source->m_provider->~ENTROPYPROVIDER();
        source->m_provider = nullptr;
        source->m_created = false;
    }
}

// ref: FUN_1420bdb90
void ARC4Init(ARC4STATE* state, const uint8_t* key, uint64_t keyBytes) {
    for (uint32_t i = 0; i < 256; i++) {
        state->m_s[i] = static_cast<uint8_t>(i);
    }
    state->m_i = 0;
    state->m_j = 0;
    uint8_t j = 0;
    uint64_t k = 0;
    for (uint32_t i = 0; i < 256; i++) {
        uint8_t t = state->m_s[i];
        j = static_cast<uint8_t>(j + t + key[k & 0xFF]);
        state->m_s[i] = state->m_s[j];
        state->m_s[j] = t;
        k = ((k & 0xFF) + 1) % keyBytes;
    }
}

// ref: FUN_1420bdb40
void ARC4Skip(ARC4STATE* state, uint64_t count) {
    for (; count != 0; count--) {
        state->m_i++;
        state->m_j = static_cast<uint8_t>(state->m_j + state->m_s[state->m_i]);
        uint8_t t = state->m_s[state->m_i];
        state->m_s[state->m_i] = state->m_s[state->m_j];
        state->m_s[state->m_j] = t;
    }
}

// ref: FUN_1420bdc20
void ARC4Process(ARC4STATE* state, uint8_t* buffer, uint64_t size) {
    for (; size != 0; size--) {
        state->m_i++;
        state->m_j = static_cast<uint8_t>(state->m_j + state->m_s[state->m_i]);
        uint8_t t = state->m_s[state->m_i];
        state->m_s[state->m_i] = state->m_s[state->m_j];
        state->m_s[state->m_j] = t;
        *buffer++ ^= state->m_s[static_cast<uint8_t>(state->m_s[state->m_j] + state->m_s[state->m_i])];
    }
}
