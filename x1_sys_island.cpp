#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <string>
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cmath>

#include "resource.h"
#include "telemetry.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

// Native Win32/GDI overlay; telemetry runs on a separate worker.
Stats g_snapshot;
std::mutex g_statsMutex;
HANDLE g_stopEvent;
HANDLE g_policyEvent;
std::atomic<bool> g_sampling{false};
std::atomic<DWORD> g_sampleInterval{1000};
bool g_onBattery = false;
bool g_suspended = false;
constexpr UINT WM_STATS_READY = WM_APP + 1;
std::thread g_worker;
constexpr int ISLAND_WIDTH = 560;
constexpr int COMPACT_HEIGHT = 46;
constexpr int EXPANDED_HEIGHT = 128;
double g_cpuLoad = -1;
constexpr COLORREF TEXT_COLOR = RGB(242, 242, 245);
HWND g_hwnd;
HANDLE g_singleInstanceMutex;

HFONT g_metricsFont;
HFONT g_smallFont;
HBITMAP g_intelLogo;
HBRUSH g_backgroundBrush;
std::array<std::wstring, 4> g_compactParts;

std::array<std::wstring, 6> g_expandedParts;

POINT g_dragStart;
bool g_dragging = false;
bool g_expanded = false;
bool g_hoverConsumed = false;
bool g_trackingMouse = false;
bool g_contextOpen = false;
bool g_userHidden = false;
bool g_hoverHidden = false;
bool g_hotkeyRegistered = false;
int g_hotkeyChoice = 0;

const UINT_PTR STATS_TIMER_ID = 1;
const UINT_PTR HOVER_TIMER_ID = 2;
const UINT_PTR RESHOW_TIMER_ID = 3;
const UINT_PTR ANIMATION_TIMER_ID = 4;
const UINT ANIMATION_INTERVAL_MS = 100;
const int HOTKEY_ID = 100;
const UINT WM_SHOW_EXISTING_ISLAND = WM_USER + 1;
const wchar_t SINGLE_INSTANCE_MUTEX[] = L"Global\\X1SYSIslandMutex";

const BYTE ISLAND_OPACITY = 230;
const wchar_t APP_VERSION[] = L"0.6.0";

enum class LoadLevel {
    Normal,
    Yellow,
    Red
};

LoadLevel g_loadLevel = LoadLevel::Normal;

struct HotkeyOption {
    UINT modifiers;
    UINT key;
    const wchar_t* label;
};

const HotkeyOption HOTKEYS[] = {
    {MOD_CONTROL | MOD_SHIFT, 'D', L"Ctrl+Shift+D"},
    {MOD_CONTROL | MOD_ALT, 'S', L"Ctrl+Alt+S"},
    {MOD_CONTROL | MOD_SHIFT, 'S', L"Ctrl+Shift+S"}
};
const wchar_t* SETTINGS_KEY = L"Software\\X1SYSIsland";
int loadHotkeyChoice() {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, L"HotkeyV2", RRF_RT_REG_DWORD,
        nullptr, &value, &size) == ERROR_SUCCESS && value < ARRAYSIZE(HOTKEYS)) return value;
    return 0;
}
void saveHotkeyChoice(int choice) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(key, L"HotkeyV2", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&choice), sizeof(choice));
        RegCloseKey(key);
    }
}
bool registerToggleHotkey(HWND hwnd, int choice) {
    if (choice < 0 || choice >= ARRAYSIZE(HOTKEYS)) return false;
    const auto& option = HOTKEYS[choice];
    g_hotkeyRegistered = RegisterHotKey(hwnd, HOTKEY_ID, option.modifiers | MOD_NOREPEAT, option.key) != FALSE;
    return g_hotkeyRegistered;
}
void selectHotkey(HWND hwnd, int choice) {
    if (choice == g_hotkeyChoice) return;
    int previous = g_hotkeyChoice;
    if (g_hotkeyRegistered) UnregisterHotKey(hwnd, HOTKEY_ID);
    if (registerToggleHotkey(hwnd, choice)) {
        g_hotkeyChoice = choice;
        saveHotkeyChoice(choice);
    } else {
        registerToggleHotkey(hwnd, previous);
        MessageBoxW(hwnd, L"Shortcut is already used. Previous shortcut retained.", L"X1 SYS Island", MB_OK | MB_ICONWARNING);
    }
}
void updateSamplingPolicy(HWND hwnd) {
    SYSTEM_POWER_STATUS power{};
    g_onBattery = !GetSystemPowerStatus(&power) || power.ACLineStatus != 1 || power.SystemStatusFlag == 1;
    g_sampleInterval.store(g_onBattery ? 3000 : 1000);
    g_sampling.store(IsWindowVisible(hwnd) && !g_suspended);
    if (g_policyEvent) SetEvent(g_policyEvent);
}
void startAnimation(HWND hwnd) {
    if(IsWindowVisible(hwnd) && !g_userHidden && !g_hoverHidden)
        SetTimer(hwnd,ANIMATION_TIMER_ID,g_onBattery ? 1000 : ANIMATION_INTERVAL_MS,nullptr);
}

