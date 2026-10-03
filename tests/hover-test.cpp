#define NOMINMAX
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <algorithm>
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
int extentCalls = 0;
BOOL TestGetTextExtentPoint32W(HDC dc, LPCWSTR text, int count, LPSIZE size) {
    ++extentCalls;
    return GetTextExtentPoint32W(dc, text, count, size);
}
#define GetTextExtentPoint32W TestGetTextExtentPoint32W
int textCalls = 0, logoCalls = 0, fillCalls = 0;
LONG largestFill = 0;
int TestDrawTextW(HDC dc, LPCWSTR text, int count, LPRECT rect, UINT flags) {
    ++textCalls;
    return DrawTextW(dc, text, count, rect, flags);
}
int TestFillRect(HDC dc, const RECT* rect, HBRUSH brush) {
    ++fillCalls;
    largestFill = (std::max)(largestFill, (rect->right-rect->left)*(rect->bottom-rect->top));
    return FillRect(dc, rect, brush);
}
BOOL TestBitBlt(HDC dc, int x, int y, int w, int h, HDC source, int sx, int sy, DWORD op) {
    if (w == 38 && h == 28) ++logoCalls;
    return BitBlt(dc,x,y,w,h,source,sx,sy,op);
}
#define DrawTextW TestDrawTextW
#define FillRect TestFillRect
#define BitBlt TestBitBlt
#include "../x1_sys_island.cpp"
#undef GetCursorPos
#undef TrackMouseEvent
#undef GetTextExtentPoint32W
#undef DrawTextW
#undef FillRect
#undef BitBlt

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
struct FakeTelemetry {
    inline static std::atomic<int> opened{0}, closed{0}, samples{0}, primes{0};
    FakeTelemetry() { ++opened; }
    ~FakeTelemetry() { ++closed; }
    void reprime() { ++primes; }
    Stats sample() { ++samples; return {}; }
};
void awaitCount(const std::atomic<int>& counter, int expected) {
    const auto deadline = GetTickCount64() + 2000;
    while (counter.load() < expected && GetTickCount64() < deadline) Sleep(5);
    assert(counter.load() >= expected);
}
void testWorkerLifecycle() {
    const auto saved = samplingPolicy();
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto policy = saved;
    policy.mode = SamplingMode::Active;
    policy.interval = 100;
    ++policy.resumeEpoch;
    auto publish = [&] {
        std::lock_guard<std::mutex> guard(g_policyMutex);
        g_policy = policy;
        g_sampling.store(policy.mode == SamplingMode::Active);
        SetEvent(g_policyEvent);
    };
    publish();
    std::thread worker(runTelemetryWorker<FakeTelemetry>);
    awaitCount(FakeTelemetry::samples, 1);
    policy.mode = SamplingMode::Hover;
    policy.hoverDeadline = GetTickCount64() + HOVER_RETENTION_MS;
    publish();
    Sleep(50); // Allow an already-running sample to finish.
    const int hiddenSamples = FakeTelemetry::samples.load();
    Sleep(200);
    assert(FakeTelemetry::samples == hiddenSamples && FakeTelemetry::closed == 0);
    policy.mode = SamplingMode::Active;
    ++policy.resumeEpoch;
    publish();
    awaitCount(FakeTelemetry::primes, 1);
    assert(FakeTelemetry::samples == hiddenSamples); // Wait a fresh rate interval.
    awaitCount(FakeTelemetry::samples, hiddenSamples + 1);
    assert(FakeTelemetry::opened == 1);
    // Unchanged policy events must not push the next sample into the future.
    const int beforeWakeStorm = FakeTelemetry::samples.load();
    for (int i = 0; i < 40; ++i) { SetEvent(g_policyEvent); Sleep(10); }
    assert(FakeTelemetry::samples >= beforeWakeStorm + 2);
    policy.mode = SamplingMode::Hover;
    policy.hoverDeadline = GetTickCount64() + 100;
    publish();
    awaitCount(FakeTelemetry::closed, 1);
    const int expiredSamples = FakeTelemetry::samples.load();
    Sleep(150);
    assert(FakeTelemetry::samples == expiredSamples);
    policy.mode = SamplingMode::Active;
    ++policy.resumeEpoch;
    publish();
    awaitCount(FakeTelemetry::opened, 2);
    policy.mode = SamplingMode::Hover;
    policy.hoverDeadline = GetTickCount64() + HOVER_RETENTION_MS;
    publish();
    policy.mode = SamplingMode::Released;
    ++policy.releaseEpoch;
    publish();
    awaitCount(FakeTelemetry::closed, 2);
    policy.mode = SamplingMode::Active;
    ++policy.resumeEpoch;
    publish();
    awaitCount(FakeTelemetry::opened, 3);
    policy.mode = SamplingMode::Hover;
    policy.hoverDeadline = GetTickCount64() + HOVER_RETENTION_MS;
    publish();
    SetEvent(g_stopEvent);
    worker.join();
    assert(FakeTelemetry::closed == 3);
    CloseHandle(g_stopEvent); g_stopEvent = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_policyMutex);
        g_policy = saved;
        g_sampling.store(saved.mode == SamplingMode::Active);
    }
    puts("PASS: worker no hidden samples, retained resume priming, expiry, release, cadence and hidden exit.");
}
void testRendering() {
    SetWindowPos(g_hwnd, HWND_TOPMOST, 20, 20, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    stopAnimation(g_hwnd);
    g_onBattery = false;
    g_snapshot.cpu = 60;
    refreshDisplayCache();
    InvalidateRect(g_hwnd, nullptr, FALSE);
    UpdateWindow(g_hwnd);
    assert(g_backBuffer.valid && g_backBuffer.width == ISLAND_WIDTH);
    setWindowSize(); UpdateWindow(g_hwnd);
    invalidateAnimation(g_hwnd); UpdateWindow(g_hwnd);
    const auto dc = g_backBuffer.dc;
    const auto bitmap = g_backBuffer.bitmap;
    GdiFlush();
    const DWORD handles = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    HRGN damage = CreateRectRgn(0,0,0,0);
    for (int i = 0; i < 100; ++i) {
        textCalls = logoCalls = fillCalls = 0; largestFill = 0;
        SendMessageW(g_hwnd, WM_TIMER, ANIMATION_TIMER_ID, 0);
        assert(GetUpdateRgn(g_hwnd, damage, FALSE) == COMPLEXREGION);
        assert(PtInRegion(damage, 1, 20));
        assert(PtInRegion(damage, 60, 20));
        assert(!PtInRegion(damage, 120, 20));
        assert(!PtInRegion(damage, 20, 20));
        UpdateWindow(g_hwnd);
        assert(textCalls == 1 && logoCalls == 0 && fillCalls > 0);
        assert(largestFill < ISLAND_WIDTH*COMPACT_HEIGHT);
        assert(g_backBuffer.dc == dc && g_backBuffer.bitmap == bitmap);
        assert(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == handles + 1);
    }
    refreshDisplayCache();
    assert(GetUpdateRgn(g_hwnd, damage, FALSE) == NULLREGION);
    g_snapshot.gpu = 9;
    refreshDisplayCache();
    HRGN expected = CreateRectRgnIndirect(&COMPACT_CELLS[3].value);
    GetUpdateRgn(g_hwnd, damage, FALSE);
    assert(EqualRgn(damage, expected));
    textCalls = logoCalls = 0;
    UpdateWindow(g_hwnd);
    assert(textCalls == 1 && logoCalls == 0);
    assert(DeleteObject(expected));
    assert(DeleteObject(damage));
    GdiFlush();
    // Warm both font faces and the expanded window's system-owned region.
    g_expanded = true; setWindowSize(); UpdateWindow(g_hwnd);
    g_expanded = false; setWindowSize(); UpdateWindow(g_hwnd);
    GdiFlush();
    const DWORD resizeHandles = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int i = 0; i < 100; ++i) {
        g_snapshot.gpu = i % 2 ? 9 : 100;
        refreshDisplayCache();
        textCalls = logoCalls = 0;
        UpdateWindow(g_hwnd);
        assert(textCalls == 1 && logoCalls == 0);
        GdiFlush();
        assert(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == resizeHandles);
    }
    g_onBattery = true;
    ValidateRect(g_hwnd, nullptr);
    SendMessageW(g_hwnd, WM_TIMER, ANIMATION_TIMER_ID, 0);
    assert(!GetUpdateRect(g_hwnd, nullptr, FALSE));
    g_onBattery = false;
    for (int i = 0; i < 20; ++i) {
        g_expanded = !g_expanded;
        setWindowSize();
        UpdateWindow(g_hwnd);
        assert(g_backBuffer.valid);
        assert(g_backBuffer.height == (g_expanded ? EXPANDED_HEIGHT : COMPACT_HEIGHT));
        GdiFlush();
        assert(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == resizeHandles);
    }
    g_expanded = true;
    setWindowSize(); UpdateWindow(g_hwnd);
    textCalls = logoCalls = 0;
    invalidateAnimation(g_hwnd); UpdateWindow(g_hwnd);
    assert(textCalls == 1 && logoCalls == 0);
    // Compare a repaired value cell with a fresh complete image, with time frozen.
    g_onBattery = true;
    InvalidateRect(g_hwnd, nullptr, FALSE); UpdateWindow(g_hwnd);
    g_snapshot.gpu = -1; refreshDisplayCache(); UpdateWindow(g_hwnd);
    g_snapshot.fans.fan2 = 900;
    refreshDisplayCache();
    damage = CreateRectRgn(0,0,0,0);
    expected = CreateRectRgnIndirect(&EXPANDED_CELLS[6].value);
    GetUpdateRgn(g_hwnd, damage, FALSE);
    assert(EqualRgn(damage, expected));
    DeleteObject(damage); DeleteObject(expected);
    textCalls = 0;
    UpdateWindow(g_hwnd);
    assert(textCalls == 1);
    // Force a border/CPU colour transition while keeping all numeric cells intact.
    g_loadLevel = LoadLevel::Red;
    g_cpuLoad = 90;
    invalidateAnimation(g_hwnd); UpdateWindow(g_hwnd);
    HDC target = GetDC(g_hwnd);
    BackBuffer reference;
    assert(reference.ensure(target, ISLAND_WIDTH, EXPANDED_HEIGHT));
    RECT rc{0,0,ISLAND_WIDTH,EXPANDED_HEIGHT};
    render(reference.dc, rc);
    for (int y = 0; y < EXPANDED_HEIGHT; ++y)
        for (int x = 0; x < ISLAND_WIDTH; ++x)
            assert(GetPixel(reference.dc,x,y) == GetPixel(g_backBuffer.dc,x,y));
    textCalls = logoCalls = 0;
    InvalidateRect(g_hwnd, nullptr, TRUE); UpdateWindow(g_hwnd);
    assert(textCalls == 22 && logoCalls == 1); // Complete exposure restores all cells.
    ReleaseDC(g_hwnd,target);
    reference.release();
    g_expanded = false; setWindowSize(); UpdateWindow(g_hwnd);
    GdiFlush();
    assert(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == resizeHandles);
    g_onBattery = false;
    SetWindowPos(g_hwnd, nullptr, -30000, -30000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    puts("PASS: exact damage, animation-only draws, pixel-equivalent repair, exposure, resize and stable backbuffer handles.");
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
    g_snapshot.tjMax = 125; g_snapshot.distanceToTjMax = 0;
    g_snapshot.used = g_snapshot.total = 64ULL * 1024 * 1024 * 1024;
    g_snapshot.fans = {8191,8191,2,true};
    refreshDisplayCache();
    HDC dc=GetDC(g_hwnd);
    auto checkCells = [&](const auto& cells, const auto& values, HFONT font) {
        auto previous = SelectObject(dc, font);
        for (size_t i = 0; i < cells.size(); ++i) {
            const auto& cell = cells[i];
            assert(cell.label.right <= cell.value.left && cell.value.right <= ISLAND_WIDTH-12);
            SIZE size{}; GetTextExtentPoint32W(dc, values[i].c_str(), static_cast<int>(values[i].size()), &size);
            if (size.cx > cell.value.right-cell.value.left)
                printf("Cell %zu needs %ld px, has %ld px\n", i, size.cx, cell.value.right-cell.value.left);
            assert(size.cx <= cell.value.right-cell.value.left);
        }
        SelectObject(dc, previous);
    };
    checkCells(COMPACT_CELLS, g_compactParts, g_metricsFont);
    checkCells(EXPANDED_CELLS, g_expandedParts, g_smallFont);
    ReleaseDC(g_hwnd,dc);
    assert(g_loadLevel==LoadLevel::Red);
    assert(levelForPercent(49)==LoadLevel::Normal);
    assert(levelForPercent(50)==LoadLevel::Yellow);
    assert(levelForPercent(80)==LoadLevel::Red);
    puts("PASS: 560px layout, separate label/value and fan cells, and border thresholds.");
    g_snapshot.cpu=17; g_snapshot.gpu=32; g_snapshot.temperature=54;
    g_snapshot.tjMax=100; g_snapshot.distanceToTjMax=46;
    g_snapshot.used=32ULL*1024*1024*1024;
    g_snapshot.fans={6172,5382,2,true};
    refreshDisplayCache();
    savePreview(false,L"build/preview-compact.bmp");
    savePreview(true,L"build/preview-expanded.bmp");
    g_expanded=false;
    HDC cacheDc = GetDC(g_hwnd);
    RECT compactRect{0,0,ISLAND_WIDTH,COMPACT_HEIGHT};
    const auto originalFont = GetCurrentObject(cacheDc, OBJ_FONT);
    const auto originalPen = GetCurrentObject(cacheDc, OBJ_PEN);
    render(cacheDc, compactRect);
    const int measured = extentCalls;
    const DWORD handles = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const HDC logoDc = g_logoDc;
    for (int i = 0; i < 100; ++i) render(cacheDc, compactRect);
    assert(extentCalls == measured && g_logoDc == logoDc && logoDc);
    assert(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == handles);
    assert(GetCurrentObject(cacheDc, OBJ_FONT) == originalFont);
    assert(GetCurrentObject(cacheDc, OBJ_PEN) == originalPen);
    const auto compactCells = COMPACT_CELLS;
    const auto expandedCells = EXPANDED_CELLS;
    for (double value : {-1.0, 0.0, 9.0, 10.0, 99.0, 100.0}) {
        g_snapshot.cpu = g_snapshot.gpu = value;
        refreshDisplayCache();
        render(cacheDc, compactRect);
        for (size_t i = 0; i < compactCells.size(); ++i) {
            assert(EqualRect(&compactCells[i].label, &COMPACT_CELLS[i].label));
            assert(EqualRect(&compactCells[i].value, &COMPACT_CELLS[i].value));
        }
        for (size_t i = 0; i < expandedCells.size(); ++i)
            assert(EqualRect(&expandedCells[i].value, &EXPANDED_CELLS[i].value));
    }
    assert(extentCalls == measured);
    ReleaseDC(g_hwnd, cacheDc);
    refreshDisplayCache();
    puts("PASS: fixed label/value cells across N/A and digit changes, retained logo and stable GDI handles.");
    testRendering();
    WorkerSchedule schedule;
    SamplingPolicy policy;
    policy.mode = SamplingMode::Hover;
    policy.hoverDeadline = 6500;
    schedule.retained = true;
    assert(!schedule.mustRelease(policy, 5000));
    assert(schedule.mustRelease(policy, 6500));
    assert(schedule.delayUntil(policy.hoverDeadline, 5000) == 1500);
    policy.mode = SamplingMode::Released;
    assert(schedule.mustRelease(policy, 1));
    policy.mode = SamplingMode::Active;
    ++policy.releaseEpoch;
    assert(schedule.mustRelease(policy, 1)); // Coalesced suspend/resume still releases.
    schedule.nextSample = 1000;
    assert(schedule.delayUntil(schedule.nextSample, 100) == 900);
    assert(schedule.delayUntil(schedule.nextSample, 900) == 100);
    assert(schedule.delayUntil(schedule.nextSample, 1000) == 0);
    g_policyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    assert(g_policyEvent);
    showIsland(g_hwnd);
    updateSamplingPolicy(g_hwnd);
    ResetEvent(g_policyEvent);
    for (int i = 0; i < 10; ++i)
        SendMessageW(g_hwnd, WM_TIMER, TELEMETRY_HEALTH_TIMER_ID, 0);
    assert(WaitForSingleObject(g_policyEvent, 0) == WAIT_TIMEOUT);
    publishSamplingPolicy(false); // Simulate missed initial visibility notification.
    ResetEvent(g_policyEvent);
    SendMessageW(g_hwnd, WM_TIMER, TELEMETRY_HEALTH_TIMER_ID, 0);
    assert(g_sampling.load() && WaitForSingleObject(g_policyEvent, 0) == WAIT_OBJECT_0);
    puts("PASS: bounded retention, immediate release policy, monotonic deadline and logon watchdog recovery.");
    testWorkerLifecycle();
    g_autoHideOnHover=false;
    enter(); pump(1200);
    assert(IsWindowVisible(g_hwnd) && !g_trackingMouse);
    g_autoHideOnHover=true;
    leave(); enter();
    pump(700); assert(IsWindowVisible(g_hwnd));
    pump(500); assert(!IsWindowVisible(g_hwnd) && g_hoverHidden);
    // The OS-generated leave caused by hiding must not erase consumed hover.
    SendMessageW(g_hwnd, WM_MOUSELEAVE, 0, 0);
    assert(g_hoverConsumed);
    pump(4500); assert(!IsWindowVisible(g_hwnd));
    pump(700); assert(IsWindowVisible(g_hwnd));
    pump(1200); assert(IsWindowVisible(g_hwnd));
    puts("PASS: disabled hover auto-hide, 1s hover, 5s hidden, visible again without repeated hide.");
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
    assert(samplingPolicy().mode == SamplingMode::Released);
    ResetEvent(g_policyEvent);
    updateSamplingPolicy(g_hwnd);
    assert(WaitForSingleObject(g_policyEvent, 0) == WAIT_TIMEOUT);
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
    assert(samplingPolicy().mode == SamplingMode::Hover);
    const auto hoverDeadline = samplingPolicy().hoverDeadline;
    updateSamplingPolicy(g_hwnd);
    assert(samplingPolicy().hoverDeadline == hoverDeadline);
    SendMessageW(g_hwnd, WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
    assert(samplingPolicy().mode == SamplingMode::Released);
    SendMessageW(g_hwnd, WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
    toggleIsland(g_hwnd);
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    assert(g_stopEvent);
    DestroyWindow(g_hwnd);
    assert(WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0);
    CloseHandle(g_stopEvent); g_stopEvent = nullptr;
    CloseHandle(g_policyEvent); g_policyEvent = nullptr;
    assert(!g_logoDc && !g_backBuffer.dc && !g_backBuffer.bitmap);
    puts("PASS: hidden/suspend sampling policy, battery hover and exit stop signal.");
    DeleteObject(g_metricsFont); DeleteObject(g_smallFont); DeleteObject(g_intelLogo); DeleteObject(g_backgroundBrush);
    return 0;
}

