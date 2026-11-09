#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <string>
#include <algorithm>
#include <array>
#include <vector>
#include <cstdarg>
#include <cmath>

#include <wtsapi32.h>

#include "resource.h"
#include "telemetry.h"
#include "cpu_identity.h"
#include "island_style.h"
#include "settings.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "wtsapi32.lib")

// Native Win32/GDI overlay; telemetry runs on a separate worker.
Stats g_snapshot; // Written by the worker under g_statsMutex.
std::mutex g_statsMutex;
HANDLE g_stopEvent;
HANDLE g_policyEvent;
std::atomic<bool> g_sampling{false};
std::atomic<DWORD> g_sampleInterval{1000};
enum class SamplingMode { Released, Active, Hover };
struct SamplingPolicy {
    SamplingMode mode = SamplingMode::Released;
    DWORD interval = 1000;
    ULONGLONG hoverDeadline = 0;
    unsigned resumeEpoch = 0, releaseEpoch = 0;
};
std::mutex g_policyMutex;
SamplingPolicy g_policy;
constexpr DWORD HOVER_RETENTION_MS = 6500;

SamplingPolicy samplingPolicy() {
    std::lock_guard<std::mutex> guard(g_policyMutex);
    return g_policy;
}
void publishSamplingPolicy(bool visible);

// Deadlines survive spurious policy wakes; epochs preserve transitions even
// when an auto-reset event coalesces a quick hide/show or suspend/resume.
struct WorkerSchedule {
    ULONGLONG nextSample = 0;
    unsigned resumeEpoch = 0, releaseEpoch = 0;
    bool retained = false;
    bool mustRelease(const SamplingPolicy& policy, ULONGLONG now) const {
        return policy.mode == SamplingMode::Released || releaseEpoch != policy.releaseEpoch ||
            ((retained || resumeEpoch != policy.resumeEpoch) &&
                policy.hoverDeadline && now >= policy.hoverDeadline);
    }
    DWORD delayUntil(ULONGLONG deadline, ULONGLONG now) const {
        return deadline > now ? static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, INFINITE - 1)) : 0;
    }
};
bool g_onBattery = false;
// Reasons nobody can see the island. Any of them releases telemetry and stops
// animation, exactly like suspension.
bool g_suspended = false;
bool g_displayOff = false;
bool g_sessionLocked = false;
bool presentationPaused() { return g_suspended || g_displayOff || g_sessionLocked; }
constexpr UINT WM_STATS_READY = WM_APP + 1;
std::thread g_worker;
constexpr int ISLAND_WIDTH = 520;
constexpr int COMPACT_HEIGHT = 46;
constexpr int EXPANDED_HEIGHT = 128;
double g_cpuLoad = -1;
const CpuIdentity g_cpuIdentity = detectCpuIdentity();
constexpr COLORREF TEXT_COLOR = RGB(242, 242, 245);
constexpr COLORREF EXPANDED_LABEL_COLOR = RGB(155, 160, 168);
constexpr COLORREF EXPANDED_VALUE_COLOR = RGB(235, 238, 242);
HWND g_hwnd;
HANDLE g_singleInstanceMutex;

HFONT g_metricsFont;
HFONT g_smallFont;
HBITMAP g_intelLogo;
HDC g_logoDc;
HGDIOBJ g_logoPrevious;
void releaseLogoDc() {
    if (!g_logoDc) return;
    SelectObject(g_logoDc, g_logoPrevious);
    DeleteDC(g_logoDc);
    g_logoDc = nullptr;
    g_logoPrevious = nullptr;
}
HBRUSH g_backgroundBrush;
std::array<std::wstring, 4> g_compactParts;

std::array<std::wstring, 9> g_expandedParts;


POINT g_dragStart;
bool g_dragging = false;
bool g_expanded = false;
bool g_hoverConsumed = false;
bool g_trackingMouse = false;
bool g_contextOpen = false;
bool g_userHidden = false;
bool g_hoverHidden = false;
bool g_autoHideOnHover = true;
bool g_hotkeyRegistered = false;
bool g_contextHotkeyRegistered = false;
int g_hotkeyChoice = 0;

const UINT_PTR HOVER_TIMER_ID = 2;
const UINT_PTR RESHOW_TIMER_ID = 3;
const UINT_PTR ANIMATION_TIMER_ID = 4;
const UINT_PTR TELEMETRY_HEALTH_TIMER_ID = 5;
const UINT ANIMATION_INTERVAL_MS = 100;
const UINT BATTERY_ANIMATION_INTERVAL_MS = 1000;
const UINT TELEMETRY_HEALTH_INTERVAL_MS = 15000;
const int HOTKEY_ID = 100;
const int CONTEXT_MENU_HOTKEY_ID = 200;
const UINT WM_SHOW_EXISTING_ISLAND = WM_USER + 1;
const wchar_t SINGLE_INSTANCE_MUTEX[] = L"Global\\X1SYSIslandMutex";

const BYTE ISLAND_OPACITY = 230;

enum MenuCommand : int {
    MENU_TOGGLE_EXPANDED = 1,
    MENU_EXIT = 2,
    MENU_HIDE = 3,
    MENU_AUTO_HIDE = 4,
    MENU_ABOUT = 5,
    MENU_RESET_POSITION = 6,
    MENU_HOTKEY_FIRST = 100
};