void stopAnimation(HWND hwnd) {
    KillTimer(hwnd,ANIMATION_TIMER_ID);
}

bool cursorInside(HWND hwnd);
void showIsland(HWND hwnd) {
    g_userHidden=false;
    g_hoverHidden=false;
    KillTimer(hwnd,RESHOW_TIMER_ID);
    ShowWindow(hwnd,SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
    KillTimer(hwnd,HOVER_TIMER_ID);
    g_hoverConsumed=cursorInside(hwnd);
    g_trackingMouse=g_hoverConsumed;
    if(g_trackingMouse) {
        TRACKMOUSEEVENT tme{sizeof(tme),TME_LEAVE,hwnd,0};
        TrackMouseEvent(&tme);
    }
    startAnimation(hwnd);
}

void toggleIsland(HWND hwnd) {
    if(g_userHidden || !IsWindowVisible(hwnd)) {
        showIsland(hwnd);
    } else {
        g_userHidden=true;
        g_hoverHidden=false;
        KillTimer(hwnd,HOVER_TIMER_ID);
        KillTimer(hwnd,RESHOW_TIMER_ID);
        stopAnimation(hwnd);
        ShowWindow(hwnd,SW_HIDE);
    }
}

void cancelHover(HWND hwnd) {
    KillTimer(hwnd,HOVER_TIMER_ID);
    g_trackingMouse=false;
    g_hoverConsumed=false;
}

bool cursorInside(HWND hwnd) {
    POINT p{};
    RECT r{};
    return GetCursorPos(&p) && GetWindowRect(hwnd,&r) && PtInRect(&r,p);
}

LoadLevel levelForPercent(unsigned value) {
    if (value >= 80) return LoadLevel::Red;
    if (value >= 50) return LoadLevel::Yellow;
    return LoadLevel::Normal;
}

std::wstring formatText(const wchar_t* format, ...) {
    wchar_t buffer[256]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, ARRAYSIZE(buffer), _TRUNCATE, format, args);
    va_end(args);
    return buffer;
}

INT_PTR CALLBACK AboutDialogProc(HWND dialog,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_INITDIALOG: {
        HBITMAP logo=LoadBitmapW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDB_3S_LOGO));
        SetWindowLongPtrW(dialog,DWLP_USER,reinterpret_cast<LONG_PTR>(logo));
        SendDlgItemMessageW(dialog,IDC_ABOUT_LOGO,STM_SETIMAGE,IMAGE_BITMAP,
                            reinterpret_cast<LPARAM>(logo));
        SetDlgItemTextW(dialog,IDC_ABOUT_VERSION,formatText(L"X1 SYS Island v%s",APP_VERSION).c_str());
        SendDlgItemMessageW(dialog,IDC_ABOUT_VERSION,WM_SETFONT,
                            reinterpret_cast<WPARAM>(g_metricsFont),TRUE);

        HWND owner=GetWindow(dialog,GW_OWNER);
        HMONITOR monitor=MonitorFromWindow(owner ? owner : dialog,MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(info)};
        RECT window{};
        if(GetMonitorInfoW(monitor,&info) && GetWindowRect(dialog,&window)) {
            int width=window.right-window.left;
            int height=window.bottom-window.top;
            int x=info.rcWork.left+(info.rcWork.right-info.rcWork.left-width)/2;
            int y=info.rcWork.top+(info.rcWork.bottom-info.rcWork.top-height)/2;
            SetWindowPos(dialog,HWND_TOP,x,y,0,0,SWP_NOSIZE|SWP_NOACTIVATE);
        }
        return TRUE;
    }
    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(GetStockObject(WHITE_BRUSH));
    case WM_CTLCOLORSTATIC: {
        HDC dc=reinterpret_cast<HDC>(wp);
        HWND control=reinterpret_cast<HWND>(lp);
        SetBkMode(dc,TRANSPARENT);
        int id=GetDlgCtrlID(control);
        if(id==IDC_ABOUT_VERSION || id==IDC_ABOUT_TITLE) SetTextColor(dc,RGB(52,148,245));
        else if(id==IDC_ABOUT_SUBTITLE) SetTextColor(dc,RGB(86,104,120));
        else if(id==IDC_ABOUT_CREDIT) SetTextColor(dc,RGB(105,105,105));
        return reinterpret_cast<INT_PTR>(GetStockObject(WHITE_BRUSH));
    }
    case WM_COMMAND:
        if(LOWORD(wp)==IDOK || LOWORD(wp)==IDCANCEL) {
            EndDialog(dialog,LOWORD(wp));
            return TRUE;
        }
        break;
    case WM_DESTROY: {
        HBITMAP logo=reinterpret_cast<HBITMAP>(GetWindowLongPtrW(dialog,DWLP_USER));
        if(logo) DeleteObject(logo);
        return TRUE;
    }
    }
    return FALSE;
}

