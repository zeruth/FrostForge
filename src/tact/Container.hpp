#ifndef TACT_CONTAINER_HPP
#define TACT_CONTAINER_HPP

// pkg/tact/3.6.39/src/container/dynamic_container.cpp: tact's view of local storage. The
// container interface (vtable 0x1428e9010, 38 slots) has one implementation the client uses,
// the dynamic container (vtable 0x1428e9140), which wraps a casc dynamic storage.

#include "blz/AllocString.hpp"
#include <cstdint>

namespace tact {

// What Instance::InitializeContainer opens the container with (0x30 bytes).
struct CONTAINERPARAMS {
    int32_t m_accessMode;       // 0x00: 0 read/write, 1 read-only, 2 ...
    uint8_t m_a7;               // 0x04
    uint64_t m_a8;              // 0x08
    const char* m_directory;    // 0x10
    void* m_ioContext;          // 0x18
    uint32_t m_a4;              // 0x20
    uint32_t m_a5;              // 0x24
    uint32_t m_a6;              // 0x28: 0 for the default 0x10000
    uint8_t m_a9;               // 0x2c: free space reclamation
};

// What an asynchronous read reports through: the function gets the functor back with the
// result and the bytes read (the read queue embeds one in each request).
struct READFUNCTOR {
    void (*m_function)(READFUNCTOR* self, uint8_t result, uint32_t read);
};

// The container interface. Slots are named where their use is known; the rest keep their
// vtable offset until they are reached.
class Container {
    public:
    virtual ~Container();                                       // 0x00
    virtual bool Slot08() = 0;                                  // 0x08
    virtual void Close() = 0;                                   // 0x10
    virtual uint8_t GetKeySize() = 0;                           // 0x18: e-key bytes the container keeps
    virtual bool IsWritable() = 0;                              // 0x20
    virtual bool Contains(const uint8_t key[16]) = 0;           // 0x28
    virtual bool GetSpan(const uint8_t key[16], uint32_t* span) = 0;  // 0x30
    virtual uint32_t Slot38(const uint8_t* key, uint32_t* out, uint32_t count, uint32_t* span) = 0;  // 0x38
    virtual uint32_t Slot40(const uint8_t* key, uint32_t* out, uint32_t count, uint32_t* span) = 0;  // 0x40
    virtual void Slot48() = 0;                                  // 0x48
    virtual void Slot50() = 0;                                  // 0x50
    virtual void Slot58() = 0;                                  // 0x58
    virtual uint8_t Read(const uint8_t key[16], void* dest, uint64_t offset, uint32_t size) = 0;  // 0x60
    virtual uint8_t ReadAsync(const uint8_t key[16], void* dest, uint64_t offset, uint32_t size, READFUNCTOR* done,
        bool* wouldBlock) = 0;                                  // 0x68
    virtual uint8_t FinishRead(uint8_t result, const uint8_t key[16], uint64_t read, uint64_t expected) = 0;  // 0x70
    virtual void Slot78() = 0;                                  // 0x78
    virtual void Slot80() = 0;                                  // 0x80
    virtual void Slot88() = 0;                                  // 0x88
    virtual void Slot90() = 0;                                  // 0x90
    virtual void Slot98() = 0;                                  // 0x98
    virtual void SlotA0() = 0;                                  // 0xa0
    virtual void SlotA8() = 0;                                  // 0xa8
    virtual void SlotB0() = 0;                                  // 0xb0
    virtual void SlotB8() = 0;                                  // 0xb8
    virtual void SlotC0() = 0;                                  // 0xc0
    virtual uint8_t Remove(const uint8_t key[16]) = 0;          // 0xc8
    virtual void SlotD0() = 0;                                  // 0xd0
    virtual uint8_t RemoveSpan(const uint8_t key[16], uint64_t offset, uint32_t size) = 0;  // 0xd8
    virtual uint8_t SlotE0() = 0;                               // 0xe0
    virtual void SlotE8() = 0;                                  // 0xe8
    virtual void SlotF0() = 0;                                  // 0xf0
    virtual void SlotF8() = 0;                                  // 0xf8
    virtual void Slot100() = 0;                                 // 0x100
    virtual void Slot108() = 0;                                 // 0x108
    virtual void Slot110() = 0;                                 // 0x110
    virtual void Slot118() = 0;                                 // 0x118
    virtual void Slot120() = 0;                                 // 0x120
    virtual void Slot128() = 0;                                 // 0x128
};

// The dynamic container (0x48 bytes).
class DynamicContainer : public Container {
    public:
    void* m_callbacks;              // 0x08: the instance's CDN callbacks (instance+0x2d8)
    blz::alloc_string m_directory;  // 0x10
    void* m_storage;                // 0x30: the casc storage
    uint64_t m_a8;                  // 0x38
    uint8_t m_readOnly;             // 0x40
    uint8_t m_a7;                   // 0x41

    explicit DynamicContainer(void* callbacks);
    ~DynamicContainer() override;

    uint8_t Open(CONTAINERPARAMS* params);

    bool Slot08() override;
    void Close() override;
    uint8_t GetKeySize() override;
    bool IsWritable() override;
    bool Contains(const uint8_t key[16]) override;
    bool GetSpan(const uint8_t key[16], uint32_t* span) override;
    uint32_t Slot38(const uint8_t* key, uint32_t* out, uint32_t count, uint32_t* span) override;
    uint32_t Slot40(const uint8_t* key, uint32_t* out, uint32_t count, uint32_t* span) override;
    void Slot48() override;
    void Slot50() override;
    void Slot58() override;
    uint8_t Read(const uint8_t key[16], void* dest, uint64_t offset, uint32_t size) override;
    uint8_t ReadAsync(const uint8_t key[16], void* dest, uint64_t offset, uint32_t size, READFUNCTOR* done,
        bool* wouldBlock) override;
    uint8_t FinishRead(uint8_t result, const uint8_t key[16], uint64_t read, uint64_t expected) override;
    void Slot78() override;
    void Slot80() override;
    void Slot88() override;
    void Slot90() override;
    void Slot98() override;
    void SlotA0() override;
    void SlotA8() override;
    void SlotB0() override;
    void SlotB8() override;
    void SlotC0() override;
    uint8_t Remove(const uint8_t key[16]) override;
    void SlotD0() override;
    uint8_t RemoveSpan(const uint8_t key[16], uint64_t offset, uint32_t size) override;
    uint8_t SlotE0() override;
    void SlotE8() override;
    void SlotF0() override;
    void SlotF8() override;
    void Slot100() override;
    void Slot108() override;
    void Slot110() override;
    void Slot118() override;
    void Slot120() override;
    void Slot128() override;
};
static_assert(sizeof(DynamicContainer) == 0x48, "DynamicContainer is 0x48 bytes");

// casc's error codes as tact's.
uint8_t ErrorFromCasc(int32_t error);

// The calls Instance makes on its container.
Container* ContainerConstruct(void* memory, void* callbacks);
uint8_t ContainerOpen(Container* container, CONTAINERPARAMS* params);
void ContainerClose(Container* container);
void ContainerDestruct(Container* container);
void ContainerDelete(Container* container);

}

#endif