LoadLevel g_loadLevel = LoadLevel::Normal;
void ensureExpandedLayout(HDC dc);
void invalidateDisplayChanges(const std::array<std::wstring, 4>& compact,
    const std::array<std::wstring, 9>& expanded, double cpu, LoadLevel level);

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
void publishSamplingPolicy(bool visible) {
    const auto mode = presentationPaused() || g_userHidden ? SamplingMode::Released
        : visible ? SamplingMode::Active
        : g_hoverHidden ? SamplingMode::Hover : SamplingMode::Released;
    const DWORD interval = g_onBattery ? 3000 : 1000;
    std::lock_guard<std::mutex> guard(g_policyMutex);
    g_sampling.store(mode == SamplingMode::Active);
    g_sampleInterval.store(interval);
    if (g_policy.mode == mode && g_policy.interval == interval) return;
    if (g_policy.mode != mode) {
        if (mode == SamplingMode::Hover)
            g_policy.hoverDeadline = GetTickCount64() + HOVER_RETENTION_MS;
        if (mode == SamplingMode::Active) ++g_policy.resumeEpoch;
        if (mode == SamplingMode::Released) ++g_policy.releaseEpoch;
    }
    g_policy.mode = mode;
    g_policy.interval = interval;
    if (g_policyEvent) SetEvent(g_policyEvent);
}
void updateSamplingPolicy(HWND hwnd) {
    SYSTEM_POWER_STATUS power{};
    g_onBattery = !GetSystemPowerStatus(&power) || power.ACLineStatus != 1 || power.SystemStatusFlag == 1;
    publishSamplingPolicy(IsWindowVisible(hwnd) != FALSE);
}
void startAnimation(HWND hwnd) {
    if (!IsWindowVisible(hwnd) || g_userHidden || g_hoverHidden || presentationPaused()) return;
    // Tolerance lets Windows batch this wakeup with others. Small on AC to
    // keep the pulse smooth; generous on battery where it only reconciles hover.
    const UINT interval = g_onBattery ? BATTERY_ANIMATION_INTERVAL_MS : ANIMATION_INTERVAL_MS;
    SetCoalescableTimer(hwnd, ANIMATION_TIMER_ID, interval, nullptr, g_onBattery ? 250 : 10);
}

void stopAnimation(HWND hwnd) {
    KillTimer(hwnd,ANIMATION_TIMER_ID);
}

bool cursorInside(HWND hwnd);
void ensureTopmostVisible(HWND hwnd) {
    if (IsWindowVisible(hwnd) && !g_userHidden && !g_hoverHidden)
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
void showIsland(HWND hwnd) {
    g_userHidden=false;
    g_hoverHidden=false;
    KillTimer(hwnd,RESHOW_TIMER_ID);
    ShowWindow(hwnd,SW_SHOWNOACTIVATE);
    ensureTopmostVisible(hwnd);
    KillTimer(hwnd,HOVER_TIMER_ID);
    g_hoverConsumed=g_autoHideOnHover && cursorInside(hwnd);
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

void refreshDisplayCache() {
    Readings stats;
    {
        std::lock_guard<std::mutex> guard(g_statsMutex);
        stats = g_snapshot; // Numeric slice only; the UI never shows the diagnostic strings.
    }
    const auto oldCpu = g_cpuLoad;
    const auto oldLevel = g_loadLevel;
    g_cpuLoad = stats.cpu;
    constexpr double GIB = 1073741824.0;
    const unsigned ramPercent = stats.total ? static_cast<unsigned>(100 * stats.used / stats.total) : 0;
    const unsigned cpuPercent = stats.cpu >= 0 ? static_cast<unsigned>(std::lround(stats.cpu)) : 0;
    g_loadLevel = levelForPercent(std::max(cpuPercent, ramPercent));
    std::array<std::wstring, 4> compact{
        stats.cpu >= 0 ? formatText(L"%.0f%%", stats.cpu) : L"N/A",
        stats.total ? formatText(L"%.1f/%.1fG", stats.used/GIB, stats.total/GIB) : L"N/A",
        stats.temperature >= 0 && stats.tjMax >= 0
            ? formatText(L"%.0f\u00B0C/%d\u00B0C", stats.temperature, stats.tjMax) : L"N/A",
        stats.gpu >= 0 ? formatText(L"%.0f%%", stats.gpu) : L"N/A"
    };
    std::array<std::wstring, 9> expanded{
        stats.cpu >= 0 ? formatText(L"%.0f%%", stats.cpu) : L"N/A",
        stats.temperature >= 0 ? formatText(L"%.0f\u00B0C", stats.temperature) : L"N/A",
        stats.tjMax >= 0 ? formatText(L"%d\u00B0C", stats.tjMax) : L"N/A",
        stats.gpu >= 0 ? formatText(L"%.0f%%", stats.gpu) : L"N/A",
        stats.total ? formatText(L"%.1f/%.1f GiB", stats.used/GIB, stats.total/GIB) : L"N/A",
        stats.total ? formatText(L"%u%%", ramPercent) : L"N/A",
        fanModeName(stats.fans),
        stats.fans.ok ? formatText(L"%lu", stats.fans.fan1) : L"N/A",
        stats.fans.ok ? formatText(L"%lu", stats.fans.fan2) : L"N/A"
    };
    // After the swaps the locals hold the previous text for change detection.
    g_compactParts.swap(compact);
    g_expandedParts.swap(expanded);
    invalidateDisplayChanges(compact, expanded, oldCpu, oldLevel);
}
void positionRight(HWND hwnd) {
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0,0}, MONITOR_DEFAULTTOPRIMARY), &info);
    RECT own{}; GetWindowRect(hwnd, &own);
    int x = info.rcWork.right - (own.right - own.left) - 36,
        y = info.rcWork.top + 36;
    RECT target{x, y, x + own.right - own.left, y + own.bottom - own.top};
    HWND ai = FindWindowW(L"X1AIIslandClass", L"X1 AI Island");
    RECT other{}, intersection{};
    // Stack below a running AI island that would overlap, as for its reserved footprint below.
    if (ai && GetWindowRect(ai, &other) && IntersectRect(&intersection, &target, &other))
        y = std::max<int>(y, other.bottom + 16);
    // Also reserve the centered AI island's default expanded footprint when it is not running.
    RECT reserved{info.rcWork.left + (info.rcWork.right-info.rcWork.left-560)/2,
        info.rcWork.top+18,
        info.rcWork.left + (info.rcWork.right-info.rcWork.left+560)/2, info.rcWork.top+160};
    if (IntersectRect(&intersection, &target, &reserved)) y = std::max<int>(y, reserved.bottom + 16);
    y = std::max<int>(info.rcWork.top, std::min<int>(y, info.rcWork.bottom - (own.bottom - own.top)));
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}
void setWindowSize() {
    int w = ISLAND_WIDTH;
    int h = g_expanded ? EXPANDED_HEIGHT : COMPACT_HEIGHT;
    // One SetWindowPos resizes and, when shown, also reasserts topmost order.
    const bool topmost = IsWindowVisible(g_hwnd) && !g_userHidden && !g_hoverHidden;
    SetWindowPos(g_hwnd, topmost ? HWND_TOPMOST : nullptr, 0, 0, w, h,
        SWP_NOMOVE | SWP_NOACTIVATE | (topmost ? 0 : SWP_NOZORDER));
    HRGN region = CreateRoundRectRgn(0, 0, w + 1, h + 1, 24, 24);
    if (!SetWindowRgn(g_hwnd, region, TRUE) && region) DeleteObject(region);
}