void showAbout() {
    cancelHover(g_hwnd);
    g_contextOpen=true;
    DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_ABOUT_DIALOG),
                    g_hwnd,AboutDialogProc,0);
    g_contextOpen=false;
}

BYTE channel(double value) {
    return static_cast<BYTE>((std::max)(0.0, (std::min)(255.0, value)));
}

COLORREF pulseColor(LoadLevel level, double phase) {
    double rWave = (std::sin)(phase) + 1.0;
    double gWave = (std::sin)(phase + 0.55) + 1.0;
    double bWave = (std::sin)(phase + 1.10) + 1.0;
    rWave *= 0.5; gWave *= 0.5; bWave *= 0.5;

    if (level == LoadLevel::Normal) {
        // Intel blue-green breathing: hue remains blue-green while brightness rises/falls
        double breath = 0.45 + 0.55 * gWave;
        return RGB(channel(1.0), channel(160.0 * breath), channel(230.0 * breath));
    }
    if (level == LoadLevel::Yellow) {
        // Yellow-dominant: red and green breathe with small blue phase offset
        return RGB(channel(205.0 + 50.0 * rWave), channel(145.0 + 100.0 * gWave), channel(4.0 + 24.0 * bWave));
    }
    // Red-biased: strong red with subtle green/blue shifts
    return RGB(channel(170.0 + 85.0 * rWave), channel(5.0 + 42.0 * gWave), channel(5.0 + 30.0 * bWave));
}

COLORREF animatedBorderColor(LoadLevel level, ULONGLONG now) {
    double period = level == LoadLevel::Normal ? 2400.0 : (level == LoadLevel::Yellow ? 1400.0 : 1050.0);
    double phase = 6.283185307179586 * (static_cast<double>(now % static_cast<ULONGLONG>(period)) / period);
    return pulseColor(level, phase);
}

COLORREF cpuLabelColor(double load, ULONGLONG now) {
    if (load < 0) return RGB(110, 110, 116);
    const auto level = levelForPercent(static_cast<unsigned>(std::lround(std::clamp(load, 0.0, 100.0))));
    if (level == LoadLevel::Normal) return RGB(0, 128, 255);
    return animatedBorderColor(level, now);
}

