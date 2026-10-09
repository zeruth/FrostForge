#ifndef BASE_C_DATA_STORE_HPP
#define BASE_C_DATA_STORE_HPP

// Engine/Source/Base/CDataStore: a byte buffer written at its end and read from a cursor. The
// buffer can hold a window [m_base, m_base + m_alloc) of a larger stream; the fetch hooks move
// the window. Only what the client database reads its sections through is here.

#include <cstdint>

class CDataStore {
    public:
    virtual void Reset();                                                           // 0x00
    virtual void Destroy(uint8_t** buffer, uint32_t* base, uint32_t* alloc);        // 0x08
    virtual bool FetchRead(uint32_t pos, uint32_t bytes, uint8_t** buffer, uint32_t* base, uint32_t* alloc);  // 0x10
    virtual bool FetchWrite(uint32_t pos, uint32_t bytes, uint8_t** buffer, uint32_t* base, uint32_t* alloc,
        const char* file, int32_t line);                                            // 0x18
    virtual void Slot20();                                                          // 0x20
    virtual void Slot28();                                                          // 0x28
    virtual void Slot30();                                                          // 0x30
    virtual void Finalize();                                                        // 0x38

    uint8_t* m_buffer = nullptr;    // 0x08
    uint32_t m_base = 0;            // 0x10
    uint32_t m_alloc = 0;           // 0x14
    uint32_t m_size = 0;            // 0x18
    uint32_t m_read = 0xFFFFFFFF;   // 0x1c ~0 until finalized

    CDataStore() = default;
    // The inlined destructor: FUN_1402ca240 through slot 0x08 when the buffer was allocated.
    ~CDataStore();

    // FUN_1402caba0: room for `bytes` more at the end; returns where they go.
    uint8_t* GetBufferForWrite(uint32_t bytes);
    // FUN_1402c9fe0: copies `bytes` from the cursor.
    CDataStore& GetData(void* dest, uint32_t bytes);
    // FUN_1402c9a30
    CDataStore& Get(uint32_t* value);
    // FUN_1402c9910: whether [pos, pos + bytes) can be read, fetching the window if needed.
    bool FetchForRead(uint32_t pos, uint32_t bytes);

    bool IsRead() const {
        return this->m_read > this->m_size;
    }
};

#endif
