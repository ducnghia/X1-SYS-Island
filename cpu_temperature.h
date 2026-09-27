#pragma once
#include <windows.h>
#include <intrin.h>
#include <string>
#include <vector>

#include <cstring>

struct CpuPackageTemperature {
    double package = -1;
    int tjMax = -1;
    int distanceToTjMax = -1;
};

inline CpuPackageTemperature decodePackageTemperature(ULONGLONG target, ULONGLONG thermal) {
    // Intel SDM: IA32_TEMPERATURE_TARGET[23:16] minus digital readout [22:16].
    if (!(thermal & (1ULL << 31))) return {};
    int tjMax = static_cast<int>((target >> 16) & 0xff);
    int delta = static_cast<int>((thermal >> 16) & 0x7f);
    if (tjMax < 70 || tjMax > 125 || delta > tjMax) return {}; // Never guess TjMax.
    int value = tjMax - delta;
    if (value < 0 || value > 125) return {};
    return {static_cast<double>(value), tjMax, delta};
}

class CpuTemperature {
    HANDLE device = INVALID_HANDLE_VALUE;
    ULONGLONG retryAt = 0;
    DWORD lastError = 0;
    bool supported = false;

    bool readMsr(ULONGLONG index, ULONGLONG& value) {
        // The app only calls this with the two read-only temperature registers.
        if (index != 0x1a2 && index != 0x1b1) return false;
        BYTE packet[40]{};
        memcpy(packet, "ioctl_read_msr", sizeof("ioctl_read_msr"));
        memcpy(packet + 32, &index, sizeof(index));
        DWORD returned = 0;
        BOOL ok = DeviceIoControl(device, 0xA1B22104, packet, sizeof(packet),
            &value, sizeof(value), &returned, nullptr);
        lastError = ok ? ERROR_SUCCESS : GetLastError();
        return ok && returned == sizeof(value);
    }
    bool open() {
        if (device != INVALID_HANDLE_VALUE) return true;
        if (GetTickCount64() < retryAt) return false;
        retryAt = GetTickCount64() + 10000;
        device = CreateFileW(L"\\\\?\\GLOBALROOT\\Device\\PawnIO", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (device == INVALID_HANDLE_VALUE) { lastError = GetLastError(); return false; }
        wchar_t path[32768]{};
        DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
        if (!length || length >= ARRAYSIZE(path)) { close(); lastError = ERROR_BAD_PATHNAME; return false; }
        std::wstring module(path);
        module = module.substr(0, module.find_last_of(L"\\/")) + L"\\IntelMSR.bin";
        HANDLE file = CreateFileW(module.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { lastError = GetLastError(); close(); return false; }
        LARGE_INTEGER size{};
        bool sized = GetFileSizeEx(file, &size) != FALSE;
        if (!sized || size.QuadPart <= 0 || size.QuadPart > 1024 * 1024) {
            lastError = sized ? ERROR_BAD_LENGTH : GetLastError();
            CloseHandle(file); close(); return false;
        }
        std::vector<BYTE> blob;
        try { blob.resize(static_cast<size_t>(size.QuadPart)); }
        catch (...) { CloseHandle(file); close(); throw; }
        DWORD read = 0;
        BOOL loaded = ReadFile(file, blob.data(), static_cast<DWORD>(blob.size()), &read, nullptr);
        DWORD readError = loaded ? ERROR_HANDLE_EOF : GetLastError();
        CloseHandle(file);
        if (!loaded || read != blob.size()) { lastError = readError; close(); return false; }
        DWORD returned = 0;
        if (!DeviceIoControl(device, 0xA1B22084, blob.data(), static_cast<DWORD>(blob.size()),
                nullptr, 0, &returned, nullptr)) {
            lastError = GetLastError(); close(); return false;
        }
        return true;
    }
    void close() {
        if (device != INVALID_HANDLE_VALUE) CloseHandle(device);
        device = INVALID_HANDLE_VALUE;
    }
public:
    CpuTemperature() {
        int registers[4]{};
        __cpuid(registers, 0);
        char vendor[13]{};
        memcpy(vendor, &registers[1], 4); memcpy(vendor + 4, &registers[3], 4); memcpy(vendor + 8, &registers[2], 4);
        if (strcmp(vendor, "GenuineIntel") == 0 && registers[0] >= 6) {
            __cpuidex(registers, 6, 0);
            supported = (registers[0] & (1 << 6)) != 0; // Package thermal management.
        }
    }
    ~CpuTemperature() { close(); }
    CpuTemperature(const CpuTemperature&) = delete;
    CpuTemperature& operator=(const CpuTemperature&) = delete;
    CpuPackageTemperature sample(std::wstring& source) {
        if (!supported) { source = L"CPU Package sensor unsupported by this Intel CPU"; return {}; }
        if (!open()) {
            source = lastError == ERROR_ACCESS_DENIED ? L"CPU temperature: run SYS as administrator (PawnIO)"
                : L"CPU temperature: PawnIO / IntelMSR.bin unavailable (error " + std::to_wstring(lastError) + L")";
            return {};
        }
        // Keep both reads on one logical processor. This app targets a single-package laptop.
        GROUP_AFFINITY current{}, previous{};
        if (!GetThreadGroupAffinity(GetCurrentThread(), &current) || !current.Mask) {
            source = L"CPU temperature: processor affinity unavailable"; return {};
        }
        current.Mask &= (~current.Mask + 1); // First permitted processor in the current group.
        if (!SetThreadGroupAffinity(GetCurrentThread(), &current, &previous)) {
            source = L"CPU temperature: cannot select processor"; return {};
        }
        ULONGLONG target = 0, thermal = 0;
        bool ok = readMsr(0x1a2, target) && readMsr(0x1b1, thermal);
        SetThreadGroupAffinity(GetCurrentThread(), &previous, nullptr);
        CpuPackageTemperature value = ok ? decodePackageTemperature(target, thermal) : CpuPackageTemperature{};
        if (value.package < 0) {
            source = L"CPU Package sensor read unavailable (error " + std::to_wstring(lastError) + L")";
            if (!ok) close();
        } else source = L"CPU Package / Intel digital thermal sensor (direct)";
        return value;
    }
};