struct TextCell { RECT label, value; };
// Geometry never depends on live digits. Long identities/values are clipped
// with ellipsis within their own cell rather than shifting their neighbours.
const std::array<TextCell, 4> COMPACT_CELLS{{
    {{58, 9, 116, 37}, {118, 9, 162, 37}},
    {{0, 0, 0, 0},     {178, 9, 268, 37}},
    {{0, 0, 0, 0},     {284, 9, 388, 37}},
    {{404, 9, 448, 37}, {450, 9, 506, 37}}
}};
const std::array<RECT, 9> EXPANDED_CELLS{{
    {14, 46, 172, 71}, {182, 46, 340, 71}, {350, 46, 508, 71},
    {14, 71, 172, 96}, {182, 71, 340, 96}, {350, 71, 508, 96},
    {14, 96, 172, 121}, {182, 96, 340, 121}, {350, 96, 508, 121}
}};
const std::array<std::wstring, 9> EXPANDED_LABELS{
    g_cpuIdentity.expanded, L"CPU Package", L"TjMax", L"iGPU", L"RAM", L"RAM Usage",
    L"Fan Mode", L"Fan 1 (rpm)", L"Fan 2 (rpm)"};
std::array<TextCell, 9> g_expandedCells{};
HFONT g_layoutFont = nullptr;
void ensureExpandedLayout(HDC dc) {
    if (g_layoutFont == g_smallFont || !g_smallFont) return;
    const auto previous = SelectObject(dc, g_metricsFont);
    SIZE cpuValue{};
    GetTextExtentPoint32W(dc, L"100%", 4, &cpuValue);
    SelectObject(dc, g_smallFont);
    // Measure labels once per font; values share each column's fixed right edge.
    for (size_t i = 0; i < g_expandedCells.size(); ++i) {
        const RECT row = EXPANDED_CELLS[i];
        SIZE label{};
        GetTextExtentPoint32W(dc, EXPANDED_LABELS[i].c_str(),
            static_cast<int>(EXPANDED_LABELS[i].size()), &label);

        const LONG end = i == 0 ? std::min(row.left + label.cx, row.right - cpuValue.cx - 8)
            : row.left + label.cx;
        g_expandedCells[i] = {{row.left, row.top, end, row.bottom},
            {end + 8, row.top, row.right, row.bottom}};
    }
    SelectObject(dc, previous);
    g_layoutFont = g_smallFont;
}
const std::array<std::wstring, 4> COMPACT_LABELS{g_cpuIdentity.compact, L"", L"", L"iGPU"};
constexpr UINT LABEL_TEXT_FORMAT = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;

