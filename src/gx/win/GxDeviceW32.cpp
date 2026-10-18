#include "gx/Gx.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include <windows.h>
#include <dxgi1_6.h>
#include <algorithm>
#include <cstring>

namespace {

const char s_monitorTag[] = ".PEAX";  // blz::vector<void*>

// blz::vector<HMONITOR> (DAT_1434908a8): slot 0 is the primary monitor, then every DXGI output
// in adapter order.
struct MONITORLIST {
    HMONITOR* m_data;
    uint64_t m_size;
    uint64_t m_capacity;            // bit 63: the storage is not owned
};
MONITORLIST s_monitors;
uint8_t s_monitorsEnumerated;       // DAT_143490a08

// ref: FUN_142221490
void MonitorReserve(MONITORLIST* list, uint64_t count) {
    uint64_t capacity = list->m_capacity & ~blz::string::INLINE;
    if (capacity >= count) {
        return;
    }
    uint64_t grown = ((list->m_capacity >> 1) & 0x3FFFFFFFFFFFFFFFull) + capacity;
    if (grown < count) {
        grown = count;
    }
    list->m_capacity = ((list->m_capacity ^ grown) & blz::string::INLINE) ^ grown;
    auto data = static_cast<HMONITOR*>(SMemAlloc(grown * 8, 0xb, s_monitorTag, SStrHashFnv1a(s_monitorTag), -6, 0));
    if (list->m_size != 0) {
        std::memcpy(data, list->m_data, list->m_size * 8);
    }
    if ((list->m_capacity & blz::string::INLINE) == 0 && list->m_data != nullptr) {
        SMemFree(list->m_data, s_monitorTag, -6, 0);
    }
    list->m_capacity &= ~blz::string::INLINE;
    list->m_data = data;
}

void MonitorPush(MONITORLIST* list, HMONITOR monitor) {
    if (list->m_size == (list->m_capacity & ~blz::string::INLINE)) {
        MonitorReserve(list, (list->m_capacity & ~blz::string::INLINE) + 1);
    }
    list->m_data[list->m_size++] = monitor;
}

// ref: FUN_1422954c0
void GxMonitorsEnumerate() {
    s_monitors.m_size = 0;
    MonitorPush(&s_monitors, nullptr);
    int32_t primary = -1;
    auto factory = static_cast<IDXGIFactory1*>(g_gxPlatform.m_factory);
    IDXGIAdapter1* adapter = nullptr;
    if (factory != nullptr && factory->EnumAdapters1(0, &adapter) != DXGI_ERROR_NOT_FOUND) {
        UINT adapterIndex = 0;
        HRESULT found;
        do {
            IDXGIOutput* output = nullptr;
            UINT outputIndex = 0;
            HRESULT hr = adapter->EnumOutputs(0, &output);
            while (SUCCEEDED(hr)) {
                DXGI_OUTPUT_DESC desc;
                if (SUCCEEDED(output->GetDesc(&desc))) {
                    if (primary == -1) {
                        MONITORINFO info;
                        info.cbSize = sizeof(info);
                        if (GetMonitorInfoA(desc.Monitor, &info) && (info.dwFlags & MONITORINFOF_PRIMARY) != 0) {
                            primary = static_cast<int32_t>(s_monitors.m_size);
                        }
                    }
                    MonitorPush(&s_monitors, desc.Monitor);
                }
                output->Release();
                outputIndex++;
                hr = adapter->EnumOutputs(outputIndex, &output);
            }
            adapter->Release();
            adapterIndex++;
            found = factory->EnumAdapters1(adapterIndex, &adapter);
        } while (found != DXGI_ERROR_NOT_FOUND);
        if (primary >= 0) {
            s_monitors.m_data[0] = s_monitors.m_data[primary];
            s_monitorsEnumerated = 1;
            return;
        }
    }
    if (s_monitors.m_size < 2) {
        s_monitors.m_size = 0;
    } else {
        s_monitors.m_data[0] = s_monitors.m_data[1];
    }
    s_monitorsEnumerated = 1;
}

}

// ref: FUN_142290c70
void GxMonitorsReset() {
    s_monitorsEnumerated = 0;
    s_monitors.m_size = 0;
}

