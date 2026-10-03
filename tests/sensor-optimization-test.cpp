#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <string>
#include <vector>
#include <cstring>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>

static size_t allocations = 0;
void* operator new(size_t size) {
    ++allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

namespace fake {
ULONGLONG now = 100, target = 100ULL << 16, thermal = (1ULL << 31) | (32ULL << 16);
int opens = 0, loads = 0, reads = 0, closes = 0, affinityCalls = 0;
int failRead = 0;
bool shortReply = false, failRestore = false, failSelect = false, failGet = false, failLoad = false;
GROUP_AFFINITY original{0xacu, 2, {0, 0, 0}}, affinity = original;
ULONGLONG WINAPI tick() { return now; }
HANDLE WINAPI create(LPCWSTR path, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) {
    if (wcsstr(path, L"PawnIO")) { ++opens; return reinterpret_cast<HANDLE>(1); }
    return reinterpret_cast<HANDLE>(2);
}
BOOL WINAPI close(HANDLE handle) { if (handle == reinterpret_cast<HANDLE>(1)) ++closes; return TRUE; }
DWORD WINAPI module(HMODULE, LPWSTR path, DWORD) { wcscpy_s(path, 32768, L"C:\\test\\sensor.exe"); return 18; }
BOOL WINAPI size(HANDLE, PLARGE_INTEGER value) { value->QuadPart = 1; return TRUE; }
BOOL WINAPI read(HANDLE, LPVOID buffer, DWORD, LPDWORD count, LPOVERLAPPED) {
    *static_cast<BYTE*>(buffer) = 0; *count = 1; return TRUE;
}
BOOL WINAPI ioctl(HANDLE, DWORD code, LPVOID input, DWORD, LPVOID output, DWORD, LPDWORD count, LPOVERLAPPED) {
    if (code == 0xA1B22084) {
        ++loads;
        if (failLoad) { now += 2000; SetLastError(ERROR_GEN_FAILURE); return FALSE; }
        return TRUE;
    }
    ++reads;
    ULONGLONG index = 0;
    memcpy(&index, static_cast<BYTE*>(input) + 32, sizeof(index));
    assert(index == 0x1a2 || index == 0x1b1);
    if (failRead == (index == 0x1a2 ? 1 : 2)) { SetLastError(ERROR_IO_DEVICE); return FALSE; }
    *static_cast<ULONGLONG*>(output) = index == 0x1a2 ? target : thermal;
    *count = shortReply ? 4 : 8;
    return TRUE;
}
BOOL WINAPI getAffinity(HANDLE, PGROUP_AFFINITY value) {
    if (failGet) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    *value = affinity; return TRUE;
}
BOOL WINAPI setAffinity(HANDLE, const GROUP_AFFINITY* value, PGROUP_AFFINITY previous) {
    ++affinityCalls;
    if ((previous && failSelect) || (!previous && failRestore)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    if (previous) { *previous = affinity; assert(value->Mask == 4 && value->Group == 2); }
    else assert(value->Mask == original.Mask && value->Group == original.Group);
    affinity = *value; return TRUE;
}
void cpuid(int* registers, int leaf) {
    memset(registers, 0, 4 * sizeof(int));
    if (leaf == 0) {
        registers[0] = 6;
        memcpy(registers + 1, "Genu", 4); memcpy(registers + 3, "ineI", 4); memcpy(registers + 2, "ntel", 4);
    }
}
void cpuidex(int* registers, int, int) { memset(registers, 0, 4 * sizeof(int)); registers[0] = 1 << 6; }
}
// Redirect only this header; production uses the original Win32 calls.
#define GetTickCount64 fake::tick
#define CreateFileW fake::create
#define CloseHandle fake::close
#define GetModuleFileNameW fake::module
#define GetFileSizeEx fake::size
#define ReadFile fake::read
#define DeviceIoControl fake::ioctl
#define GetThreadGroupAffinity fake::getAffinity
#define SetThreadGroupAffinity fake::setAffinity
#define __cpuid fake::cpuid
#define __cpuidex fake::cpuidex
#include "../cpu_temperature.h"
#undef GetTickCount64
#undef CreateFileW
#undef CloseHandle
#undef GetModuleFileNameW
#undef GetFileSizeEx
#undef ReadFile
#undef DeviceIoControl
#undef GetThreadGroupAffinity
#undef SetThreadGroupAffinity
#undef __cpuid
#undef __cpuidex
#include "../telemetry.h"

static void testSensors() {
    using namespace fake;
    std::wstring source;
    CpuTemperature sensor;
    assert(sensor.sample(source).package == 68);
    assert(opens == 1 && loads == 1 && reads == 2);
    assert(affinity.Mask == original.Mask && affinity.Group == original.Group);
    target = 105ULL << 16;
    assert(sensor.sample(source).package == 73); // TjMax is read each sample.
    assert(reads == 4);
    for (int failedRegister : {1, 2}) {
        now += 20000;
        failRead = failedRegister;
        int oldOpens = opens, oldLoads = loads, oldCloses = closes;
        assert(sensor.sample(source).package == -1);
        assert(source.find(std::to_wstring(ERROR_IO_DEVICE)) != std::wstring::npos);
        assert(closes == oldCloses + 1 && affinity.Mask == original.Mask);
        now += 9999;
        assert(sensor.sample(source).package == -1);
        assert(opens == oldOpens && loads == oldLoads);
        ++now;
        assert(sensor.sample(source).package == -1); // Sustained failure starts another delay.
        assert(opens == oldOpens + 1 && loads == oldLoads + 1);
        failRead = 0;
        now += 9999;
        assert(sensor.sample(source).package == -1);
        assert(opens == oldOpens + 1);
        ++now;
        assert(sensor.sample(source).package == 73);
    }
    shortReply = true;
    assert(sensor.sample(source).package == -1);
    assert(source.find(L"malformed response") != std::wstring::npos);
    assert(source.find(std::to_wstring(ERROR_BAD_LENGTH)) != std::wstring::npos);
    shortReply = false;
    now += 10000;
    thermal = 0;
    int oldOpens = opens;
    assert(sensor.sample(source).package == -1);
    assert(source.find(L"invalid thermal data") != std::wstring::npos);
    thermal = (1ULL << 31) | (32ULL << 16);
    assert(sensor.sample(source).package == 73);
    assert(opens == oldOpens + 1);
    failGet = true;
    int oldReads = reads;
    assert(sensor.sample(source).package == -1 && reads == oldReads);
    failGet = false;
    failSelect = true;
    assert(sensor.sample(source).package == -1 && reads == oldReads);
    failSelect = false;
    failRestore = true;
    assert(sensor.sample(source).package == -1);
    assert(source.find(L"cannot restore processor affinity (error 5)") != std::wstring::npos);
    // Also preserve read-failure backoff if restoration fails simultaneously.
    affinity = original;
    failRead = 1;
    oldOpens = opens;
    assert(sensor.sample(source).package == -1);
    assert(source.find(L"cannot restore") != std::wstring::npos);
    failRestore = false; failRead = 0; affinity = original;
    now += 9999;
    assert(sensor.sample(source).package == -1 && opens == oldOpens);
    ++now;
    assert(sensor.sample(source).package == 73);

    CpuTemperature loading;
    failLoad = true;
    assert(loading.sample(source).package == -1);
    oldOpens = opens;
    now += 9999;
    assert(loading.sample(source).package == -1 && opens == oldOpens);
    ++now; failLoad = false;
    assert(loading.sample(source).package == 73 && opens == oldOpens + 1);
}

static void testGpu() {
    const std::vector<std::wstring> luids{L"luid_0x00000000_0x00000001_", L"luid_0x00000000_0x00000002_"};
    std::map<std::wstring, double> engines, reference;
    auto add = [&](const wchar_t* name, double value, DWORD status = PDH_CSTATUS_VALID_DATA) {
        PDH_FMT_COUNTERVALUE_ITEM_W item{};
        item.szName = const_cast<wchar_t*>(name); item.FmtValue.CStatus = status; item.FmtValue.doubleValue = value;
        accumulateGpuEngine(engines, item, luids);
        if (!validCounter(status) || !name) return;
        std::wstring normalized(name);
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        for (const auto& luid : luids) {
            auto start = normalized.find(luid);
            if (start != std::wstring::npos) { reference[normalized.substr(start)] += (std::max)(0.0, value); break; }
        }
        assert(engines == reference);
    };
    add(L"pid_1_LUID_0X00000000_0X00000001_PHYS_0_ENG_0_ENGTYPE_3D", 30);
    add(L"pid_2_luid_0x00000000_0x00000001_phys_0_eng_0_engtype_3d", 35, PDH_CSTATUS_NEW_DATA);
    add(L"pid_3_luid_0x00000000_0x00000001_phys_0_eng_1_engtype_3d", 45);
    add(L"pid_3_luid_0x00000000_0x00000001_phys_1_eng_0_engtype_3d", 40);
    add(L"pid_4_luid_0x00000000_0x00000002_phys_0_eng_0_engtype_3d", 50);
    add(L"pid_5_luid_0x00000000_0x00000001_phys_0_eng_0_engtype_3d", -10);
    add(L"pid_6_luid_0x00000000_0x00000001_phys_0_eng_0_engtype_3d", 100, PDH_CSTATUS_INVALID_DATA);
    add(nullptr, 100);
    assert(engines.size() == 4 && busiestEngine(engines) == 65);
    PDH_FMT_COUNTERVALUE_ITEM_W unrelated{};
    unrelated.szName = const_cast<wchar_t*>(L"pid_100_LUID_0X00000000_0X00000003_phys_0_eng_0_engtype_3d");
    unrelated.FmtValue.CStatus = PDH_CSTATUS_VALID_DATA;
    unrelated.FmtValue.doubleValue = 99;
    size_t before = allocations;
    for (int i = 0; i < 10000; ++i) accumulateGpuEngine(engines, unrelated, luids);
    assert(allocations == before && engines == reference);
    add(L"pid_7_luid_0x00000000_0x00000001_phys_0_eng_0_engtype_3d", 70);
    assert(busiestEngine(engines) == 100);
}

int main() {
    testSensors(); testGpu();
    puts("PASS: GPU grouping/allocation and sensor retry/diagnostic/affinity tests");
}