// The CPU-label halo draws the same text 24 times per animation frame.
// DrawTextW re-runs ellipsis layout for each copy; resolving the displayed
// string and baseline once per font lets the halo use plain ExtTextOutW.
struct GlowLabelLayout {
    HFONT font = nullptr;
    std::wstring text;
    int top = 0;
};
// Requires the label's font to be selected into dc.
GlowLabelLayout layoutGlowLabel(HDC dc, const std::wstring& label, const RECT& cell) {
    // DT_MODIFYSTRING may append up to four characters for the ellipsis.
    std::wstring text = label;
    text.append(4, L'\0');
    RECT bounds = cell;
    DrawTextW(dc, text.data(), static_cast<int>(label.size()), &bounds,
        LABEL_TEXT_FORMAT | DT_MODIFYSTRING | DT_CALCRECT);
    text.resize(wcslen(text.c_str()));
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    // DT_VCENTER for a single line centres the font's full cell height.
    return {static_cast<HFONT>(GetCurrentObject(dc, OBJ_FONT)), std::move(text),
        static_cast<int>(cell.top + (cell.bottom - cell.top - metrics.tmHeight) / 2)};
}
// Pixel-identical to DrawTextW(label, cell offset by dx/dy, LABEL_TEXT_FORMAT).
void drawGlowCopy(HDC dc, const GlowLabelLayout& layout, const RECT& cell, int dx, int dy) {
    RECT clip = cell;
    OffsetRect(&clip, dx, dy);
    ExtTextOutW(dc, clip.left, layout.top + dy, ETO_CLIPPED, &clip,
        layout.text.c_str(), static_cast<UINT>(layout.text.size()), nullptr);
}
GlowLabelLayout g_cpuGlowLayout;

std::array<RECT, 6> borderRects(const RECT& rc) {
    // Edge strips cover the rounded corners without touching the text/logo.
    // The innermost glow layer's bottom arcs also reach one pixel beside each
    // side strip; widening the strips would intersect the logo/expanded cells.
    return {{{0, 0, rc.right, 9}, {0, rc.bottom - 7, rc.right, rc.bottom},
        {0, 9, 10, rc.bottom - 7}, {rc.right - 10, 9, rc.right, rc.bottom - 7},
        {10, rc.bottom - 8, 11, rc.bottom - 7}, {rc.right - 11, rc.bottom - 8, rc.right - 10, rc.bottom - 7}}};
}
void invalidateBorder(HWND hwnd) {
    RECT rc{}; GetClientRect(hwnd, &rc);
    for (const auto& rect : borderRects(rc)) InvalidateRect(hwnd, &rect, FALSE);
}
void invalidateAnimation(HWND hwnd) {
    invalidateBorder(hwnd);
    InvalidateRect(hwnd, &COMPACT_CELLS[0].label, FALSE);
}
void invalidateDisplayChanges(const std::array<std::wstring, 4>& compact,
    const std::array<std::wstring, 9>& expanded, double cpu, LoadLevel level) {
    if (!g_hwnd) return;
    if (g_layoutFont != g_smallFont) {
        HDC dc = GetDC(g_hwnd);
        if (dc) { ensureExpandedLayout(dc); ReleaseDC(g_hwnd, dc); }
    }
    for (size_t i = 0; i < compact.size(); ++i)
        if (compact[i] != g_compactParts[i]) InvalidateRect(g_hwnd, &COMPACT_CELLS[i].value, FALSE);
    if (g_expanded) for (size_t i = 0; i < expanded.size(); ++i)
        if (expanded[i] != g_expandedParts[i]) InvalidateRect(g_hwnd, &g_expandedCells[i].value, FALSE);
    const auto now = g_onBattery ? 0 : GetTickCount64();
    if (cpuLabelColor(cpu, now) != cpuLabelColor(g_cpuLoad, now))
        InvalidateRect(g_hwnd, &COMPACT_CELLS[0].label, FALSE);
    if (level != g_loadLevel) invalidateBorder(g_hwnd);
}

struct BackBuffer {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    int width = 0, height = 0;
    bool valid = false;
    BackBuffer() = default;
    BackBuffer(const BackBuffer&) = delete;
    BackBuffer& operator=(const BackBuffer&) = delete;
    ~BackBuffer() { release(); }
    void release() {
        if (dc && previous) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        dc = nullptr; bitmap = nullptr; previous = nullptr;
        width = height = 0; valid = false;
    }
    bool ensure(HDC target, int w, int h) {
        if (dc && width == w && height == h) return true;
        release();
        if (w <= 0 || h <= 0) return false;
        dc = CreateCompatibleDC(target);
        if (dc) bitmap = CreateCompatibleBitmap(target, w, h);
        if (bitmap) previous = SelectObject(dc, bitmap);
        if (!previous || previous == HGDI_ERROR) { previous = nullptr; release(); return false; }
        width = w; height = h;
        return true;
    }
};
BackBuffer g_backBuffer;
struct DamageRegion {
    HRGN region = nullptr;
    ~DamageRegion() { release(); }
    void release() { if (region) DeleteObject(region); region = nullptr; }
    HRGN ensure() {
        if (!region) region = CreateRectRgn(0, 0, 0, 0);
        return region;
    }
} g_damageRegion;

void drawIntelLogo(HDC dc, int x, int y) {
    if (!g_intelLogo) return;
    if (!g_logoDc) {
        HDC source = CreateCompatibleDC(dc);
        if (!source) return;
        HGDIOBJ previous = SelectObject(source, g_intelLogo);
        if (!previous || previous == HGDI_ERROR) { DeleteDC(source); return; }
        g_logoDc = source;
        g_logoPrevious = previous;
    }
    BitBlt(dc, x, y, 38, 28, g_logoDc, 0, 0, SRCCOPY);
}

