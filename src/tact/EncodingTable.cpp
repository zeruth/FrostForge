#include "tact/EncodingTable.hpp"
#include <cstring>

namespace tact {

namespace {

const char s_tableFile[] =
    "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\encoding_table\\encoding_table_reader.cpp";

uint32_t ReadBE16(const uint8_t* p) {
    return uint32_t(p[0]) << 8 | p[1];
}

uint32_t ReadBE32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

}

// ref: FUN_1420e72c0
void ENCODINGTABLE::Init(blz::allocator* allocator) {
    this->m_allocator = allocator;
    this->m_data = nullptr;
    this->m_size = 0;
    this->m_cKeySize = 0;
    this->m_eKeySize = 0;
    this->m_cPageCount = 0;
    this->m_cPageSize = 0;
    this->m_ePageCount = 0;
    this->m_ePageSize = 0;
    this->m_eSpecs.m_data = nullptr;
    this->m_eSpecs.m_count = 0;
    this->m_eSpecs.m_capacity = 0;
    this->m_eSpecs.m_allocator = allocator;
    this->m_valid = 0;
    this->m_cIndex = nullptr;
    this->m_cPages = nullptr;
    this->m_eIndex = nullptr;
    this->m_ePages = nullptr;
    this->m_68 = 0;
}

// ref: FUN_1420e7300
void ENCODINGTABLE::Destroy() {
    this->m_valid = 0;
    if (this->m_data) {
        this->m_allocator->deallocate(this->m_data);
    }
    this->m_data = nullptr;
    this->m_size = 0;
    if (this->m_eSpecs.m_data) {
        this->m_eSpecs.m_allocator->deallocate(this->m_eSpecs.m_data);
    }
}

// The buffer the encoding file is read into.
// ref: FUN_1420e7350
uint8_t* ENCODINGTABLE::Allocate(uint32_t size) {
    this->m_valid = 0;
    if (this->m_data) {
        this->m_allocator->deallocate(this->m_data);
    }
    this->m_data = static_cast<uint8_t*>(this->m_allocator->allocate(size, 0x10));
    this->m_size = size;
    if (!this->m_data) {
        blz::log(g_logCategory, 4, s_tableFile, 0x22, "Failed to allocate encoding table buffer. Size: %u", size);
    }
    return this->m_data;
}

// ref: FUN_1420e73d0
void ENCODINGTABLE::Clear() {
    this->m_valid = 0;
    if (this->m_data) {
        this->m_allocator->deallocate(this->m_data);
    }
    this->m_data = nullptr;
    this->m_size = 0;
}

// ref: FUN_1420e7aa0
uint8_t ENCODINGTABLE::Parse(uint64_t a2) {
    const uint8_t* data = this->m_data;
    this->m_68 = a2;
    this->m_valid = 0;
    if (!data || this->m_size <= 0x15) {
        return 8;
    }
    const char* message;
    int32_t line;
    if (data[0] != 'E' || data[1] != 'N') {
        message = "Invalid encoding table header string";
        line = 0x37;
    } else {
        if (data[2] != 1 || data[0x11] != 0) {
            blz::log(g_logCategory, 4, s_tableFile, 0x3e, "Unknown encoding table version. Version: '%u' Flags: '%u'",
                data[2], data[0x11]);
            return 8;
        }
        this->m_cKeySize = data[3];
        this->m_eKeySize = data[4];
        uint32_t cPage = ReadBE16(data + 5);
        uint32_t ePage = ReadBE16(data + 7);
        this->m_cPageSize = cPage << 10;
        this->m_ePageSize = ePage << 10;
        this->m_cPageCount = ReadBE32(data + 9);
        this->m_ePageCount = ReadBE32(data + 0xd);
        uint32_t specSize = ReadBE32(data + 0x12);
        if (static_cast<uint8_t>(this->m_cKeySize - 1) < 0x10 && static_cast<uint8_t>(this->m_eKeySize - 1) < 0x10 &&
            cPage != 0 && ePage != 0 && this->m_cPageCount != 0 && this->m_ePageCount != 0 && specSize != 0) {
            auto specs = reinterpret_cast<const char*>(data + 0x16);
            if (specs[specSize - 1] == '\0') {
                uint32_t count = 0;
                for (uint32_t i = 0; i < specSize; i++) {
                    if (specs[i] == '\0') {
                        count++;
                    }
                }
                this->m_eSpecs.Reserve(count);
                const char* start = specs;
                for (uint32_t i = 0; i < specSize; i++) {
                    if (specs[i] != '\0') {
                        continue;
                    }
                    if (std::strlen(start) == 0) {
                        blz::log(g_logCategory, 4, s_tableFile, 0x164, "Empty e-spec found in encoding table");
                        return 8;
                    }
                    if (this->m_eSpecs.m_count == this->m_eSpecs.m_capacity) {
                        this->m_eSpecs.Reserve(this->m_eSpecs.m_count + 1);
                    }
                    this->m_eSpecs.m_data[this->m_eSpecs.m_count] = start;
                    this->m_eSpecs.m_count++;
                    start = specs + i + 1;
                }
                const uint8_t* p = data + specSize + 0x16;
                this->m_cIndex = p;
                p += (uint32_t(this->m_cKeySize) + 0x10) * this->m_cPageCount;
                this->m_cPages = p;
                const uint8_t* eIndex = p + this->m_cPageCount * this->m_cPageSize;
                this->m_eIndex = eIndex;
                this->m_ePages = eIndex + (uint32_t(this->m_eKeySize) + 0x10) * this->m_ePageCount;
                uint32_t end = static_cast<uint32_t>(this->m_ePages - data) + this->m_ePageCount * this->m_ePageSize;
                if (this->m_size <= end) {
                    return 8;
                }
                this->m_valid = 1;
                return 0;
            }
            message = "Invalid e-spec table";
            line = 0x150;
        } else {
            message = "Invalid size in encoding table header";
            line = 0x54;
        }
    }
    blz::log(g_logCategory, 4, s_tableFile, line, message);
    return 8;
}

// Binary search over the page index: the last page whose first key is not above the key.
// ref: FUN_1420e7a00
uint32_t EncodingPageSearch(const uint8_t* index, uint32_t count, uint8_t keySize, const uint8_t* key, uint8_t compare) {
    uint32_t low = 0;
    while (low < count) {
        uint32_t mid = ((count - low) >> 1) + low;
        int result = std::memcmp(key, index + uint64_t(mid) * (uint32_t(keySize) + 0x10), compare);
        if (result == 0) {
            return mid;
        }
        if (result >= 0) {
            low = mid + 1;
        } else {
            count = mid;
        }
    }
    return low - 1;
}

// The e-key page entry (key, e-spec index, 40-bit size); 6 when absent.
// ref: FUN_1420e7880
uint8_t ENCODINGTABLE::FindEKey(const uint8_t* ekey, uint8_t keySize, const uint8_t** entry) {
    *entry = nullptr;
    uint8_t size = this->m_eKeySize;
    uint8_t compare = keySize < size ? keySize : size;
    uint32_t page = EncodingPageSearch(this->m_eIndex, this->m_ePageCount, size, ekey, keySize);
    if (page != 0xFFFFFFFF) {
        const uint8_t* p = this->m_ePages + uint64_t(this->m_ePageSize * page);
        const uint8_t* end = p + this->m_ePageSize;
        for (; p < end; p += uint64_t(size) + 9) {
            uint32_t index = uint32_t(p[size]) << 24 | uint32_t(p[size + 1]) << 16 | uint32_t(p[size + 2]) << 8 | p[size + 3];
            if (index == 0xFFFFFFFF) {
                break;
            }
            if (std::memcmp(p, ekey, compare) == 0) {
                *entry = p;
                return 0;
            }
        }
    }
    return 6;
}

// ref: FUN_1420e7980
uint8_t ENCODINGTABLE::FindESpec(const uint8_t* ekey, uint8_t keySize, const char** eSpec) {
    if (!this->m_valid) {
        return 1;
    }
    const uint8_t* entry = nullptr;
    uint8_t result = this->FindEKey(ekey, keySize, &entry);
    if (result == 0) {
        const uint8_t* p = entry + this->m_eKeySize;
        uint32_t index = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
        *eSpec = this->m_eSpecs.m_data[index];
    }
    return result;
}

}
