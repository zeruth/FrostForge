#include "base/CDataStore.hpp"

#include <cstring>

#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/FrostLog.hpp"
#include "util/Unimplemented.hpp"
#include <windows.h>

namespace {

const char s_dataStoreFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Base\\CDataStore.cpp";

}

// ref: FUN_1402112e0
void CDataStore::Reset() {
}

// ref: FUN_1402ca240
void CDataStore::Destroy(uint8_t** buffer, uint32_t* base, uint32_t* alloc) {
    if (*alloc != 0 && *buffer != nullptr) {
        SMemFree(*buffer, s_dataStoreFile, -7, 0);
    }
    *buffer = nullptr;
    *base = 0;
    *alloc = 0;
}

// ref: FUN_1402ca290: a plain store holds the whole stream, so there is nothing to fetch.
bool CDataStore::FetchRead(uint32_t, uint32_t, uint8_t**, uint32_t*, uint32_t*) {
    return false;
}

// ref: FUN_1402ca2a0: grows the buffer to the write end rounded up to 0x100.
bool CDataStore::FetchWrite(uint32_t pos, uint32_t bytes, uint8_t** buffer, uint32_t* base, uint32_t* alloc,
    const char* file, int32_t line) {
    (void)base;
    uint32_t size = (bytes + 0xff + pos) & 0xFFFFFF00;
    int32_t flags = line;
    if (file == nullptr) {
        file = s_dataStoreFile;
        flags = -7;
    }
    *alloc = size;
    *buffer = static_cast<uint8_t*>(SMemReAlloc(*buffer, size, 1, file, SStrHashFnv1a(file), flags, 0));
    return true;
}

// ref: FUN_140233740
void CDataStore::Slot20() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1402348b0
void CDataStore::Slot28() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140235ae0
void CDataStore::Slot30() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140234070
void CDataStore::Finalize() {
    this->m_read = 0;
}

CDataStore::~CDataStore() {
    if (this->m_alloc != 0xFFFFFFFF) {
        this->Destroy(&this->m_buffer, &this->m_base, &this->m_alloc);
    }
}

// ref: FUN_1402caba0
uint8_t* CDataStore::GetBufferForWrite(uint32_t bytes) {
    uint32_t size = this->m_size;
    if (size < this->m_base || this->m_base + this->m_alloc < size + bytes) {
        this->FetchWrite(size, bytes, &this->m_buffer, &this->m_base, &this->m_alloc, s_dataStoreFile, 0x3b6);
    }
    this->m_size = size + bytes;
    return this->m_buffer + (size - this->m_base);
}

// ref: FUN_1402c9910
bool CDataStore::FetchForRead(uint32_t pos, uint32_t bytes) {
    if (this->m_size < pos + bytes) {
        // (a read past the end is how a message whose layout differs from the reader's fails:
        // the reader that ran off names the field. The first object update after Enter World
        // was refused this way and the client hung up with reason 7, 2026-10-21)
        static volatile long s_overruns;
        if (InterlockedIncrement(&s_overruns) <= 8) {
            FrostLog("net", "read past the end: %u bytes at %u of %u", bytes, pos, this->m_size);
            FrostLogStack(0, 12);
        }
        this->m_read = this->m_size + 1;
        return false;
    }
    if ((pos < this->m_base || this->m_base + this->m_alloc < pos + bytes)
        && !this->FetchRead(pos, bytes, &this->m_buffer, &this->m_base, &this->m_alloc)) {
        this->m_read = this->m_size + 1;
        return false;
    }
    return true;
}

// ref: FUN_1402c9fe0
CDataStore& CDataStore::GetData(void* dest, uint32_t bytes) {
    auto out = static_cast<uint8_t*>(dest);
    uint32_t read = this->m_read;
    if (this->m_size < read || bytes == 0) {
        return *this;
    }
    do {
        uint32_t size = this->m_size;
        uint32_t chunk = bytes;
        if (size - read < bytes) {
            chunk = size - read;
        }
        if (this->m_alloc < chunk) {
            chunk = this->m_alloc;
        }
        if (chunk < 1) {
            chunk = 1;
        }
        if (size < read + chunk) {
            this->m_read = size + 1;
            return *this;
        }
        if ((read < this->m_base || this->m_base + this->m_alloc < read + chunk)
            && !this->FetchRead(read, chunk, &this->m_buffer, &this->m_base, &this->m_alloc)) {
            this->m_read = this->m_size + 1;
            return *this;
        }
        uint8_t* source = this->m_buffer + (this->m_read - this->m_base);
        if (out != source) {
            std::memmove(out, source, chunk);
        }
        this->m_read += chunk;
        out += chunk;
        read = this->m_read;
        bytes -= chunk;
    } while (bytes != 0);
    return *this;
}

// ref: FUN_1402c9a30
CDataStore& CDataStore::Get(uint32_t* value) {
    if (this->FetchForRead(this->m_read, 4)) {
        std::memcpy(value, this->m_buffer + (this->m_read - this->m_base), 4);
        this->m_read += 4;
    }
    return *this;
}