void render(HDC dc, const RECT& rc, HRGN damage = nullptr) {
    const int saved = SaveDC(dc);
    if (!saved) return;
    // FillRect honours the exact DC clip, including disjoint border strips.
    // If selecting damage fails, repaint the whole buffer rather than retaining
    // stale pixels. A paint DC still carries BeginPaint's own clipping.
    if (damage) ExtSelectClipRgn(dc, damage, RGN_AND);
    FillRect(dc, &rc, g_backgroundBrush);

    ULONGLONG now = g_onBattery ? 0 : GetTickCount64();
    bool borderVisible = false;
    for (const auto& rect : borderRects(rc)) borderVisible = borderVisible || RectVisible(dc, &rect);
    if (borderVisible) {
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(DC_PEN));
        const COLORREF oldPenColor = GetDCPenColor(dc);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        const COLORREF color = animatedBorderColor(g_loadLevel, now);
        constexpr double intensity[] = {1.18, 0.46, 0.24, 0.11, 0.045, 0.015};
        for (int layer = 5; layer >= 0; --layer) {
            const auto blend = [&](int fg, int bg) {
                return channel(bg + (fg - bg) * intensity[layer]);
            };
            SetDCPenColor(dc, RGB(blend(GetRValue(color), 18),
                blend(GetGValue(color), 18), blend(GetBValue(color), 20)));
            const int inset = 1 + layer;
            RoundRect(dc, inset, inset, rc.right - inset, rc.bottom - inset,
                24 - 2 * layer, 24 - 2 * layer);
        }
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        SetDCPenColor(dc, oldPenColor);
    }

    SetBkMode(dc, TRANSPARENT);
    const RECT logo{10, 9, 48, 37};
    if (RectVisible(dc, &logo)) drawIntelLogo(dc, 10, 9);
    auto text = [&](const std::wstring& value, RECT cell, COLORREF color, UINT alignment = DT_LEFT) {
        if (value.empty() || !RectVisible(dc, &cell)) return;
        SetTextColor(dc, color);
        DrawTextW(dc, value.c_str(), -1, &cell, (LABEL_TEXT_FORMAT & ~DT_LEFT) | alignment);
    };
    SelectObject(dc, g_metricsFont);
    const RECT cpuCell = COMPACT_CELLS[0].label;
    if (g_cpuLoad >= 0 && RectVisible(dc, &cpuCell)) {
        const int glowState = SaveDC(dc);
        if (glowState) {
            // Confine the bloom to the label's existing damage cell so it cannot
            // spill into the CPU percentage or leave trails on partial repaints.
            IntersectClipRect(dc, cpuCell.left, cpuCell.top, cpuCell.right, cpuCell.bottom);
            if (g_cpuGlowLayout.font != g_metricsFont)
                g_cpuGlowLayout = layoutGlowLabel(dc, COMPACT_LABELS[0], cpuCell);
            const COLORREF color = cpuLabelColor(g_cpuLoad, now);
            for (int radius = 2; radius >= 1; --radius) {
                const double strength = radius == 2 ? 0.10 : 0.28;
                const auto blend = [&](int fg, int bg) {
                    return channel(bg + (fg - bg) * strength);
                };
                SetTextColor(dc, RGB(blend(GetRValue(color), 18),
                    blend(GetGValue(color), 18), blend(GetBValue(color), 20)));
                for (int y = -radius; y <= radius; ++y) {
                    for (int x = -radius; x <= radius; ++x) {
                        if (std::max(std::abs(x), std::abs(y)) != radius) continue;
                        drawGlowCopy(dc, g_cpuGlowLayout, cpuCell, x, y);
                    }
                }
            }
            RestoreDC(dc, glowState);
        }
    }
    for (size_t i = 0; i < COMPACT_CELLS.size(); ++i) {
        text(COMPACT_LABELS[i], COMPACT_CELLS[i].label, i == 0 ? cpuLabelColor(g_cpuLoad, now) : TEXT_COLOR);
        text(g_compactParts[i], COMPACT_CELLS[i].value, TEXT_COLOR);
    }
    if (g_expanded) {
        SelectObject(dc, g_smallFont);
        ensureExpandedLayout(dc);
        for (size_t i = 0; i < g_expandedCells.size(); ++i)
            text(EXPANDED_LABELS[i], g_expandedCells[i].label, EXPANDED_LABEL_COLOR);
        SelectObject(dc, g_metricsFont);
        for (size_t i = 0; i < g_expandedCells.size(); ++i)
            text(g_expandedParts[i], g_expandedCells[i].value, EXPANDED_VALUE_COLOR, DT_RIGHT);
    }
    RestoreDC(dc, saved);
}

void paint() {
    // Capture the actual (possibly disjoint) update region before BeginPaint
    // validates it. rcPaint alone would turn border damage into a full redraw.
    HRGN damage = g_damageRegion.ensure();
    const int regionType = damage ? GetUpdateRgn(g_hwnd, damage, FALSE) : ERROR;
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(g_hwnd, &ps);
    RECT rc{}; GetClientRect(g_hwnd, &rc);
    if (regionType != NULLREGION && !IsRectEmpty(&ps.rcPaint)) {
        HRGN clip = regionType == ERROR ? nullptr : damage;
        if (g_backBuffer.ensure(dc, rc.right, rc.bottom)) {
            render(g_backBuffer.dc, rc, g_backBuffer.valid ? clip : nullptr);
            g_backBuffer.valid = true;
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top,
                ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
                g_backBuffer.dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        } else render(dc, rc, clip); // Keep the UI usable if GDI allocation fails.
    }
    EndPaint(g_hwnd, &ps);
}

