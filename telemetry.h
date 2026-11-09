#pragma once
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi1_2.h>
#include "cpu_temperature.h"
#include "fan_reader.h"
#include <wrl/client.h>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <algorithm>
#include <cmath>
#include <cwctype>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "dxgi.lib")



using Microsoft::WRL::ComPtr;

// Numeric readings shown by the UI. Copying a Readings out of a Stats
// deliberately drops the diagnostic strings, so the UI never copies them.
struct Readings {
    double cpu = -1, gpu = -1, temperature = -1;
    int tjMax = -1, distanceToTjMax = -1;
    FanSnapshot fans;
    unsigned long long total = 0, used = 0;
};

struct Stats : Readings {
    std::wstring gpuName = L"Intel integrated GPU not detected";
    std::wstring tempSource = L"Reading CPU Package directly from Intel sensor...";
};

inline bool validCounter(DWORD status) {
    return status == PDH_CSTATUS_VALID_DATA || status == PDH_CSTATUS_NEW_DATA;
}
inline double busiestEngine(const std::map<std::wstring, double>& engines) {
    double result = 0;
    for (const auto& engine : engines) result = (std::max)(result, engine.second);
    return (std::clamp)(result, 0.0, 100.0);
}

// PDH instance names are ASCII, so ASCII folding matches towlower in the C
// locale without a CRT call per character across ~1000 instances per sample.
inline wchar_t asciiLower(wchar_t c) {
    return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
}

// Case-insensitive search; lowerPattern must already be lowercase.
inline const wchar_t* findAsciiInsensitive(const wchar_t* text, std::wstring_view lowerPattern) {
    if (lowerPattern.empty()) return text;
    for (; *text; ++text) {
        if (asciiLower(*text) != lowerPattern[0]) continue;
        size_t i = 1;
        while (i < lowerPattern.size() && text[i] && asciiLower(text[i]) == lowerPattern[i]) ++i;
        if (i == lowerPattern.size()) return text;
    }
    return nullptr;
}

// intelLuids must be lowercase, as produced by Telemetry.
inline void accumulateGpuEngine(std::map<std::wstring, double>& engines,
    const PDH_FMT_COUNTERVALUE_ITEM_W& item, const std::vector<std::wstring>& intelLuids) {
    if (!validCounter(item.FmtValue.CStatus) || !item.szName) return;
    for (const auto& luid : intelLuids) {
        const wchar_t* start = findAsciiInsensitive(item.szName, luid);
        if (!start) continue;
        // Only matching adapters allocate a key. Retain the full physical-engine
        // suffix so processes combine without merging adapters or distinct engines.
        std::wstring key(start);
        std::transform(key.begin(), key.end(), key.begin(), asciiLower);
        engines[std::move(key)] += (std::max)(0.0, item.FmtValue.doubleValue);
        break;
    }
}

// Direct CPU sensor and performance counters run on the worker, never the UI.
class Telemetry {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER cpuCounter = nullptr, gpuCounter = nullptr;
    std::vector<std::wstring> intelLuids;
    std::wstring gpuName;
    bool primed = false;
    std::vector<BYTE> gpuBuffer;
    CpuTemperature cpuTemperature;

    // Try the retained buffer first: the instance set rarely grows, so this
    // usually skips PDH's separate (and similarly expensive) size query.
    // If it is too small, fall back to the documented size-then-fetch pattern.
    const PDH_FMT_COUNTERVALUE_ITEM_W* readGpuArray(DWORD& count) {
        auto items = [this] { return reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(gpuBuffer.data()); };
        DWORD bytes = static_cast<DWORD>(gpuBuffer.size());
        PDH_STATUS status = bytes
            ? PdhGetFormattedCounterArrayW(gpuCounter, PDH_FMT_DOUBLE, &bytes, &count, items())
            : PDH_MORE_DATA;
        if (status == PDH_MORE_DATA) {
            bytes = 0;
            if (PdhGetFormattedCounterArrayW(gpuCounter, PDH_FMT_DOUBLE, &bytes, &count, nullptr) != PDH_MORE_DATA)
                return nullptr;
            gpuBuffer.resize(bytes);
            status = PdhGetFormattedCounterArrayW(gpuCounter, PDH_FMT_DOUBLE, &bytes, &count, items());
        }
        return status == ERROR_SUCCESS ? items() : nullptr;
    }
public:
    Telemetry() {
        ComPtr<IDXGIFactory1> factory;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            for (UINT i = 0;; ++i) {
                ComPtr<IDXGIAdapter1> adapter;
                if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
                if (!adapter) break;
                DXGI_ADAPTER_DESC1 desc{};
                if (FAILED(adapter->GetDesc1(&desc)) || desc.VendorId != 0x8086 ||
                    (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
                // DXGI reports a small reserved allocation for integrated adapters;
                // Intel discrete cards have substantial dedicated video memory.
                if (desc.DedicatedVideoMemory >= 1024ULL * 1024 * 1024) continue;
                wchar_t luid[64];
                swprintf_s(luid, L"luid_0x%08x_0x%08x_", static_cast<UINT>(desc.AdapterLuid.HighPart), desc.AdapterLuid.LowPart);
                intelLuids.emplace_back(luid);
                if (gpuName.empty()) gpuName = desc.Description;
            }
        }
        if (PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS) {
            PdhAddEnglishCounterW(query, L"\\Processor(_Total)\\% Processor Time", 0, &cpuCounter);
            if (!intelLuids.empty())
                PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &gpuCounter);
            primed = PdhCollectQueryData(query) == ERROR_SUCCESS;
        }
    }
    ~Telemetry() { if (query) PdhCloseQuery(query); }
    void reprime() {
        primed = query && PdhCollectQueryData(query) == ERROR_SUCCESS;
    }
    Stats sample() {
        Stats result;
        result.fans = readFans();
        if (!gpuName.empty()) result.gpuName = gpuName;
        MEMORYSTATUSEX memory{sizeof(memory)};
        if (GlobalMemoryStatusEx(&memory)) {
            result.total = memory.ullTotalPhys;
            result.used = memory.ullTotalPhys - memory.ullAvailPhys;
        }
        if (query && PdhCollectQueryData(query) == ERROR_SUCCESS) {
            if (primed) {
                PDH_FMT_COUNTERVALUE value{};
                if (cpuCounter && PdhGetFormattedCounterValue(cpuCounter, PDH_FMT_DOUBLE, nullptr, &value) == ERROR_SUCCESS && validCounter(value.CStatus))
                    result.cpu = (std::clamp)(value.doubleValue, 0.0, 100.0);
                DWORD count = 0;
                const auto* items = gpuCounter ? readGpuArray(count) : nullptr;
                if (items) {
                    std::map<std::wstring, double> engines;
                    for (DWORD i = 0; i < count; ++i)
                        accumulateGpuEngine(engines, items[i], intelLuids);
                    if (!engines.empty()) result.gpu = busiestEngine(engines);
                }
            }
            primed = true;
        } else primed = false;
        const CpuPackageTemperature packageTemperature = cpuTemperature.sample(result.tempSource);
        result.temperature = packageTemperature.package;
        result.tjMax = packageTemperature.tjMax;
        result.distanceToTjMax = packageTemperature.distanceToTjMax;
        return result;
    }
};

