#include "tact/ImmediateRead.hpp"
#include "blz/Md5.hpp"
#include "tact/Codec.hpp"
#include "tact/Common.hpp"
#include "tact/Container.hpp"
#include "tact/Instance.hpp"
#include "tact/U32Set.hpp"
#include "util/Unimplemented.hpp"
#include <cstring>

namespace tact {

namespace {

const char s_immediateFile[] = "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\immediate_read.cpp";

// The corrupt blocks' encoded spans are no longer resident.
// ref: FUN_1420d6d20
void RemoveCorruptBlocks(Instance* instance, const uint8_t ekey[16], const ENCODINGHEADER* header, uint32_t offset,
    U32SET* corrupt) {
    for (uint32_t i = 0; i < header->m_blockCount; i++) {
        const ENCODINGBLOCK* block = &header->m_blocks[i];
        if (corrupt->Contains(i)) {
            instance->m_container->RemoveSpan(ekey, offset, block->m_encodedSize);
        }
        offset += block->m_encodedSize;
    }
}

}

// An unencoded file: read straight into place and checked against its e-key.
// ref: FUN_1420d63b0
uint8_t ImmediateReadUncompressed(Instance* instance, void* dest, const FILEKEY* key) {
    Container* container = instance->m_container;
    uint8_t result = container->Read(key->m_ekey, dest, 0, key->m_esize);
    if (result == 0) {
        uint8_t keySize = container->GetKeySize();
        blz::md5_context md5;
        blz::md5_init(&md5);
        blz::md5_update(&md5, dest, key->m_esize);
        uint8_t hash[16];
        blz::md5_final(&md5, hash);
        if (key->m_eSpecFlags < keySize) {
            keySize = key->m_eSpecFlags;
        }
        if (std::memcmp(key->m_ekey, hash, keySize) == 0) {
            return 0;
        }
        char text[0x21];
        KeyToString(text, sizeof(text), key->m_ekey);
        blz::log(g_logCategory, 4, s_immediateFile, 0x5b, "Corrupt uncompressed file for key: %s.", text);
        container->Remove(key->m_ekey);
        return 7;
    }
    if (result == 6) {
        return 7;
    }
    if (result != 7) {
        char text[0x21];
        KeyToString(text, sizeof(text), key->m_ekey);
        blz::log(g_logCategory, 4, s_immediateFile, 0x49, "Failed to read uncompressed file for key: %s. Error (%u): %s",
            text, result, ErrorString(result));
    }
    return result;
}

// An encoded file: read into scratch memory and decoded into place. Corrupt data drops the
// file (or, block-encoded, its corrupt blocks).
// ref: FUN_1420d6a40
uint8_t ImmediateReadWhole(Instance* instance, const FILEKEY* key, uint32_t codec, void* dest, uint32_t flags) {
    Container* container = instance->m_container;
    auto encoded = static_cast<uint8_t*>(instance->m_scratch.Allocate(key->m_esize));
    uint8_t result = container->Read(key->m_ekey, encoded, 0, key->m_esize);
    uint8_t status;
    if (result == 0) {
        uint8_t keySize = container->GetKeySize();
        if (key->m_eSpecFlags < keySize) {
            keySize = key->m_eSpecFlags;
        }
        U32SET corrupt;
        corrupt.Init();
        result = CodecDecode(dest, key->m_csize, encoded, key->m_esize, key->m_ekey, key->m_eSpec, keySize,
            static_cast<uint8_t>(flags & 1), reinterpret_cast<KEYGETTER>(instance->m_f58), instance->m_f60,
            static_cast<uint8_t>(~(flags >> 1) & 1), &instance->m_scratch, static_cast<DECODERHOOK*>(instance->m_memoryLimit),
            &corrupt);
        if (result == 0) {
            status = 0;
        } else {
            char text[0x21];
            KeyToString(text, sizeof(text), key->m_ekey);
            blz::log(g_logCategory, 4, s_immediateFile, 0x1b2, "Corrupt file data found for key: %s.", text);
            if (codec == 6) {
                uint32_t headerSize = 0;
                uint8_t error = 0;
                ENCODINGHEADER* header =
                    EncodingHeaderCreate(key->m_ekey, keySize, encoded, key->m_esize, &headerSize, &error);
                if (error == 0) {
                    RemoveCorruptBlocks(instance, key->m_ekey, header, headerSize, &corrupt);
                    status = 7;
                    EncodingHeaderFree(header);
                } else {
                    container->Remove(key->m_ekey);
                    status = 7;
                }
            } else {
                container->Remove(key->m_ekey);
                status = 7;
            }
        }
        if (corrupt.m_buckets) {
            for (uint32_t i = 0; i < corrupt.m_bucketCount; i++) {
                corrupt.m_buckets[i] = 0xFFFFFFFF;
            }
        }
        corrupt.m_count = 0;
        corrupt.m_allocator->deallocate(corrupt.m_buckets);
        corrupt.m_allocator->deallocate(corrupt.m_entries);
        corrupt.m_allocator->deallocate(corrupt.m_next);
    } else {
        if (result == 6) {
            result = 7;
        }
        status = result;
        if (result != 7) {
            char text[0x21];
            KeyToString(text, sizeof(text), key->m_ekey);
            blz::log(g_logCategory, 4, s_immediateFile, 0x193, "Failed to read encoded file for key: %s. Error (%u): %s",
                text, result, ErrorString(result));
        }
    }
    instance->m_scratch.Free(encoded);
    return status;
}

// A span of a block-encoded file, through the header cache.
// ref: FUN_1420d6570
uint8_t ImmediateReadSpan(Instance* instance, const FILEKEY* key, void* dest, uint32_t offset, uint32_t size,
    uint32_t flags) {
    (void)instance;
    (void)key;
    (void)dest;
    (void)offset;
    (void)size;
    (void)flags;
    // The header cache (FUN_1420db860 and the header fetch state) is not ported yet.
    FROST_UNIMPLEMENTED();
    return 7;
}

// ref: FUN_1420d6f40
uint8_t ImmediateReadByKey(Instance* instance, const FILEKEY* key, void* dest, uint32_t offset, uint32_t size,
    uint32_t* read, uint32_t flags) {
    uint32_t codec = 6;
    if (key->m_eSpec && *key->m_eSpec) {
        codec = ESpecType(key->m_eSpec, nullptr);
        if (codec == 0) {
            char text[0x21];
            KeyToString(text, sizeof(text), key->m_ekey);
            blz::log(g_logCategory, 4, s_immediateFile, 0x1c, "Unknown encoding %s for key: %s.", key->m_eSpec, text);
            return 1;
        }
    }
    uint32_t fileSize = key->m_csize;
    if (fileSize < offset || (offset == fileSize && size != 0)) {
        char text[0x21];
        KeyToString(text, sizeof(text), key->m_ekey);
        blz::log(g_logCategory, 4, s_immediateFile, 0x1e8, "Invalid span[%u, %u] requested for key: %s. FileSize: %u",
            offset, size, text, key->m_csize);
        return 3;
    }
    if (size == 0 || fileSize < offset + size) {
        size = fileSize - offset;
    }
    *read = size;
    if (size == 0) {
        return 0;
    }
    if (codec == 1) {
        if ((flags >> 1 & 1) == 0) {
            if (size == key->m_esize) {
                return ImmediateReadUncompressed(instance, dest, key);
            }
            auto whole = static_cast<uint8_t*>(instance->m_scratch.Allocate(key->m_esize));
            uint8_t result = ImmediateReadUncompressed(instance, whole, key);
            if (result == 0) {
                std::memcpy(dest, whole + offset, size);
                instance->m_scratch.Free(whole);
                return 0;
            }
            instance->m_scratch.Free(whole);
            return result;
        }
        uint8_t result = instance->m_container->Read(key->m_ekey, dest, offset, size);
        if (result == 6 || result == 7) {
            return 7;
        }
        if (result == 0) {
            return 0;
        }
        char text[0x21];
        KeyToString(text, sizeof(text), key->m_ekey);
        blz::log(g_logCategory, 4, s_immediateFile, 0x7a, "Failed to read uncompressed file for key: %s. Error (%u): %s",
            text, result, ErrorString(result));
        return result;
    }
    if (offset == 0 && key->m_csize <= size) {
        return ImmediateReadWhole(instance, key, codec, dest, flags);
    }
    if (codec == 6) {
        return ImmediateReadSpan(instance, key, dest, offset, size, flags);
    }
    char text[0x21];
    KeyToString(text, sizeof(text), key->m_ekey);
    blz::log(g_logCategory, 4, s_immediateFile, 0x202, "Invalid encoding %s for span reads for key: %s.", key->m_eSpec,
        text);
    return 3;
}

// ref: FUN_1420d6e50
uint8_t ImmediateReadByName(Instance* instance, const char* name, void* dest, uint32_t offset, uint32_t size,
    uint32_t* read, uint32_t flags) {
    FILEKEY key;
    uint8_t result = instance->m_fileManifest.Lookup(name, &key, 1);
    if (result != 0) {
        blz::log(g_logCategory, 4, s_immediateFile, 0x221, "Unable to lookup file with path %s. Error(%u) : %s", name,
            result, ErrorString(result));
        return result;
    }
    key.m_31 = 9;
    return ImmediateReadByKey(instance, &key, dest, offset, size, read, flags);
}

}
