#include "prism/Intel.hpp"
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstring>

namespace pm {

// ref: FUN_142358720
uint8_t IntelGeneration(const uint16_t* ids) {
    uint16_t device = ids[1];
    uint32_t low = device & 0xff;
    uint32_t high = device & 0xff00;
    if (high > 0x2200) {
        if (high > 0x5900) {
            switch (high) {
                case 0x5a00: return 9;
                case 0x8a00: return 10;
                case 0x9a00: return 0xc;
                case 0x9b00: return 8;
                default: return 0;
            }
        }
        switch (high) {
            case 0x5900:
            case 0x3e00: return 8;
            case 0x3100: return 6;
            case 0x4900:
            case 0x4c00:
            case 0x5600: return 0xd;
            default: return 0;
        }
    }
    if (high == 0x2200) {
        return 4;
    }
    if (high < 0xc01) {
        switch (high) {
            case 0xc00:
            case 0x400:
            case 0xa00: return 3;
            case 0x100: {
                uint32_t index = low - 0x50;
                if (index > 0x10 || ((0x100a1u >> index) & 1) == 0) {
                    return 1;
                }
                return 2;
            }
            case 0x900: return 7;
            case 0xb00: return 5;
            default: return 0;
        }
    }
    switch (high) {
        case 0xd00: return 3;
        case 0xf00: return 2;
        case 0x1600: return 5;
        case 0x1900: return 7;
        default: return 0;
    }
}

// ref: FUN_1423583b0
bool IntelQueryDeviceInfo(Allocator* allocator, uint8_t* info, ID3D11Device* device, ID3D11DeviceContext* context) {
    D3D11_COUNTER_INFO counterInfo = {};
    device->CheckCounterInfo(&counterInfo);
    if (counterInfo.LastDeviceDependentCounter == 0) {
        return false;
    }
    bool found = false;
    int32_t version = 0;
    D3D11_COUNTER_DESC desc = {};
    int32_t counter = D3D11_COUNTER_DEVICE_DEPENDENT_0;
    if (static_cast<int32_t>(counterInfo.LastDeviceDependentCounter) >= D3D11_COUNTER_DEVICE_DEPENDENT_0) {
        do {
            desc.Counter = static_cast<D3D11_COUNTER>(counter);
            desc.MiscFlags = 0;
            D3D11_COUNTER_TYPE type;
            UINT active = 0;
            UINT nameLength = 0;
            UINT unitsLength = 0;
            UINT descriptionLength = 0;
            HRESULT hr = device->CheckCounter(&desc, &type, &active, nullptr, &nameLength, nullptr, &unitsLength,
                nullptr, &descriptionLength);
            if (SUCCEEDED(hr) && nameLength != 0 && unitsLength != 0 && descriptionLength != 0) {
                auto name = static_cast<char*>(allocator->allocate(nameLength, 1));
                if (name != nullptr) {
                    std::memset(name, 0, nameLength);
                }
                auto units = static_cast<char*>(allocator->allocate(unitsLength, 1));
                if (units != nullptr) {
                    std::memset(units, 0, unitsLength);
                }
                auto description = static_cast<char*>(allocator->allocate(descriptionLength, 1));
                if (description != nullptr) {
                    std::memset(description, 0, descriptionLength);
                }
                hr = device->CheckCounter(&desc, &type, &active, name, &nameLength, units, &unitsLength, description,
                    &descriptionLength);
                found = SUCCEEDED(hr) && std::strcmp(name, "Intel Device Information") == 0;
                if (found) {
                    sscanf_s(description, "Version %d", &version);  // FUN_142358840
                }
                allocator->deallocate(name);
                allocator->deallocate(units);
                allocator->deallocate(description);
                if (found) {
                    break;
                }
            }
            counter++;
        } while (counter <= static_cast<int32_t>(counterInfo.LastDeviceDependentCounter));
    }
    ID3D11Counter* query = nullptr;
    bool read = false;
    if (found && SUCCEEDED(device->CreateCounter(&desc, &query))) {
        context->Begin(query);
        context->End(query);
        const uint32_t* data = nullptr;
        if (context->GetData(query, &data, sizeof(data), 0) == S_OK) {
            if (version > 0) {
                std::memcpy(info + 4, &data[0], 4);
                std::memcpy(info + 8, &data[1], 4);
            }
            if (version > 1) {
                std::memcpy(info + 0xc, &data[3], 4);
                std::memcpy(info + 0x10, &data[4], 4);
                std::memcpy(info + 0x14, &data[5], 4);
            }
            read = true;
        }
    }
    if (query != nullptr) {
        query->Release();
    }
    return read;
}

}
