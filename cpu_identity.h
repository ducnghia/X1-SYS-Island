#pragma once
#include <windows.h>
#include <string>
#include <cwctype>

struct CpuIdentity {
    std::wstring compact = L"CPU";
    std::wstring expanded = L"CPU";
};

inline CpuIdentity normalizeCpuIdentity(std::wstring brand) {
    for (const auto* marker : {L"(R)", L"(TM)"}) {
        for (auto pos = brand.find(marker); pos != std::wstring::npos; pos = brand.find(marker))
            brand.erase(pos, std::wstring(marker).size());
    }
    if (brand.find(L"Intel") == std::wstring::npos) return {};
    auto core = brand.find(L"Core");
    if (core == std::wstring::npos) return {};
    auto pos = brand.find_first_not_of(L" \t", core + 4);
    if (pos == std::wstring::npos) return {};
    std::wstring family;
    if (brand.compare(pos, 5, L"Ultra") == 0) {
        family = L"Ultra ";
        pos = brand.find_first_not_of(L" \t", pos + 5);
        if (pos == std::wstring::npos) return {};
    }
    if (brand[pos] == L'i' && family.empty()) {
        family = L"i";
        ++pos;
    }
    if (pos >= brand.size() || std::wstring(L"3579").find(brand[pos]) == std::wstring::npos) return {};
    family += brand[pos++];
    if (pos < brand.size() && brand[pos] != L'-' && !std::iswspace(brand[pos])) return {};
    if (family.size() == 1) family = L"Core " + family;
    const std::wstring label = family.rfind(L"Core ", 0) == 0 ? family : L"Core " + family;
    CpuIdentity result{label, label};
    pos = brand.find_first_not_of(L" -\t", pos);
    if (pos == std::wstring::npos || !std::iswdigit(brand[pos])) return result;
    auto end = pos;
    while (end < brand.size() && std::iswalnum(brand[end])) ++end;
    // Keep the UI bounded; unfamiliar model formats retain the family label.
    if (end - pos <= 12) result.expanded += L" " + brand.substr(pos, end - pos);
    return result;
}

inline CpuIdentity detectCpuIdentity() {
    wchar_t brand[256]{};
    DWORD bytes = sizeof(brand);
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
        RRF_RT_REG_SZ, nullptr, brand, &bytes) != ERROR_SUCCESS) return {};
    return normalizeCpuIdentity(brand);
}
