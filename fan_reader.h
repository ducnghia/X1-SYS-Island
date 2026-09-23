#pragma once
#include <windows.h>
#include <cstddef>

struct FanSnapshot {
    DWORD fan1 = 0, fan2 = 0, mode = 0;
    bool ok = false;
};

// Version 2 wire layout published by X1FanService. No command API is exposed.
namespace fan_detail {
struct SharedData {
    DWORD magic, version;
    volatile LONG sequence;
    DWORD status, fan1, fan2;
    ULONGLONG updatedTick;
    DWORD lastError;
    LONG requestedMode;
    DWORD activeMode;
    LONG hottestTemp;
};
static_assert(sizeof(SharedData) == 48 && offsetof(SharedData, updatedTick) == 24);
inline bool valid(const SharedData& data, ULONGLONG now) {
    return data.magic == 0x31463158 && data.version == 2 && data.status == 1 &&
        now >= data.updatedTick && now - data.updatedTick < 5000;
}
}

inline FanSnapshot readFans() {
    FanSnapshot result;
    // Reopen each sample so service restarts cannot leave us on an old mapping.
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Global\\X1FanTelemetryV1");
    if (!mapping) return result;
    auto view = static_cast<const fan_detail::SharedData*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(fan_detail::SharedData)));
    if (view) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            LONG before = view->sequence;
            if (before & 1) continue;
            MemoryBarrier();
            fan_detail::SharedData copy = *view;
            MemoryBarrier();
            if (before != view->sequence) continue;
            if (fan_detail::valid(copy, GetTickCount64()))
                result = {copy.fan1, copy.fan2, copy.activeMode, true};
            break;
        }
        UnmapViewOfFile(view);
    }
    CloseHandle(mapping);
    return result;
}

inline const wchar_t* fanModeName(const FanSnapshot& fans) {
    if (!fans.ok) return L"N/A";
    switch (fans.mode) {
    case 0: return L"BIOS Auto";
    case 1: return L"Cool";
    case 2: return L"Aggressive";
    default: return L"N/A";
    }
}