void refreshDisplayCache() {
    Stats stats;
    {
        std::lock_guard<std::mutex> guard(g_statsMutex);
        stats = g_snapshot;
    }
    g_cpuLoad = stats.cpu;
    constexpr double GIB = 1073741824.0;
    const unsigned ramPercent = stats.total ? static_cast<unsigned>(100 * stats.used / stats.total) : 0;
    const unsigned cpuPercent = stats.cpu >= 0 ? static_cast<unsigned>(std::lround(stats.cpu)) : 0;
    g_loadLevel = levelForPercent(std::max(cpuPercent, ramPercent));
    g_compactParts = {
        stats.cpu >= 0 ? formatText(L"CPU %.0f%%", stats.cpu) : L"CPU N/A",
        stats.total ? formatText(L"RAM %.1f/%.1fG", stats.used/GIB, stats.total/GIB) : L"RAM N/A",
        stats.temperature >= 0 ? formatText(L"Temp %.0f\u00B0C", stats.temperature) : L"Temp N/A",
        stats.gpu >= 0 ? formatText(L"iGPU %.0f%%", stats.gpu) : L"iGPU N/A"
    };
    g_expandedParts = {
        stats.cpu >= 0 ? formatText(L"CPU Load  %.0f%%", stats.cpu) : L"CPU Load  N/A",
        stats.temperature >= 0 ? formatText(L"CPU Package  %.0f\u00B0C", stats.temperature) : L"CPU Package  N/A",
        stats.total ? formatText(L"RAM  %.1f/%.1f GiB (%u%%)", stats.used/GIB, stats.total/GIB, ramPercent) : L"RAM  N/A",
        stats.gpu >= 0 ? formatText(L"iGPU Load  %.0f%%", stats.gpu) : L"iGPU Load  N/A",
        formatText(L"Fan Mode  %s", fanModeName(stats.fans)),
        stats.fans.ok ? formatText(L"Fan 1 %lu RPM | Fan 2 %lu RPM", stats.fans.fan1, stats.fans.fan2) : L"Fan 1 N/A | Fan 2 N/A"
    };
}
void positionLeft(HWND hwnd) {
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0,0}, MONITOR_DEFAULTTOPRIMARY), &info);
    RECT own{}; GetWindowRect(hwnd, &own);
    int x = info.rcWork.left + 18, y = info.rcWork.top + 18;
    RECT target{x, y, x + own.right - own.left, y + own.bottom - own.top};
    HWND ai = FindWindowW(L"X1AIIslandClass", L"X1 AI Island");
    RECT other{}, intersection{};
    if (ai && GetWindowRect(ai, &other) && IntersectRect(&intersection, &target, &other))
        y = other.bottom + 16;
    // Also reserve the centered AI island's default expanded footprint when it is not running.
    RECT reserved{info.rcWork.left + (info.rcWork.right-info.rcWork.left-560)/2, info.rcWork.top+18,
        info.rcWork.left + (info.rcWork.right-info.rcWork.left+560)/2, info.rcWork.top+160};
    if (IntersectRect(&intersection, &target, &reserved)) y = std::max<int>(y, reserved.bottom + 16);
    y = std::max<int>(info.rcWork.top, std::min<int>(y, info.rcWork.bottom - (own.bottom-own.top)));
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}
void setWindowSize() {
    int w = ISLAND_WIDTH;
    int h = g_expanded ? EXPANDED_HEIGHT : COMPACT_HEIGHT;
    RECT r{}; GetWindowRect(g_hwnd, &r);
    SetWindowPos(g_hwnd, HWND_TOPMOST, r.left, r.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    HRGN region = CreateRoundRectRgn(0, 0, w + 1, h + 1, 24, 24);
    if (!SetWindowRgn(g_hwnd, region, TRUE) && region) DeleteObject(region);
}

void drawIntelLogo(HDC dc, int x, int y) {
    if (!g_intelLogo) return;
    HDC logoDc = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(logoDc, g_intelLogo);
    BitBlt(dc, x, y, 38, 28, logoDc, 0, 0, SRCCOPY);
    SelectObject(logoDc, old);
    DeleteDC(logoDc);
}

void render(HDC dc, const RECT& rc) {
    FillRect(dc, &rc, g_backgroundBrush);

    ULONGLONG now = g_onBattery ? 0 : GetTickCount64();
    HPEN border = CreatePen(PS_SOLID, 1, animatedBorderColor(g_loadLevel, now));
    HGDIOBJ oldPen = SelectObject(dc, border);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, 1, 1, rc.right - 1, rc.bottom - 1, 24, 24);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(border);

    SetBkMode(dc, TRANSPARENT);
    drawIntelLogo(dc, 10, 9);
    SetTextColor(dc, TEXT_COLOR);
    SelectObject(dc, g_metricsFont);
    const auto& parts = g_compactParts;
    std::array<SIZE, 4> sizes{};
    int totalWidth = 0;
    int activeSegments = 0;

    for (size_t i = 0; i < parts.size(); ++i) {
        if (parts[i].empty()) continue;
        GetTextExtentPoint32W(dc, parts[i].c_str(), static_cast<int>(parts[i].size()), &sizes[i]);
        totalWidth += sizes[i].cx;
        activeSegments++;
    }

    const int left = 58;
    const int right = rc.right - 12;
    int extra = std::max(0, right - left - totalWidth);
    int gapCount = std::max(1, activeSegments - 1);
    int gap = extra / gapCount;
    int remainder = extra % gapCount;
    int x = left;
    int drawn = 0;

    for (size_t i = 0; i < parts.size(); ++i) {
        if (parts[i].empty()) continue;

        int y = (46 - sizes[i].cy) / 2;
        if (i == 0) {
            // Like AI Island's GPU name: color only the label, never the value.
            SIZE labelSize{};
            GetTextExtentPoint32W(dc, L"CPU", 3, &labelSize);
            SetTextColor(dc, cpuLabelColor(g_cpuLoad, now));
            TextOutW(dc, x, y, L"CPU", 3);
            SetTextColor(dc, TEXT_COLOR);
            TextOutW(dc, x + labelSize.cx, y, parts[i].c_str() + 3, static_cast<int>(parts[i].size()) - 3);
        } else {
            TextOutW(dc, x, y, parts[i].c_str(), static_cast<int>(parts[i].size()));
        }
        drawn++;
        if (drawn < activeSegments)
            x += sizes[i].cx + gap + (drawn <= remainder ? 1 : 0);
    }

    if (g_expanded) {
        SelectObject(dc, g_smallFont);
        SetTextColor(dc, TEXT_COLOR);
        constexpr UINT FLAGS = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 2; ++col) {
                RECT cell{col == 0 ? 14 : rc.right / 2 + 6, 46 + row * 25,
                          col == 0 ? rc.right / 2 - 4 : rc.right - 12, 71 + row * 25};
                DrawTextW(dc, g_expandedParts[row * 2 + col].c_str(), -1, &cell, FLAGS);
            }
        }
    }
}