void showContextMenu(HWND hwnd, POINT p, UINT alignFlags = 0) {
    cancelHover(hwnd);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, MENU_TOGGLE_EXPANDED, L"&Expand / Collapse");
    AppendMenuW(m, MF_STRING, MENU_RESET_POSITION, L"&Reset to right corner");
    std::wstring hideLabel = L"&Hide Island\t";
    if (g_hotkeyChoice >= 0 && g_hotkeyChoice < static_cast<int>(ARRAYSIZE(HOTKEYS))) {
        hideLabel += HOTKEYS[g_hotkeyChoice].label;
    } else {
        hideLabel += L"N/A";
    }
    AppendMenuW(m, MF_STRING, MENU_HIDE, hideLabel.c_str());
    AppendMenuW(m, MF_STRING | (g_autoHideOnHover ? MF_CHECKED : 0), MENU_AUTO_HIDE, L"&Auto-hide on hover");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    HMENU shortcuts = CreatePopupMenu();
    for (int i = 0; i < static_cast<int>(ARRAYSIZE(HOTKEYS)); ++i) {
        UINT flags = MF_STRING | (i == g_hotkeyChoice ? MF_CHECKED : 0);
        AppendMenuW(shortcuts, flags, MENU_HOTKEY_FIRST + i, HOTKEYS[i].label);
    }
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(shortcuts), L"Hide / show &shortcut");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(m, MF_STRING, MENU_ABOUT, L"A&bout X1 SYS Island");
    AppendMenuW(m, MF_STRING, MENU_EXIT, L"E&xit");

    SetForegroundWindow(hwnd);
    SendMessageW(hwnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEACCEL), 0);
    g_contextOpen = true;
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | alignFlags, p.x, p.y, 0, hwnd, nullptr);
    g_contextOpen = false;
    DestroyMenu(m);

    switch (cmd) {
    case MENU_TOGGLE_EXPANDED: g_expanded = !g_expanded; setWindowSize(); break;
    case MENU_RESET_POSITION: positionRight(hwnd); break;
    case MENU_HIDE: toggleIsland(hwnd); break;
    case MENU_AUTO_HIDE:
        g_autoHideOnHover = !g_autoHideOnHover;
        saveAutoHideOnHover(g_autoHideOnHover);
        cancelHover(hwnd);
        break;
    case MENU_ABOUT: showAbout(); break;
    case MENU_EXIT: DestroyWindow(hwnd); break;
    default:
        if (cmd >= MENU_HOTKEY_FIRST && cmd < MENU_HOTKEY_FIRST + static_cast<int>(ARRAYSIZE(HOTKEYS)))
            selectHotkey(hwnd, cmd - MENU_HOTKEY_FIRST);
    }
}

