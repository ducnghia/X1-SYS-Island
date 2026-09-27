#define NOMINMAX
#include <windows.h>
#include "../telemetry.h"
#include "../cpu_identity.h"
#pragma comment(lib, "advapi32.lib")
#include <cstdio>
#include <cassert>
int wmain(int argc, wchar_t** argv) {
    const auto i9 = normalizeCpuIdentity(L"11th Gen Intel(R) Core(TM) i9-11950H @ 2.60GHz");
    assert(i9.compact == L"Core i9" && i9.expanded == L"Core i9 11950H");
    assert(normalizeCpuIdentity(L"Intel(R) Core(TM) i7-8650U CPU @ 1.90GHz").expanded == L"Core i7 8650U");
    assert(normalizeCpuIdentity(L"Intel Core i5-1240P").expanded == L"Core i5 1240P");
    assert(normalizeCpuIdentity(L"Intel Core i3-8100").compact == L"Core i3");
    assert(normalizeCpuIdentity(L"Intel(R) Core(TM) Ultra 7 155H").expanded == L"Core Ultra 7 155H");
    assert(normalizeCpuIdentity(L"Intel Core 7 150U").expanded == L"Core 7 150U");
    assert(normalizeCpuIdentity(L"Intel Core i9").expanded == L"Core i9");
    assert(normalizeCpuIdentity(L"Intel Core Ultra").compact == L"CPU");
    assert(normalizeCpuIdentity(L"Intel Core i99-123").compact == L"CPU");
    assert(normalizeCpuIdentity(L"Unknown CPU").compact == L"CPU");
    assert(normalizeCpuIdentity(L"").expanded == L"CPU");
    const auto identity = detectCpuIdentity();
    wprintf(L"CPU identity: %s / %s\n", identity.compact.c_str(), identity.expanded.c_str());
    assert(validCounter(PDH_CSTATUS_VALID_DATA));
    assert(!validCounter(PDH_CSTATUS_INVALID_DATA));
    assert(busiestEngine({{L"3D", 65}, {L"Copy", 40}}) == 65);
    assert(busiestEngine({{L"3D", 130}}) == 100);
    assert(decodePackageTemperature(100ULL << 16, (1ULL << 31) | (32ULL << 16)) == 68);
    assert(decodePackageTemperature(100ULL << 16, 32ULL << 16) == -1);
    assert(decodePackageTemperature(0, 1ULL << 31) == -1);
    assert(decodePackageTemperature(100ULL << 16, (1ULL << 31) | (110ULL << 16)) == -1);
    assert(decodePackageTemperature(100ULL << 16, (1ULL << 31)) == 100);
    fan_detail::SharedData fanData{};
    fanData.magic = 0x31463158; fanData.version = 2; fanData.status = 1; fanData.updatedTick = 1000;
    assert(fan_detail::valid(fanData, 5999));
    assert(!fan_detail::valid(fanData, 6000));
    assert(!fan_detail::valid(fanData, 999));
    fanData.version = 99;
    assert(!fan_detail::valid(fanData, 2000));
    bool requireTemperature = argc > 1 && wcscmp(argv[1], L"--require-temperature") == 0;
    Telemetry telemetry;
    for (int i = 0; i < 4; ++i) {
        Sleep(1100);
        Stats s = telemetry.sample();
        wprintf(L"CPU=%.1f%% RAM=%.2f/%.2fGiB IntelGPU=%.1f%% Temp=%.1fC\nGPU: %s\nTemperature: %s\n",
            s.cpu, s.used / 1073741824.0, s.total / 1073741824.0,
            s.gpu, s.temperature, s.gpuName.c_str(), s.tempSource.c_str());
        assert(s.total > 0 && s.used <= s.total);
        assert(s.cpu >= 0 && s.cpu <= 100);
        assert(s.gpu == -1 || (s.gpu >= 0 && s.gpu <= 100));
        wprintf(L"Fan Mode=%s Fan 1=%lu Fan 2=%lu ReadOnly=%s\n", fanModeName(s.fans), s.fans.fan1, s.fans.fan2, s.fans.ok ? L"live" : L"unavailable");
        if (requireTemperature) assert(s.temperature >= 0 && s.temperature <= 125);
    }
    puts("PASS: native telemetry and sensor-validity tests.");
    return 0;
}