void paint() {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(g_hwnd, &ps);
    RECT rc{}; GetClientRect(g_hwnd, &rc);
    render(dc, rc);
    EndPaint(g_hwnd, &ps);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SHOWWINDOW:
        g_sampling.store(wp != 0 && !g_suspended);
        if (g_policyEvent) SetEvent(g_policyEvent);
        if (!wp) stopAnimation(hwnd);
        break;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMSUSPEND) {
            g_suspended = true;
            g_sampling.store(false);
            if (g_policyEvent) SetEvent(g_policyEvent);
            stopAnimation(hwnd);
        } else if (wp == PBT_APMPOWERSTATUSCHANGE || wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
            g_suspended = false;
            updateSamplingPolicy(hwnd);
            startAnimation(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return TRUE;
    case WM_STATS_READY:
        if (IsWindowVisible(hwnd)) {
            refreshDisplayCache();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_SHOW_EXISTING_ISLAND:
        showIsland(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_CREATE:
        // The worker delivers display updates; no independent polling timer.
        g_hotkeyChoice = loadHotkeyChoice();
        if (!registerToggleHotkey(hwnd, g_hotkeyChoice))
            MessageBoxW(hwnd, L"Hide/show shortcut is already in use. Choose another from the right-click menu. Launch this app again to restore it if hidden.", L"X1 SYS Island", MB_OK | MB_ICONWARNING);
        startAnimation(hwnd);
        return 0;

    case WM_TIMER:
        if(wp==STATS_TIMER_ID) {

            refreshDisplayCache();
            InvalidateRect(hwnd,nullptr,FALSE);
          } else if(wp==ANIMATION_TIMER_ID) {
              // A layered, non-activating HWND can miss a leave notification
              // after being shown under a stationary cursor. Reconcile on the
              // existing animation tick so the next real entry always rearms.
              if(IsWindowVisible(hwnd) && !g_userHidden && !g_hoverHidden && !g_dragging && !g_contextOpen) {
                  if(!cursorInside(hwnd)) cancelHover(hwnd);
                  else if(!g_trackingMouse && !g_hoverConsumed) {
                      g_trackingMouse=true;
                      SetTimer(hwnd,HOVER_TIMER_ID,1000,nullptr);
                  }
              }
              if (!g_onBattery) InvalidateRect(hwnd,nullptr,FALSE);
        } else if(wp==HOVER_TIMER_ID) {
            KillTimer(hwnd,HOVER_TIMER_ID);
            if(!g_contextOpen && !g_dragging && !g_userHidden && cursorInside(hwnd)) {
                g_hoverConsumed=true;
                g_hoverHidden=true;
                g_trackingMouse=false;
                stopAnimation(hwnd);
                ShowWindow(hwnd,SW_HIDE);
                SetTimer(hwnd,RESHOW_TIMER_ID,5000,nullptr);
            }
        } else if(wp==RESHOW_TIMER_ID) {
            KillTimer(hwnd,RESHOW_TIMER_ID);
            if(g_hoverHidden && !g_userHidden) {
                showIsland(hwnd);
                if(cursorInside(hwnd)) {
                    TRACKMOUSEEVENT tme{sizeof(tme),TME_LEAVE,hwnd,0};
                    TrackMouseEvent(&tme);
                    g_trackingMouse=true;
                } else {
                    g_hoverConsumed=false;
                }
            }
        }
        return 0;
    case WM_HOTKEY:
        if (wp == HOTKEY_ID) toggleIsland(hwnd);
        return 0;

    case WM_LBUTTONDBLCLK:
        g_expanded = !g_expanded; setWindowSize(); return 0;

    case WM_LBUTTONDOWN:
        cancelHover(hwnd);
        g_dragging = true; SetCapture(hwnd);
        g_dragStart = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        return 0;

    case WM_MOUSEMOVE:
        if(g_dragging && (wp & MK_LBUTTON)) {
            POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}; ClientToScreen(hwnd,&p);
            SetWindowPos(hwnd,HWND_TOPMOST,p.x-g_dragStart.x,p.y-g_dragStart.y,0,0,SWP_NOSIZE|SWP_NOACTIVATE);
        } else if(!g_trackingMouse && !g_contextOpen) {
            TRACKMOUSEEVENT tme{sizeof(tme),TME_LEAVE,hwnd,0};
            TrackMouseEvent(&tme);
            g_trackingMouse=true;
            if(!g_hoverConsumed) SetTimer(hwnd,HOVER_TIMER_ID,1000,nullptr);
        }
        return 0;
    case WM_MOUSELEAVE:
        // Hiding the HWND itself causes WM_MOUSELEAVE. Preserve the consumed
        // hover until the cursor actually leaves after the island returns.
        if (g_hoverHidden) { KillTimer(hwnd, HOVER_TIMER_ID); g_trackingMouse = false; }
        else cancelHover(hwnd);
        return 0;
    case WM_LBUTTONUP:
        g_dragging=false; ReleaseCapture(); return 0;
    case WM_RBUTTONDOWN:
        cancelHover(hwnd);
        return 0;
    case WM_RBUTTONUP: {
        cancelHover(hwnd);
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, 1, L"Expand / Collapse");
        AppendMenuW(m, MF_STRING, 6, L"Reset to left corner");
        std::wstring hideLabel = L"Hide Island\t";
        if (g_hotkeyChoice >= 0 && g_hotkeyChoice < static_cast<int>(ARRAYSIZE(HOTKEYS))) {
            hideLabel += HOTKEYS[g_hotkeyChoice].label;
        } else {
            hideLabel += L"N/A";
        }
        AppendMenuW(m, MF_STRING, 3, hideLabel.c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

        HMENU shortcuts = CreatePopupMenu();
        for (int i = 0; i < static_cast<int>(ARRAYSIZE(HOTKEYS)); ++i) {
            UINT flags = MF_STRING | (i == g_hotkeyChoice ? MF_CHECKED : 0);
            AppendMenuW(shortcuts, flags, 100 + i, HOTKEYS[i].label);
        }
        AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(shortcuts), L"Hide / show shortcut");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

        AppendMenuW(m, MF_STRING, 5, L"About X1 SYS Island");
        AppendMenuW(m, MF_STRING, 2, L"Exit");

        POINT p{}; GetCursorPos(&p);
        SetForegroundWindow(hwnd);
        g_contextOpen = true;
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, nullptr);
        g_contextOpen = false;
        DestroyMenu(m);

        if (cmd == 1) { g_expanded = !g_expanded; setWindowSize(); }
        if (cmd == 6) positionLeft(hwnd);
        if (cmd == 3) toggleIsland(hwnd);
        if (cmd >= 100 && cmd < 100 + static_cast<int>(ARRAYSIZE(HOTKEYS))) selectHotkey(hwnd, cmd - 100);

        if (cmd == 5) showAbout();
        if (cmd == 2) DestroyWindow(hwnd);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == 5) { showAbout(); return 0; }
        break;

    case WM_PAINT: paint(); return 0;

    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION:
        if (wp) DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_stopEvent) SetEvent(g_stopEvent);
        KillTimer(hwnd, STATS_TIMER_ID);
        KillTimer(hwnd, HOVER_TIMER_ID);
        KillTimer(hwnd, RESHOW_TIMER_ID);
        KillTimer(hwnd, ANIMATION_TIMER_ID);
        if (g_hotkeyRegistered) UnregisterHotKey(hwnd, HOTKEY_ID);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE h, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_singleInstanceMutex = CreateMutexW(nullptr, FALSE, SINGLE_INSTANCE_MUTEX);
    if (g_singleInstanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_singleInstanceMutex);
        g_singleInstanceMutex = nullptr;
        HWND existing{};
        for (int attempt = 0; attempt < 40 && !existing; ++attempt) {
            existing = FindWindowW(L"X1SYSIslandClass", L"X1 SYS Island");
            if (!existing) Sleep(50);
        }
        if (existing) PostMessageW(existing, WM_SHOW_EXISTING_ISLAND, 0, 0);
        return 0;
    }

    refreshDisplayCache();

    g_metricsFont = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_smallFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    // Try to load Intel logo bitmap (if available)
    g_intelLogo = LoadBitmapW(h, MAKEINTRESOURCEW(IDB_INTEL_LOGO));

    g_backgroundBrush = CreateSolidBrush(RGB(18, 18, 20));

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc; wc.hInstance = h; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"X1SYSIslandClass";
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    const int initialWidth = ISLAND_WIDTH;
    const int initialX = std::max(0, (sw - initialWidth) / 2);
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        wc.lpszClassName, L"X1 SYS Island", WS_POPUP,
        initialX, 18, initialWidth, 46, nullptr, nullptr, h, nullptr);
    if (!g_hwnd) return 1;
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_policyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_stopEvent || !g_policyEvent) {
        if (g_stopEvent) CloseHandle(g_stopEvent);
        if (g_policyEvent) CloseHandle(g_policyEvent);
        return 1;
    }
    updateSamplingPolicy(g_hwnd);
    try {
        g_worker = std::thread([] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            try {
                std::unique_ptr<Telemetry> telemetry;
                HANDLE events[] = {g_stopEvent, g_policyEvent};
                DWORD delay = 0;
                for (;;) {
                    DWORD wake = WaitForMultipleObjects(2, events, FALSE, delay);
                    if (wake == WAIT_OBJECT_0 || wake == WAIT_FAILED) break;
                    if (!g_sampling.load()) {
                        // Release the PDH query and PawnIO device/module while inactive.
                        telemetry.reset();
                        delay = INFINITE;
                        continue;
                    }
                    if (!telemetry) {
                        telemetry = std::make_unique<Telemetry>();
                        delay = g_sampleInterval.load(); // Prime rate counters first.
                        continue;
                    }
                    if (wake == WAIT_TIMEOUT) {
                        Stats snapshot = telemetry->sample();
                        {
                            std::lock_guard<std::mutex> guard(g_statsMutex);
                            g_snapshot = std::move(snapshot);
                        }
                        PostMessageW(g_hwnd, WM_STATS_READY, 0, 0);
                    }
                    delay = g_sampleInterval.load();
                }
            } catch (...) {
                PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
            }
        });
    } catch (...) {
        CloseHandle(g_policyEvent);
        CloseHandle(g_stopEvent);
        return 1;
    }
    SetLayeredWindowAttributes(g_hwnd, 0, ISLAND_OPACITY, LWA_ALPHA);
    setWindowSize();
    positionLeft(g_hwnd);
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    startAnimation(g_hwnd);
    UpdateWindow(g_hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    SetEvent(g_stopEvent);
    g_worker.join();
    CloseHandle(g_stopEvent);
    CloseHandle(g_policyEvent);
    DeleteObject(g_metricsFont); DeleteObject(g_smallFont);
    if (g_intelLogo) DeleteObject(g_intelLogo);
    if (g_backgroundBrush) DeleteObject(g_backgroundBrush);
    if (g_singleInstanceMutex) CloseHandle(g_singleInstanceMutex);
    return 0;
}