// Display off or session locked: nobody can see the island, so release
// telemetry and stop animating, as during suspension.
void setPresentationPaused(HWND hwnd, bool& reason, bool paused) {
    if (reason == paused) return;
    reason = paused;
    if (presentationPaused()) {
        publishSamplingPolicy(IsWindowVisible(hwnd) != FALSE);
        stopAnimation(hwnd);
    } else {
        updateSamplingPolicy(hwnd);
        startAnimation(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}
// Input reaching the island proves the display is on and the session unlocked.
// Recovers if Windows ever drops the matching resume notification.
void confirmPresented(HWND hwnd) {
    if (!g_displayOff && !g_sessionLocked) return;
    g_displayOff = g_sessionLocked = false;
    if (presentationPaused()) return;
    updateSamplingPolicy(hwnd);
    startAnimation(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
}
HPOWERNOTIFY g_displayNotification;
bool g_sessionNotificationRegistered = false;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SHOWWINDOW:
        publishSamplingPolicy(wp != 0);
        if (!wp) stopAnimation(hwnd);
        break;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMSUSPEND) {
            g_suspended = true;
            publishSamplingPolicy(false);
            stopAnimation(hwnd);
        } else if (wp == PBT_POWERSETTINGCHANGE) {
            const auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lp);
            if (setting && IsEqualGUID(setting->PowerSetting, GUID_SESSION_DISPLAY_STATUS) &&
                setting->DataLength >= sizeof(DWORD)) {
                DWORD state = 0;
                memcpy(&state, setting->Data, sizeof(state));
                // 0 = off, 1 = on, 2 = dimmed (still visible).
                setPresentationPaused(hwnd, g_displayOff, state == 0);
            }
        } else if (wp == PBT_APMPOWERSTATUSCHANGE || wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
            g_suspended = false;
            updateSamplingPolicy(hwnd);
            startAnimation(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return TRUE;
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK) setPresentationPaused(hwnd, g_sessionLocked, true);
        else if (wp == WTS_SESSION_UNLOCK) setPresentationPaused(hwnd, g_sessionLocked, false);
        return 0;
    case WM_STATS_READY:
        if (IsWindowVisible(hwnd)) {
            refreshDisplayCache();
        }
        return 0;
    case WM_SHOW_EXISTING_ISLAND:
        confirmPresented(hwnd);
        showIsland(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_CREATE:
        // The worker delivers display updates; no independent polling timer.
        // A logon task can begin before the desktop or telemetry provider is
        // fully ready. Reconcile actual visibility to recover a missed initial
        // show notification, without waking a worker whose policy is unchanged.
        SetCoalescableTimer(hwnd, TELEMETRY_HEALTH_TIMER_ID, TELEMETRY_HEALTH_INTERVAL_MS, nullptr, 5000);
        // Both deliver the current state immediately after registration.
        g_displayNotification = RegisterPowerSettingNotification(hwnd, &GUID_SESSION_DISPLAY_STATUS,
            DEVICE_NOTIFY_WINDOW_HANDLE);
        g_sessionNotificationRegistered = WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION) != FALSE;
        g_hotkeyChoice = loadHotkeyChoice();
        g_autoHideOnHover = loadAutoHideOnHover();
        if (!registerToggleHotkey(hwnd, g_hotkeyChoice))
            MessageBoxW(hwnd, L"Hide/show shortcut is already in use. Choose another from the right-click menu. Launch this app again to restore it if hidden.", L"X1 SYS Island", MB_OK | MB_ICONWARNING);
        g_contextHotkeyRegistered = RegisterHotKey(hwnd, CONTEXT_MENU_HOTKEY_ID, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'D') != FALSE;
        startAnimation(hwnd);
        return 0;

    case WM_TIMER:
        if(wp==ANIMATION_TIMER_ID) {
              // A layered, non-activating HWND can miss a leave notification
              // after being shown under a stationary cursor. Reconcile on the
              // existing animation tick so the next real entry always rearms.
              if(g_autoHideOnHover && IsWindowVisible(hwnd) && !g_userHidden && !g_hoverHidden && !g_dragging && !g_contextOpen) {
                  if(!cursorInside(hwnd)) cancelHover(hwnd);
                  else if(!g_trackingMouse && !g_hoverConsumed) {
                      g_trackingMouse=true;
                      SetTimer(hwnd,HOVER_TIMER_ID,1000,nullptr);
                  }
              }
              if (!g_onBattery) invalidateAnimation(hwnd);
        } else if (wp == TELEMETRY_HEALTH_TIMER_ID) {
              updateSamplingPolicy(hwnd);
        } else if(wp==HOVER_TIMER_ID) {
            KillTimer(hwnd,HOVER_TIMER_ID);
            if(g_autoHideOnHover && !g_contextOpen && !g_dragging && !g_userHidden && cursorInside(hwnd)) {
                g_hoverConsumed=true;
                g_hoverHidden=true;
                g_trackingMouse=false;
                stopAnimation(hwnd);
                ShowWindow(hwnd,SW_HIDE);
                SetTimer(hwnd,RESHOW_TIMER_ID,5000,nullptr);
            }
        } else if(wp==RESHOW_TIMER_ID) {
            KillTimer(hwnd,RESHOW_TIMER_ID);
            if(g_autoHideOnHover && g_hoverHidden && !g_userHidden) {
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
        confirmPresented(hwnd);
        if (wp == HOTKEY_ID) {
            toggleIsland(hwnd);
        } else if (wp == CONTEXT_MENU_HOTKEY_ID) {
            if (!g_contextOpen) {
                if (g_userHidden || g_hoverHidden || !IsWindowVisible(hwnd)) {
                    showIsland(hwnd);
                }
                RECT r{};
                GetWindowRect(hwnd, &r);
                POINT center{ r.left + (r.right - r.left) / 2, r.top + (r.bottom - r.top) / 2 };
                showContextMenu(hwnd, center, TPM_CENTERALIGN);
            }
        }
        return 0;

    case WM_LBUTTONDBLCLK:
        g_expanded = !g_expanded; setWindowSize(); return 0;

    case WM_LBUTTONDOWN:
        cancelHover(hwnd);
        g_dragging = true; SetCapture(hwnd);
        g_dragStart = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        return 0;

    case WM_MOUSEMOVE:
        confirmPresented(hwnd);
        if(g_dragging && (wp & MK_LBUTTON)) {
            POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}; ClientToScreen(hwnd,&p);
            // A visible, dragged island is never hidden; move and keep it topmost in one call.
            SetWindowPos(hwnd,HWND_TOPMOST,p.x-g_dragStart.x,p.y-g_dragStart.y,0,0,SWP_NOSIZE|SWP_NOACTIVATE);
        } else if(g_autoHideOnHover && !g_trackingMouse && !g_contextOpen) {
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
        g_dragging=false; ReleaseCapture(); ensureTopmostVisible(hwnd); return 0;
    case WM_RBUTTONDOWN:
        cancelHover(hwnd);
        return 0;
    case WM_RBUTTONUP: {
        POINT p{}; GetCursorPos(&p);
        showContextMenu(hwnd, p);
        return 0;
    }


    case WM_UPDATEUISTATE:
        // Mouse input can hide accelerators again after the menu-opening request.
        // Keep menu mnemonics visible without changing focus-cue preferences.
        DefWindowProcW(hwnd, msg, wp, lp);
        return DefWindowProcW(hwnd, WM_UPDATEUISTATE,
            MAKEWPARAM(UIS_CLEAR, UISF_HIDEACCEL), 0);
    case WM_INITMENUPOPUP:
        SendMessageW(hwnd, WM_UPDATEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEACCEL), 0);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
    case WM_DISPLAYCHANGE:
        g_backBuffer.release();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_PAINT: paint(); return 0;

    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION:
        if (wp) DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_stopEvent) SetEvent(g_stopEvent);
        if (g_displayNotification) UnregisterPowerSettingNotification(g_displayNotification);
        g_displayNotification = nullptr;
        if (g_sessionNotificationRegistered) WTSUnRegisterSessionNotification(hwnd);
        g_sessionNotificationRegistered = false;
        KillTimer(hwnd, HOVER_TIMER_ID);
        KillTimer(hwnd, RESHOW_TIMER_ID);
        KillTimer(hwnd, ANIMATION_TIMER_ID);
        KillTimer(hwnd, TELEMETRY_HEALTH_TIMER_ID);
        if (g_hotkeyRegistered) UnregisterHotKey(hwnd, HOTKEY_ID);
        if (g_contextHotkeyRegistered) UnregisterHotKey(hwnd, CONTEXT_MENU_HOTKEY_ID);
        g_backBuffer.release();
        g_damageRegion.release();
        releaseLogoDc();
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

template<class Source>
void runTelemetryWorker() {
    std::unique_ptr<Source> telemetry;
    HANDLE events[] = {g_stopEvent, g_policyEvent};
    WorkerSchedule schedule;
    DWORD delay = 0;
    for (;;) {
        DWORD wake = WaitForMultipleObjects(2, events, FALSE, delay);
        if (wake == WAIT_OBJECT_0 || wake == WAIT_FAILED) break;
        const auto policy = samplingPolicy();
        auto now = GetTickCount64();
        if (schedule.mustRelease(policy, now)) telemetry.reset();
        schedule.releaseEpoch = policy.releaseEpoch;
        if (policy.mode != SamplingMode::Active) {
            schedule.retained = telemetry != nullptr;
            delay = telemetry ? schedule.delayUntil(policy.hoverDeadline, now) : INFINITE;
            continue;
        }
        if (!telemetry || schedule.resumeEpoch != policy.resumeEpoch) {
            if (!telemetry) telemetry = std::make_unique<Source>();
            else telemetry->reprime();
            schedule.nextSample = GetTickCount64() + policy.interval;
            schedule.resumeEpoch = policy.resumeEpoch;
        }
        schedule.retained = false;
        now = GetTickCount64();
        if (now >= schedule.nextSample) {
            // Do not start a sample after a hide/suspend published
            // while initialization was in progress.
            const auto latest = samplingPolicy();
            if (latest.mode != SamplingMode::Active || latest.resumeEpoch != policy.resumeEpoch) {
                delay = 0;
                continue;
            }
            Stats snapshot = telemetry->sample();
            if (g_sampling.load()) {
                std::lock_guard<std::mutex> guard(g_statsMutex);
                g_snapshot = std::move(snapshot);
                PostMessageW(g_hwnd, WM_STATS_READY, 0, 0);
            }
            schedule.nextSample = GetTickCount64() + policy.interval;
        }
        delay = schedule.delayUntil(schedule.nextSample, GetTickCount64());
    }
}

int WINAPI wWinMain(HINSTANCE h, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_singleInstanceMutex = CreateMutexW(nullptr, FALSE, SINGLE_INSTANCE_MUTEX);
    const DWORD mutexError = GetLastError();
    // Fail closed: inability to acquire the gate must never launch another worker.
    // ACCESS_DENIED commonly means an instance at a different privilege level.
    if (!g_singleInstanceMutex && mutexError != ERROR_ACCESS_DENIED) {
        MessageBoxW(nullptr, L"Cannot establish the single-instance lock. SYS will not start.",
            L"X1 SYS Island", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (!g_singleInstanceMutex || mutexError == ERROR_ALREADY_EXISTS) {
        if (g_singleInstanceMutex) CloseHandle(g_singleInstanceMutex);
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
        initialX, 36, initialWidth, 46, nullptr, nullptr, h, nullptr);
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
            // EcoQoS: background sampling may run on efficient cores at lower
            // clocks. Best effort; unsupported Windows versions ignore it.
            THREAD_POWER_THROTTLING_STATE throttling{THREAD_POWER_THROTTLING_CURRENT_VERSION,
                THREAD_POWER_THROTTLING_EXECUTION_SPEED, THREAD_POWER_THROTTLING_EXECUTION_SPEED};
            SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &throttling, sizeof(throttling));
            try {
                runTelemetryWorker<Telemetry>();
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
    positionRight(g_hwnd);
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    ensureTopmostVisible(g_hwnd);
    startAnimation(g_hwnd);
    UpdateWindow(g_hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    SetEvent(g_stopEvent);
    g_worker.join();
    CloseHandle(g_stopEvent);
    CloseHandle(g_policyEvent);
    DeleteObject(g_metricsFont); DeleteObject(g_smallFont);
    releaseLogoDc();
    if (g_intelLogo) DeleteObject(g_intelLogo);
    if (g_backgroundBrush) DeleteObject(g_backgroundBrush);
    if (g_singleInstanceMutex) CloseHandle(g_singleInstanceMutex);
    return 0;
}




