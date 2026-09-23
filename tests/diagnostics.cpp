#define NOMINMAX
#include <windows.h>
#include "../telemetry.h"
#include <cstdio>
#include <cassert>
int wmain(int argc, wchar_t** argv) {
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
