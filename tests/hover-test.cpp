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
int expandedLabelCalls = 0;
bool countExpandedLabels = false;
RECT lastTextRect{};
UINT lastTextFlags = 0;
int regionCreates = 0;
HRGN TestCreateRectRgn(int l, int t, int r, int b) {
    ++regionCreates;
    return CreateRectRgn(l, t, r, b);
}
#define CreateRectRgn TestCreateRectRgn
LONG largestFill = 0;
int TestDrawTextW(HDC dc, LPCWSTR text, int count, LPRECT rect, UINT flags) {
    ++textCalls;
    lastTextRect = *rect;
    lastTextFlags = flags;
    if (countExpandedLabels && (wcscmp(text, L"RAM") == 0 || wcscmp(text, L"iGPU") == 0 ||
        wcscmp(text, L"CPU Package/TjMax") == 0 || wcscmp(text, L"Fan Mode") == 0 ||
        wcscmp(text, L"Fan 1/Fan 2") == 0 || wcscmp(text, L"rpm") == 0))
        ++expandedLabelCalls;
    return DrawTextW(dc, text, count, rect, flags);
}
int TestFillRect(HDC dc, const RECT* rect, HBRUSH brush) {
    ++fillCalls;
    // Count actual writable pixels, not the bounding box of a disjoint clip.
    // RectVisible on unit rectangles queries the real DC clip without allocating
    // a test region that could hide production region churn.
    LONG area = 0;
    for (LONG y = rect->top; y < rect->bottom; ++y)
        for (LONG x = rect->left; x < rect->right; ++x) {
            RECT pixel{x, y, x+1, y+1};
            if (RectVisible(dc, &pixel)) ++area;
        }
    largestFill = (std::max)(largestFill, area);
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
#undef CreateRectRgn

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
    const HRGN retainedDamage = g_damageRegion.region;
    const int createdRegions = regionCreates;
    const int measuredExtents = extentCalls;
    assert(retainedDamage);
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
        LONG expectedArea = 0;
        for (int y = 0; y < COMPACT_HEIGHT; ++y)
            for (int x = 0; x < ISLAND_WIDTH; ++x)
                if (PtInRegion(damage, x, y)) ++expectedArea;
        assert(largestFill == expectedArea);
        assert(largestFill < ISLAND_WIDTH*COMPACT_HEIGHT);
        assert(g_damageRegion.region == retainedDamage && regionCreates == createdRegions);
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
        RECT client{}; GetClientRect(g_hwnd, &client);
        assert(client.right == 520 && client.bottom == (g_expanded ? 128 : 46));
        assert(g_damageRegion.region == retainedDamage && regionCreates == createdRegions);
        assert(extentCalls == measuredExtents);
        assert(g_backBuffer.valid && g_backBuffer.width == ISLAND_WIDTH);
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
    expected = CreateRectRgnIndirect(&g_expandedCells[5].value);
    GetUpdateRgn(g_hwnd, damage, FALSE);
    assert(EqualRgn(damage, expected));
    DeleteObject(damage); DeleteObject(expected);
    textCalls = expandedLabelCalls = 0;
    countExpandedLabels = true;
    UpdateWindow(g_hwnd);
    assert(textCalls == 1 && expandedLabelCalls == 0);
    countExpandedLabels = false;
    // Every expanded field repairs only its own value, even when its digits
    // grow/shrink. Compare each incremental result to a complete fresh image.
    HDC comparisonDc = GetDC(g_hwnd);
    BackBuffer valueReference;
    assert(valueReference.ensure(comparisonDc, ISLAND_WIDTH, EXPANDED_HEIGHT));
    const RECT expandedRect{0, 0, ISLAND_WIDTH, EXPANDED_HEIGHT};
    g_expandedParts.fill(L"baseline");
    InvalidateRect(g_hwnd, nullptr, FALSE); UpdateWindow(g_hwnd);
    damage = CreateRectRgn(0,0,0,0);
    expected = CreateRectRgn(0,0,0,0);
    for (size_t field = 0; field < g_expandedParts.size(); ++field) {
        for (const wchar_t* value : {L"N/A", L"1", L"100"}) {
            const auto oldParts = g_expandedParts;
            g_expandedParts[field] = value;
            invalidateDisplayChanges(g_compactParts, oldParts, g_cpuLoad, g_loadLevel);
            SetRectRgn(expected, g_expandedCells[field].value.left, g_expandedCells[field].value.top,
                g_expandedCells[field].value.right, g_expandedCells[field].value.bottom);
            GetUpdateRgn(g_hwnd, damage, FALSE);
            assert(EqualRgn(damage, expected));
            textCalls = expandedLabelCalls = 0;
            countExpandedLabels = true;
            UpdateWindow(g_hwnd);
            assert(textCalls == 1 && expandedLabelCalls == 0);
            assert(EqualRect(&lastTextRect, &g_expandedCells[field].value));
            assert(lastTextFlags & DT_RIGHT);
            countExpandedLabels = false;
            render(valueReference.dc, expandedRect);
            for (int y = 0; y < EXPANDED_HEIGHT; ++y)
                for (int x = 0; x < ISLAND_WIDTH; ++x)
                    assert(GetPixel(valueReference.dc,x,y) == GetPixel(g_backBuffer.dc,x,y));
        }
    }
    DeleteObject(damage); DeleteObject(expected);
    valueReference.release();
    ReleaseDC(g_hwnd, comparisonDc);
    // Restore real formatted values before the remaining exposure checks.
    refreshDisplayCache(); UpdateWindow(g_hwnd);
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
    assert(textCalls == 19 && logoCalls == 1); // 6 compact + 12 expanded + static rpm.
    assert(g_damageRegion.region == retainedDamage && regionCreates == createdRegions);
    assert(extentCalls == measuredExtents);
    ReleaseDC(g_hwnd,target);
    reference.release();
    g_expanded = false; setWindowSize(); UpdateWindow(g_hwnd);
    GdiFlush();
    assert(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == resizeHandles);
    g_onBattery = false;
    SetWindowPos(g_hwnd, nullptr, -30000, -30000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    puts("PASS: exact damage, animation-only draws, pixel-equivalent repair, exposure, resize and stable backbuffer handles.");
}
#pragma comment(lib, "version.lib")
void testAboutVersion() {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    assert(length && length < ARRAYSIZE(path));
    const DWORD bytes = GetFileVersionInfoSizeW(path, nullptr);
    assert(bytes);
    std::vector<BYTE> metadata(bytes);
    assert(GetFileVersionInfoW(path, 0, bytes, metadata.data()));
    VS_FIXEDFILEINFO* info = nullptr;
    UINT size = 0;
    assert(VerQueryValueW(metadata.data(), L"\\", reinterpret_cast<void**>(&info), &size));
    assert(size >= sizeof(*info) && info->dwSignature == 0xfeef04bd);
    const auto version = formatText(L"%u.%u.%u", HIWORD(info->dwProductVersionMS),
        LOWORD(info->dwProductVersionMS), HIWORD(info->dwProductVersionLS));
    wchar_t* product = nullptr;
    assert(VerQueryValueW(metadata.data(), L"\\StringFileInfo\\040904b0\\ProductVersion",
        reinterpret_cast<void**>(&product), &size));
    assert(product && version == product);
    assert(info->dwFileVersionMS == info->dwProductVersionMS &&
        info->dwFileVersionLS == info->dwProductVersionLS);
    wchar_t* file = nullptr;
    assert(VerQueryValueW(metadata.data(), L"\\StringFileInfo\\040904b0\\FileVersion",
        reinterpret_cast<void**>(&file), &size));
    assert(file && version + formatText(L".%u", LOWORD(info->dwFileVersionLS)) == file);
    HWND dialog = CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_ABOUT_DIALOG),
        g_hwnd, AboutDialogProc, 0);
    assert(dialog);
    wchar_t text[256]{};
    assert(GetDlgItemTextW(dialog, IDC_ABOUT_VERSION, text, ARRAYSIZE(text)));
    assert(std::wstring(text) == L"X1 SYS Island v" + version);
    assert(reinterpret_cast<HFONT>(SendDlgItemMessageW(dialog, IDC_ABOUT_VERSION,
        WM_GETFONT, 0, 0)) == g_metricsFont);
    assert(DestroyWindow(dialog));
    puts("PASS: About version from resources matches numeric/string metadata; WM_SETFONT applies the font.");
}
int main() {
    g_metricsFont = CreateFontW(-14,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    g_smallFont = CreateFontW(-14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    assert(g_metricsFont && g_smallFont);
    g_intelLogo=LoadBitmapW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDB_INTEL_LOGO));
    g_backgroundBrush = CreateSolidBrush(RGB(18,18,20));
    WNDCLASSW cls{}; cls.lpfnWndProc=TestProc; cls.hInstance=GetModuleHandleW(nullptr); cls.lpszClassName=L"X1SYSHoverTest";
    assert(RegisterClassW(&cls));
    g_hwnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,cls.lpszClassName,L"Hover test",WS_POPUP,
        -30000,-30000,ISLAND_WIDTH,COMPACT_HEIGHT,nullptr,nullptr,cls.hInstance,nullptr);
    assert(g_hwnd);
    testAboutVersion();
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
            SIZE size{};
            assert(GetTextExtentPoint32W(dc, values[i].c_str(), static_cast<int>(values[i].size()), &size));
            printf("Cell %zu needs %ld px, has %ld px\n", i, size.cx, cell.value.right-cell.value.left);
            assert(size.cx <= cell.value.right-cell.value.left);
        }
        SelectObject(dc, previous);
    };
    ensureExpandedLayout(dc);
    auto unitFont = SelectObject(dc, g_smallFont);
    SIZE unitSize{};
    assert(GetTextExtentPoint32W(dc, L"rpm", 3, &unitSize));
    assert(unitSize.cx == g_fanUnit.right - g_fanUnit.left);
    assert(g_expandedCells[5].value.right + 4 == g_fanUnit.left);
    assert(g_fanUnit.right == EXPANDED_CELLS[5].right);
    SelectObject(dc, unitFont);
    auto checkExpandedCells = [&](const auto& cells, const auto& values, HFONT font) {
        checkCells(cells, values, font);
        auto previous = SelectObject(dc, font);
        for (size_t i = 0; i < cells.size(); ++i) {
            const auto& cell = cells[i];
            SIZE size{};
            assert(GetTextExtentPoint32W(dc, EXPANDED_LABELS[i].c_str(),
                static_cast<int>(EXPANDED_LABELS[i].size()), &size));
            assert(cell.value.left - cell.label.right == 8);
            printf("Expanded cell %zu: label %ld/%ld px, value region %ld px\n",
                i, size.cx, cell.label.right-cell.label.left, cell.value.right-cell.value.left);
            assert(cell.label.right-cell.label.left == size.cx);
        }
        SelectObject(dc, previous);
    };
    assert(EXPANDED_LABELS[2] == L"iGPU" && EXPANDED_LABELS[3] == L"RAM");
    assert(EXPANDED_LABELS[5] == L"Fan 1/Fan 2");
    assert(g_expandedParts[3] == L"64.0/64.0 GiB (100%)");
    assert(g_expandedParts[5] == L"8191/8191");
    checkCells(COMPACT_CELLS, g_compactParts, g_metricsFont);
    checkExpandedCells(g_expandedCells, g_expandedParts, g_smallFont);
    auto previous = SelectObject(dc, g_metricsFont);
    // CPU identity may intentionally ellipsize; the other compact labels must not.
    for (size_t i = 1; i < COMPACT_LABELS.size(); ++i) {
        if (COMPACT_LABELS[i].empty()) continue;
        SIZE size{};
        assert(GetTextExtentPoint32W(dc, COMPACT_LABELS[i].c_str(),
            static_cast<int>(COMPACT_LABELS[i].size()), &size));
        const auto& label = COMPACT_CELLS[i].label;
        printf("Compact label %ls: %ld px / %ld px\n", COMPACT_LABELS[i].c_str(), size.cx, label.right-label.left);
        assert(size.cx <= label.right-label.left && size.cy <= label.bottom-label.top);
        assert(COMPACT_CELLS[i-1].value.right <= label.left);
    }
    SelectObject(dc, previous);
    g_snapshot.temperature = 100; g_snapshot.tjMax = 100;
    g_snapshot.used = g_snapshot.total = static_cast<ULONGLONG>(63.7 * 1073741824.0);
    refreshDisplayCache();
    const std::array<std::wstring, 4> typicalValues{L"100%", L"63.7/63.7G", L"100\u00B0C/100\u00B0C", L"100%"};
    assert(g_compactParts == typicalValues);
    checkCells(COMPACT_CELLS, g_compactParts, g_metricsFont);
    checkExpandedCells(g_expandedCells, g_expandedParts, g_smallFont);
    ReleaseDC(g_hwnd,dc);
    assert(g_loadLevel==LoadLevel::Red);
    assert(levelForPercent(49)==LoadLevel::Normal);
    assert(levelForPercent(50)==LoadLevel::Yellow);
    assert(levelForPercent(80)==LoadLevel::Red);
    puts("PASS: 520px layout, value-only RAM and C-Pkg compact cells, and border thresholds.");
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
    const auto expandedCells = g_expandedCells;
    for (double value : {-1.0, 0.0, 1.0, 9.0, 10.0, 99.0, 100.0}) {
        g_snapshot.cpu = g_snapshot.gpu = value;
        refreshDisplayCache();
        render(cacheDc, compactRect);
        for (size_t i = 0; i < compactCells.size(); ++i) {
            assert(EqualRect(&compactCells[i].label, &COMPACT_CELLS[i].label));
            assert(EqualRect(&compactCells[i].value, &COMPACT_CELLS[i].value));
        }
        for (size_t i = 0; i < expandedCells.size(); ++i)
        {
            assert(EqualRect(&expandedCells[i].label, &g_expandedCells[i].label));
            assert(EqualRect(&expandedCells[i].value, &g_expandedCells[i].value));
        }
    }
    assert(extentCalls == measured);
    ReleaseDC(g_hwnd, cacheDc);
    refreshDisplayCache();
    puts("PASS: measured 8px expanded label gaps, fixed N/A/1/100 value positions, retained logo and stable GDI handles.");
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
    assert(!g_logoDc && !g_backBuffer.dc && !g_backBuffer.bitmap && !g_damageRegion.region);
    puts("PASS: hidden/suspend sampling policy, battery hover and exit stop signal.");
    DeleteObject(g_metricsFont); DeleteObject(g_smallFont); DeleteObject(g_intelLogo); DeleteObject(g_backgroundBrush);
    return 0;
}