// ref: FUN_142290bd0
void* GxMonitorGet(uint32_t index) {
    if (s_monitorsEnumerated == 0) {
        GxMonitorsEnumerate();
    }
    if (s_monitors.m_size <= index) {
        return nullptr;
    }
    return s_monitors.m_data[index];
}

// ref: FUN_142290c10
uint32_t GxMonitorCount() {
    if (s_monitorsEnumerated == 0) {
        GxMonitorsEnumerate();
    }
    return static_cast<uint32_t>(s_monitors.m_size);
}

// The monitor's current desktop mode.
// ref: FUN_142296050
bool GxMonitorGetDesktopSize(uint32_t index, GXSIZE* size, uint32_t* bitsPerPixel) {
    auto monitor = static_cast<HMONITOR>(GxMonitorGet(index));
    if (monitor == nullptr) {
        return false;
    }
    MONITORINFOEXA info;
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoA(monitor, &info)) {
        return false;
    }
    DEVMODEA mode;
    std::memset(&mode, 0, sizeof(mode));
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsA(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
        return false;
    }
    size->m_width = static_cast<uint16_t>(mode.dmPelsWidth);
    size->m_height = static_cast<uint16_t>(mode.dmPelsHeight);
    *bitsPerPixel = mode.dmBitsPerPel;
    return true;
}

const char s_gxMonitorModeTag[] = ".?AUGxMonitorMode@@";
const char s_gxSizeTag[] = ".?AUGxSize@@";

namespace {

// The monitor's modes of at least 640x480 and 16 bits, not shown centred at a fixed size, and no
// larger than its current mode.
// ref: FUN_142296150
void MonitorEnumerateModes(uint32_t index, blz::vector<GXMONITORMODE, s_gxMonitorModeTag>* modes) {
    modes->clear();
    auto monitor = static_cast<HMONITOR>(GxMonitorGet(index));
    if (monitor == nullptr) {
        return;
    }
    uint32_t maxWidth = 0;
    uint32_t maxHeight = 0;
    auto current = static_cast<HMONITOR>(GxMonitorGet(index));
    if (current != nullptr) {
        MONITORINFOEXA info = {};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoA(current, &info)) {
            DEVMODEA mode = {};
            mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsA(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
                maxHeight = static_cast<uint16_t>(mode.dmPelsHeight);
                maxWidth = static_cast<uint16_t>(mode.dmPelsWidth);
            }
        }
    }
    MONITORINFOEXA info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoA(monitor, &info)) {
        return;
    }
    DEVMODEA mode = {};
    mode.dmSize = sizeof(mode);
    for (DWORD i = 0; EnumDisplaySettingsA(info.szDevice, i, &mode); i++) {
        if (mode.dmPelsWidth >= 0x280 && mode.dmPelsHeight >= 0x1e0 && mode.dmBitsPerPel >= 0x10
            && ((mode.dmFields & DM_DISPLAYFIXEDOUTPUT) == 0 || mode.dmDisplayFixedOutput != DMDFO_CENTER)
            && mode.dmPelsWidth <= maxWidth && mode.dmPelsHeight <= maxHeight) {
            GXMONITORMODE entry = {};
            entry.m_size.m_width = static_cast<uint16_t>(mode.dmPelsWidth);
            entry.m_size.m_height = static_cast<uint16_t>(mode.dmPelsHeight);
            entry.m_bitsPerPixel = mode.dmBitsPerPel;
            modes->push_back(entry);
        }
    }
}

// Larger first: width, then height, then depth (LAB_142291ae0).
bool MonitorModeGreater(const GXMONITORMODE& a, const GXMONITORMODE& b) {
    if (a.m_size.m_width != b.m_size.m_width) {
        return a.m_size.m_width > b.m_size.m_width;
    }
    if (a.m_size.m_height != b.m_size.m_height) {
        return a.m_size.m_height > b.m_size.m_height;
    }
    return a.m_bitsPerPixel > b.m_bitsPerPixel;
}

}  // namespace

// The modes, sorted (FUN_14228fb80). True when there are none.
// ref: FUN_142290c30
bool GxMonitorGetModes(uint32_t index, blz::vector<GXMONITORMODE, s_gxMonitorModeTag>* modes) {
    MonitorEnumerateModes(index, modes);
    std::sort(modes->begin(), modes->end(), MonitorModeGreater);
    return modes->size() == 0;
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_143490a08 = (void*)&s_monitorsEnumerated;
// ---- end of generated globals ----
