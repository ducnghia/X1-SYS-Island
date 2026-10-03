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

struct Stats {
    double cpu = -1, gpu = -1, temperature = -1;
    int tjMax = -1, distanceToTjMax = -1;
    FanSnapshot fans;
    unsigned long long total = 0, used = 0;
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

inline void accumulateGpuEngine(std::map<std::wstring, double>& engines,
    const PDH_FMT_COUNTERVALUE_ITEM_W& item, const std::vector<std::wstring>& intelLuids) {
    if (!validCounter(item.FmtValue.CStatus) || !item.szName) return;
    const std::wstring_view name(item.szName);
    for (const auto& luid : intelLuids) {
        const auto start = std::search(name.begin(), name.end(), luid.begin(), luid.end(),
            [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); });
        if (start == name.end()) continue;
        // Only matching adapters allocate a key. Retain the full physical-engine
        // suffix so processes combine without merging adapters or distinct engines.
        std::wstring key(start, name.end());
        std::transform(key.begin(), key.end(), key.begin(),
            [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
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
                DWORD bytes = 0, count = 0;
                if (gpuCounter && PdhGetFormattedCounterArrayW(gpuCounter, PDH_FMT_DOUBLE, &bytes, &count, nullptr) == PDH_MORE_DATA) {
                    gpuBuffer.resize(bytes);
                    auto items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(gpuBuffer.data());
                    if (PdhGetFormattedCounterArrayW(gpuCounter, PDH_FMT_DOUBLE, &bytes, &count, items) == ERROR_SUCCESS) {
                        std::map<std::wstring, double> engines;
                        for (DWORD i = 0; i < count; ++i)
                            accumulateGpuEngine(engines, items[i], intelLuids);
                        if (!engines.empty()) result.gpu = busiestEngine(engines);
                    }
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

