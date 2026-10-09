#include "tact/Container.hpp"
#include "tact/AsyncState.hpp"
#include "tact/Common.hpp"
#include "casc/Dynamic.hpp"
#include "blz/Log.hpp"
#include "util/Unimplemented.hpp"
#include <cstring>
#include <new>

namespace tact {

namespace {

const char s_containerFile[] = "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\container\\dynamic_container.cpp";

// The instance's callback block (instance+0x2d8): two (context, owner, line) callbacks the
// client may install, statistics first.
struct CALLBACKBLOCK {
    void* m_owner;
    void (*m_stat)(void* context, void* owner, const void* line);
    void* m_statContext;
    void (*m_second)(void* context, void* owner, const void* line);
    void* m_secondContext;
};

// ref: 0x1420dc4d0
void ForwardStat(CALLBACKBLOCK* block, const void* line) {
    if (block->m_stat) {
        block->m_stat(block->m_statContext, block->m_owner, line);
    }
}

// ref: 0x1420dc4f0
void ForwardSecond(CALLBACKBLOCK* block, const void* line) {
    if (block->m_second) {
        block->m_second(block->m_secondContext, block->m_owner, line);
    }
}

// casc's two callbacks forward to the instance's block, dropping casc's owner argument.
// ref: 0x1420f84b0
void StorageCallbackA(void* context, void* argument, void* data) {
    (void)argument;
    ForwardStat(static_cast<CALLBACKBLOCK*>(context), data);
}

// ref: 0x1420f84c0
void StorageCallbackB(void* context, void* argument, void* data) {
    (void)argument;
    ForwardSecond(static_cast<CALLBACKBLOCK*>(context), data);
}

// casc's completion of an asynchronous read: its result goes to the reader's functor.
// ref: FUN_1420f8450
void OnStorageRead(void* context, void* owner, uint64_t key) {
    auto functor = static_cast<READFUNCTOR*>(context);
    uint32_t read = 0;
    int32_t result = casc::DynamicGetResult(static_cast<casc::DYNAMICCONTAINER*>(owner), key, &read);
    functor->m_function(functor, ErrorFromCasc(result), read);
}

}

// ref: FUN_1420f9200
uint8_t ErrorFromCasc(int32_t error) {
    switch (error) {
        case 0:
            return ERR_SUCCESS;
        case 2:
            return ERR_NOT_FOUND;
        case 3:
            return ERR_NOT_AVAILABLE;
        case 4:
        case 10:
            return ERR_INVALID_PARAMETER;
        case 5:
            return ERR_NOT_SUPPORTED;
        case 7:
            return ERR_DISK_FULL;
        case 9:
            return ERR_NEEDS_REPAIR;
        case 0xb:
            return ERR_CONTAINER_LOCKED;
        case 0xc:
            return ERR_ALREADY_EXISTS;
        case 0xd:
            return ERR_INVALID_DRIVE;
        case 0xf:
            return ERR_CANCELLED;
        case 0x15:
            return ERR_WOULD_BLOCK;
        case 0x17:
            return ERR_WOULD_NOT_PIPELINE;
        default:
            return ERR_UNKNOWN;
    }
}

// ref: FUN_1420f7ab0
Container::~Container() {
}

// ref: FUN_1420f7a10
DynamicContainer::DynamicContainer(void* callbacks) {
    this->m_callbacks = callbacks;
    this->m_directory.Init(GetAllocator());
    this->m_storage = nullptr;
    this->m_a8 = ~uint64_t(0);
    this->m_readOnly = 0;
    this->m_a7 = 0;
}

// ref: FUN_1420f7a60
DynamicContainer::~DynamicContainer() {
    casc::DynamicClose(reinterpret_cast<casc::DYNAMICCONTAINER**>(&this->m_storage));
    this->m_directory.Destroy();
}

// ref: FUN_1420f8030
uint8_t DynamicContainer::Open(CONTAINERPARAMS* params) {
    if (!params->m_directory) {
        blz::log(g_logCategory, 4, s_containerFile, 0x26, "Storage directory is required to initialize the dynamic container.");
        return ERR_INVALID_PARAMETER;
    }
    this->m_directory.Assign(params->m_directory, static_cast<uint32_t>(std::strlen(params->m_directory)));
    casc::OPENPARAMS open;
    open.m_bufferSize = 0x10000;
    open.m_20 = 0;
    this->m_readOnly = params->m_accessMode == 1;
    open.m_archiveCacheSize = static_cast<int32_t>(params->m_a4);
    open.m_freezeArchives = 0;
    open.m_noPidTable = 0;
    open.m_archiveCount = 0x3FF;
    open.m_allocator = GetAllocator();
    open.m_callbackB = &StorageCallbackB;
    open.m_callbackA = &StorageCallbackA;
    open.m_callbackAContext = this->m_callbacks;
    open.m_bufferCount = static_cast<int32_t>(params->m_a5);
    if (params->m_a6 != 0) {
        open.m_bufferSize = static_cast<int32_t>(params->m_a6);
    }
    open.m_ioContext = params->m_ioContext;
    open.m_reclaimFreeSpace = params->m_a9;
    open.m_callbackBContext = this->m_callbacks;
    if (open.m_reclaimFreeSpace && params->m_accessMode == 1) {
        blz::log(g_logCategory, 3, s_containerFile, 0x42,
            "Attempting to enable free space reclamation on a read-only container has no effect. Disabling.");
        open.m_reclaimFreeSpace = 0;
    }
    int32_t mode = 0;
    if (params->m_accessMode != 0) {
        if (params->m_accessMode == 1) {
            mode = 2;
        } else if (params->m_accessMode == 2) {
            mode = 5;
        }
    }
    open.m_lru = params->m_a7;
    int32_t error = casc::DynamicOpen(reinterpret_cast<casc::DYNAMICCONTAINER**>(&this->m_storage), params->m_directory,
        mode, &open);
    if (error == 0) {
        this->m_a7 = params->m_a7;
        this->m_a8 = params->m_a8;
        return ERR_SUCCESS;
    }
    blz::log(g_logCategory, 4, s_containerFile, 0x5e, "Failed to open dynamic container at '%s'. Error (%u): %s",
        params->m_directory, error, casc::ErrorString(error));
    if (error == casc::ERR_NEEDS_REPAIR) {
        int32_t reason = 0;
        casc::WriteRepairMarker(this->m_directory.c_str(), &reason);
    }
    return ErrorFromCasc(error);
}

// The shared `return false` (0x1402ca290).
bool DynamicContainer::Slot08() {
    return false;
}

// ref: FUN_1420f8ef0
void DynamicContainer::Close() {
    casc::DynamicClose(reinterpret_cast<casc::DYNAMICCONTAINER**>(&this->m_storage));
}

// ref: FUN_1420f7c10
uint8_t DynamicContainer::GetKeySize() {
    return 0x10;
}

// ref: FUN_1420f8ee0
bool DynamicContainer::IsWritable() {
    return this->m_readOnly == 0;
}

// ref: FUN_1420f8280
bool DynamicContainer::Contains(const uint8_t key[16]) {
    uint8_t copy[16];
    std::memcpy(copy, key, sizeof(copy));
    return casc::DynamicContains(static_cast<casc::DYNAMICCONTAINER*>(this->m_storage), copy) == 0;
}

// ref: FUN_1420f82b0
bool DynamicContainer::GetSpan(const uint8_t key[16], uint32_t* span) {
    uint64_t range[2];
    range[0] = span[0];
    range[1] = span[1];
    uint8_t copy[16];
    std::memcpy(copy, key, sizeof(copy));
    return casc::DynamicGetSpan(static_cast<casc::DYNAMICCONTAINER*>(this->m_storage), copy, range) == 0;
}

// ref: FUN_1420f7f40
uint32_t DynamicContainer::Slot38(const uint8_t* key, uint32_t* out, uint32_t count, uint32_t* span) {
    (void)key;
    (void)out;
    (void)count;
    (void)span;
    FROST_UNIMPLEMENTED();
    return 0;
}

// ref: FUN_1420f7e50
uint32_t DynamicContainer::Slot40(const uint8_t* key, uint32_t* out, uint32_t count, uint32_t* span) {
    (void)key;
    (void)out;
    (void)count;
    (void)span;
    FROST_UNIMPLEMENTED();
    return 0;
}

// ref: FUN_1420f7c20
void DynamicContainer::Slot48() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1420f7c70
void DynamicContainer::Slot50() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1420f7ca0
void DynamicContainer::Slot58() {
    FROST_UNIMPLEMENTED();
}

// A short read marks the rest of the span non-resident.
// ref: FUN_1420f84f0
uint8_t DynamicContainer::Read(const uint8_t key[16], void* dest, uint64_t offset, uint32_t size) {
    uint8_t copy[16];
    std::memcpy(copy, key, 16);
    uint64_t span[2] = { offset, size };
    uint32_t read = 0;
    auto storage = static_cast<casc::DYNAMICCONTAINER*>(this->m_storage);
    int32_t error = casc::DynamicRead(storage, copy, span, dest, &read);
    if (static_cast<uint32_t>(error - 2) > 1) {
        if (error == 0) {
            if (read < size) {
                char text[0x21];
                KeyToString(text, sizeof(text), key);
                blz::log(g_logCategory, 3, s_containerFile, 0x127,
                    "Detected truncated read for key %s. Marking file non resident", text);
                std::memcpy(copy, key, 16);
                uint64_t rest[2] = { uint64_t(read) + offset, size - read };
                error = casc::DynamicMarkNonResident(storage, copy, rest);
                if (error != 0) {
                    KeyToString(text, sizeof(text), key);
                    blz::log(g_logCategory, 4, s_containerFile, 0x24f,
                        "Failed to remove file span from dynamic container using key %s. Error (%u) : %s", text, error,
                        casc::ErrorString(error));
                }
                ErrorFromCasc(error);
                return 7;
            }
            return ERR_SUCCESS;
        }
        char text[0x21];
        KeyToString(text, sizeof(text), key);
        blz::log(g_logCategory, 4, s_containerFile, 0x118,
            "Failed to read file span from dynamic container using key %s. Error (%u): %s", text, error,
            casc::ErrorString(error));
    }
    return ErrorFromCasc(error);
}

// *wouldBlock when casc had no buffer free; a read served at once reports straight away.
// ref: FUN_1420f86d0
uint8_t DynamicContainer::ReadAsync(const uint8_t key[16], void* dest, uint64_t offset, uint32_t size, READFUNCTOR* done,
    bool* wouldBlock) {
    *wouldBlock = false;
    uint8_t copy[16];
    std::memcpy(copy, key, 16);
    uint64_t span[2] = { offset, size };
    uint64_t pending = 0;
    int32_t error = casc::DynamicReadAsync(static_cast<casc::DYNAMICCONTAINER*>(this->m_storage), copy, span, dest,
        &pending, &OnStorageRead, done, 1);
    if (pending == 0 && error == 0) {
        done->m_function(done, 0, size);
    }
    *wouldBlock = error == 0x15;
    return ErrorFromCasc(error);
}

// ref: FUN_1420f8790
uint8_t DynamicContainer::FinishRead(uint8_t result, const uint8_t key[16], uint64_t read, uint64_t expected) {
    (void)key;
    if (result == 0 && read < expected) {
        return ErrorFromCasc(3);
    }
    return result;
}

// ref: FUN_1420f8fc0
void DynamicContainer::Slot78() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1420f9080
void DynamicContainer::Slot80() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1420f9120
void DynamicContainer::Slot88() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1420f8f00
void DynamicContainer::Slot90() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1420f83a0
void DynamicContainer::Slot98() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1420f9130
void DynamicContainer::SlotA0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1420f7b10
void DynamicContainer::SlotA8() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1420f8b70
void DynamicContainer::SlotB0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1420f8d30
void DynamicContainer::SlotB8() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1420f8ad0
void DynamicContainer::SlotC0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1420f87b0
uint8_t DynamicContainer::Remove(const uint8_t key[16]) {
    uint8_t copy[16];
    std::memcpy(copy, key, sizeof(copy));
    int32_t error = casc::DynamicRemove(static_cast<casc::DYNAMICCONTAINER*>(this->m_storage), copy);
    if (error != 0) {
        char text[0x21];
        KeyToString(text, sizeof(text), key);
        blz::log(g_logCategory, 4, s_containerFile, 0x230,
            "Failed to remove file from dynamic container using key %s. Error (%u) : %s", text, error,
            casc::ErrorString(error));
    }
    return ErrorFromCasc(error);
}

// ref: FUN_1420f8900
void DynamicContainer::SlotD0() {
    FROST_UNIMPLEMENTED();
}

// Part of a file is no longer resident.
// ref: FUN_1420f8850
uint8_t DynamicContainer::RemoveSpan(const uint8_t key[16], uint64_t offset, uint32_t size) {
    uint8_t copy[16];
    std::memcpy(copy, key, 16);
    uint64_t span[2] = { offset, size };
    int32_t error = casc::DynamicMarkNonResident(static_cast<casc::DYNAMICCONTAINER*>(this->m_storage), copy, span);
    if (error != 0) {
        char text[0x21];
        KeyToString(text, sizeof(text), key);
        blz::log(g_logCategory, 4, s_containerFile, 0x24f,
            "Failed to remove file span from dynamic container using key %s. Error (%u) : %s", text, error,
            casc::ErrorString(error));
    }
    return ErrorFromCasc(error);
}

// The shared `return 5` (0x1413f8fd0).
uint8_t DynamicContainer::SlotE0() {
    return 5;
}

// ref: FUN_1420f84d0
void DynamicContainer::SlotE8() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1420f8dd0
void DynamicContainer::SlotF0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1420f7b80
void DynamicContainer::SlotF8() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1420f7ae0
void DynamicContainer::Slot100() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1420f7b00
void DynamicContainer::Slot108() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1420f7bc0
void DynamicContainer::Slot110() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141ee9840
void DynamicContainer::Slot118() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1420f82f0
void DynamicContainer::Slot120() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1420f8380
void DynamicContainer::Slot128() {
    FROST_UNIMPLEMENTED();
}

Container* ContainerConstruct(void* memory, void* callbacks) {
    return new (memory) DynamicContainer(callbacks);
}

uint8_t ContainerOpen(Container* container, CONTAINERPARAMS* params) {
    return static_cast<DynamicContainer*>(container)->Open(params);
}

void ContainerClose(Container* container) {
    container->Close();
}

void ContainerDestruct(Container* container) {
    container->~Container();
}

// The scalar deleting destructor (FUN_1420d4190) through the vtable, then the tact allocator.
void ContainerDelete(Container* container) {
    blz::allocator* allocator = GetAllocator();
    if (container) {
        container->~Container();
        allocator->deallocate(container);
    }
}

}
