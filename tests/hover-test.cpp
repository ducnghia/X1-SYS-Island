#define NOMINMAX
#include <windows.h>
#include <cassert>
#include <cstdio>
extern HWND g_hwnd;
bool testInside = false;
BOOL TestGetCursorPos(POINT* point) {
    RECT rect{}; GetWindowRect(g_hwnd, &rect);
    *point = testInside ? POINT{rect.left+60, rect.top+20} : POINT{rect.right+40, rect.bottom+40};
    return TRUE;
}
BOOL TestTrackMouseEvent(TRACKMOUSEEVENT*) { return TRUE; }
// Only the cursor input is simulated. Exercise the production WndProc, real
// HWND visibility, Windows timers and message loop, without moving user's mouse.
#define GetCursorPos TestGetCursorPos
#define TrackMouseEvent TestTrackMouseEvent
#include "../x1_sys_island.cpp"
#undef GetCursorPos
#undef TrackMouseEvent

LRESULT CALLBACK TestProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CREATE) return 0; // No real hotkey registration/telemetry worker.
    return WndProc(hwnd, msg, wp, lp);
}
void pump(DWORD milliseconds) {
    ULONGLONG deadline = GetTickCount64() + milliseconds;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        Sleep(5);
    } while (GetTickCount64() < deadline);
}
void enter() {
    testInside = true;
    SendMessageW(g_hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(60,20));
}
void leave() {
    testInside = false;
    SendMessageW(g_hwnd, WM_MOUSELEAVE, 0, 0);
}
void savePreview(bool expanded, const wchar_t* path) {
    g_expanded=expanded;
    const int height=expanded ? EXPANDED_HEIGHT : COMPACT_HEIGHT;
    BITMAPINFO info{};
    info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=ISLAND_WIDTH; info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32;
    void* pixels=nullptr;
    HDC dc=CreateCompatibleDC(nullptr);
    HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    auto previous=SelectObject(dc,bitmap);
    RECT rect{0,0,ISLAND_WIDTH,height};
    render(dc,rect);
    BITMAPFILEHEADER header{};
    header.bfType=0x4d42;
    header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);
    header.bfSize=header.bfOffBits+ISLAND_WIDTH*height*4;
    FILE* file=nullptr; _wfopen_s(&file,path,L"wb"); assert(file);
    fwrite(&header,sizeof(header),1,file);
    fwrite(&info.bmiHeader,sizeof(BITMAPINFOHEADER),1,file);
    fwrite(pixels,ISLAND_WIDTH*height*4,1,file);
    fclose(file);
    SelectObject(dc,previous); DeleteObject(bitmap); DeleteDC(dc);
}
int main() {
    g_metricsFont = CreateFontW(-14,0,0,0,600,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,0,0,L"Segoe UI");
    g_smallFont = CreateFontW(-14,0,0,0,400,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,0,0,L"Segoe UI");
    g_intelLogo=LoadBitmapW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDB_INTEL_LOGO));
    g_backgroundBrush = CreateSolidBrush(RGB(18,18,20));
    WNDCLASSW cls{}; cls.lpfnWndProc=TestProc; cls.hInstance=GetModuleHandleW(nullptr); cls.lpszClassName=L"X1SYSHoverTest";
    assert(RegisterClassW(&cls));
    g_hwnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,cls.lpszClassName,L"Hover test",WS_POPUP,
        -30000,-30000,ISLAND_WIDTH,COMPACT_HEIGHT,nullptr,nullptr,cls.hInstance,nullptr);
    assert(g_hwnd);
    g_snapshot.cpu = 100; g_snapshot.gpu = 100; g_snapshot.temperature = 125;
    g_snapshot.used = g_snapshot.total = 64ULL * 1024 * 1024 * 1024;
    g_snapshot.fans = {8191,8191,2,true};
    refreshDisplayCache();
    HDC dc=GetDC(g_hwnd);
    auto oldFont=SelectObject(dc,g_metricsFont);
    int width=0;
    for(const auto& part:g_compactParts) {
        SIZE size{}; GetTextExtentPoint32W(dc,part.c_str(),static_cast<int>(part.size()),&size);
        width+=size.cx;
    }
    assert(width+3*8 <= ISLAND_WIDTH-58-12);
    for(const auto& part:g_expandedParts) {
        SIZE size{}; GetTextExtentPoint32W(dc,part.c_str(),static_cast<int>(part.size()),&size);
        assert(size.cx<=ISLAND_WIDTH/2-18);
    }
    SelectObject(dc,oldFont); ReleaseDC(g_hwnd,dc);
    assert(g_loadLevel==LoadLevel::Red);
    assert(levelForPercent(49)==LoadLevel::Normal);
    assert(levelForPercent(50)==LoadLevel::Yellow);
    assert(levelForPercent(80)==LoadLevel::Red);
    puts("PASS: 560px layout, six cells and border thresholds.");
    g_snapshot.cpu=17; g_snapshot.gpu=32; g_snapshot.temperature=54;
    g_snapshot.used=32ULL*1024*1024*1024;
    g_snapshot.fans={6172,5382,2,true};
    refreshDisplayCache();
    savePreview(false,L"build/preview-compact.bmp");
    savePreview(true,L"build/preview-expanded.bmp");
    g_expanded=false;
    showIsland(g_hwnd);
    enter();
    pump(700); assert(IsWindowVisible(g_hwnd));
    pump(500); assert(!IsWindowVisible(g_hwnd) && g_hoverHidden);
    // The OS-generated leave caused by hiding must not erase consumed hover.
    SendMessageW(g_hwnd, WM_MOUSELEAVE, 0, 0);
    assert(g_hoverConsumed);
    pump(4500); assert(!IsWindowVisible(g_hwnd));
    pump(700); assert(IsWindowVisible(g_hwnd));
    pump(1200); assert(IsWindowVisible(g_hwnd));
    puts("PASS: 1s hover, 5s hidden, visible again without repeated hide.");
    // Missing WM_MOUSELEAVE after re-show: animation reconciliation rearms.
    testInside=false; pump(250);
    assert(!g_hoverConsumed && !g_trackingMouse);
    enter(); pump(1200); assert(!IsWindowVisible(g_hwnd));
    toggleIsland(g_hwnd); toggleIsland(g_hwnd);
    leave(); pump(5300); assert(!IsWindowVisible(g_hwnd) && g_userHidden);
    toggleIsland(g_hwnd); assert(IsWindowVisible(g_hwnd));
    puts("PASS: re-entry after show and manual hide cancels automatic return.");
    enter(); pump(200);
    SendMessageW(g_hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(60,20));
    pump(1200); assert(IsWindowVisible(g_hwnd));
    SendMessageW(g_hwnd,WM_LBUTTONUP,0,MAKELPARAM(60,20));
    leave(); enter(); pump(200);
    SendMessageW(g_hwnd,WM_RBUTTONDOWN,0,0);
    g_contextOpen=true;
    pump(1200); assert(IsWindowVisible(g_hwnd));
    g_contextOpen=false; leave();
    puts("PASS: drag and context menu cancel hover.");
    assert(g_sampling.load());
    toggleIsland(g_hwnd);
    assert(!g_sampling.load());
    toggleIsland(g_hwnd);
    assert(g_sampling.load());
    SendMessageW(g_hwnd, WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
    assert(!g_sampling.load());
    SendMessageW(g_hwnd, WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
    assert(g_sampling.load());
    assert(g_sampleInterval.load() == (g_onBattery ? 3000u : 1000u));
    g_onBattery = true;
    startAnimation(g_hwnd);
    leave(); enter(); pump(1200);
    assert(!IsWindowVisible(g_hwnd) && !g_sampling.load());
    toggleIsland(g_hwnd);
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    assert(g_stopEvent);
    DestroyWindow(g_hwnd);
    assert(WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0);
    CloseHandle(g_stopEvent); g_stopEvent = nullptr;
    puts("PASS: hidden/suspend sampling policy, battery hover and exit stop signal.");
    DeleteObject(g_metricsFont); DeleteObject(g_smallFont); DeleteObject(g_intelLogo); DeleteObject(g_backgroundBrush);
    return 0;
}

