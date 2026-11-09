#pragma once
#include <windows.h>

#pragma comment(lib, "advapi32.lib")

// Per-user preferences under HKCU. Missing or malformed values use defaults.
struct HotkeyOption {
    UINT modifiers;
    UINT key;
    const wchar_t* label;
};

inline const HotkeyOption HOTKEYS[] = {
    {MOD_CONTROL | MOD_SHIFT, 'S', L"Ctrl+Shift+S"},
    {MOD_CONTROL | MOD_SHIFT, 'D', L"Ctrl+Shift+D"}
};
inline const wchar_t* const SETTINGS_KEY = L"Software\\X1SYSIsland";

inline bool loadSettingDword(const wchar_t* name, DWORD& value) {
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, name, RRF_RT_REG_DWORD,
        nullptr, &value, &size) == ERROR_SUCCESS;
}
inline void saveSettingDword(const wchar_t* name, DWORD value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, SETTINGS_KEY, name, REG_DWORD, &value, sizeof(value));
}

inline int loadHotkeyChoice() {
    DWORD value = 0;
    return loadSettingDword(L"HotkeyV3", value) && value < ARRAYSIZE(HOTKEYS) ? static_cast<int>(value) : 0;
}
inline void saveHotkeyChoice(int choice) { saveSettingDword(L"HotkeyV3", static_cast<DWORD>(choice)); }

inline bool loadAutoHideOnHover() {
    DWORD value = 1;
    return loadSettingDword(L"AutoHideOnHover", value) ? value != 0 : true;
}
inline void saveAutoHideOnHover(bool enabled) { saveSettingDword(L"AutoHideOnHover", enabled ? 1 : 0); }
