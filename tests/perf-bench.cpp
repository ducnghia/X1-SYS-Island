// Microbenchmark for the paint and telemetry hot paths. It needs neither
// Administrator access nor a visible window, so before/after numbers can be
// compared on the same machine. Results are indicative, not absolute.
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include "../x1_sys_island.cpp"

static double microseconds() {
    static const double frequency = [] {
        LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return static_cast<double>(value.QuadPart);
    }();
    LARGE_INTEGER counter{}; QueryPerformanceCounter(&counter);
    return counter.QuadPart * 1e6 / frequency;
}

template<class Work>
static double averageUs(int iterations, Work&& work) {
    for (int i = 0; i < iterations / 10 + 1; ++i) work(i);
    GdiFlush();
    const double start = microseconds();
    for (int i = 0; i < iterations; ++i) work(i);
    GdiFlush();
    return (microseconds() - start) / iterations;
}

static HRGN animationDamage(const RECT& rc) {
    HRGN damage = CreateRectRgnIndirect(&COMPACT_CELLS[0].label);
    for (const auto& rect : borderRects(rc)) {
        HRGN strip = CreateRectRgnIndirect(&rect);
        CombineRgn(damage, damage, strip, RGN_OR);
        DeleteObject(strip);
    }
    return damage;
}

static void benchmarkRendering() {
    g_metricsFont = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_smallFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_intelLogo = LoadBitmapW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDB_INTEL_LOGO));
    g_backgroundBrush = CreateSolidBrush(RGB(18, 18, 20));
    g_snapshot.cpu = 17; g_snapshot.gpu = 32; g_snapshot.temperature = 54; g_snapshot.tjMax = 100;
    g_snapshot.used = 32ULL << 30; g_snapshot.total = 64ULL << 30;
    g_snapshot.fans = {6172, 5382, 2, true};
    refreshDisplayCache();
    g_onBattery = false;

    HDC screen = GetDC(nullptr);
    BackBuffer buffer;
    if (!buffer.ensure(screen, ISLAND_WIDTH, EXPANDED_HEIGHT)) { puts("Back buffer unavailable"); return; }
    for (bool expanded : {false, true}) {
        g_expanded = expanded;
        const RECT rc{0, 0, ISLAND_WIDTH, expanded ? EXPANDED_HEIGHT : COMPACT_HEIGHT};
        HRGN damage = animationDamage(rc);
        const double full = averageUs(2000, [&](int) { render(buffer.dc, rc); });
        const double frame = averageUs(4000, [&](int) { render(buffer.dc, rc, damage); });
        printf("%-9s full render %8.1f us | animation frame %8.1f us (%.2f ms CPU/s at 10 fps)\n",
            expanded ? "expanded" : "compact", full, frame, frame * 10 / 1000);
        HRGN border = CreateRectRgn(0, 0, 0, 0);
        for (const auto& rect : borderRects(rc)) {
            HRGN strip = CreateRectRgnIndirect(&rect);
            CombineRgn(border, border, strip, RGN_OR);
            DeleteObject(strip);
        }
        HRGN label = CreateRectRgnIndirect(&COMPACT_CELLS[0].label);
        printf("          border only %8.1f us | CPU label only %8.1f us\n",
            averageUs(2000, [&](int) { render(buffer.dc, rc, border); }),
            averageUs(2000, [&](int) { render(buffer.dc, rc, label); }));
        DeleteObject(border); DeleteObject(label);

        DeleteObject(damage);
    }
    buffer.release();
    ReleaseDC(nullptr, screen);
    releaseLogoDc();
}

static void benchmarkPdh() {
    for (bool gpu : {false, true}) {
        PDH_HQUERY query{};
        PDH_HCOUNTER cpuCounter{}, gpuCounter{};
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) { puts("PDH unavailable"); return; }
        PdhAddEnglishCounterW(query, L"\\Processor(_Total)\\% Processor Time", 0, &cpuCounter);
        if (gpu) PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &gpuCounter);
        PdhCollectQueryData(query);
        Sleep(300);
        std::vector<BYTE> buffer;
        DWORD instances = 0;
        double collect = 0, format = 0;
        constexpr int samples = 8;
        for (int i = 0; i < samples; ++i) {
            Sleep(250);
            double start = microseconds();
            PdhCollectQueryData(query);
            collect += microseconds() - start;
            if (!gpu) continue;
            start = microseconds();
            DWORD bytes = 0, count = 0;
            if (PdhGetFormattedCounterArrayW(gpuCounter, PDH_FMT_DOUBLE, &bytes, &count, nullptr) == PDH_MORE_DATA) {
                buffer.resize(bytes);
                PdhGetFormattedCounterArrayW(gpuCounter, PDH_FMT_DOUBLE, &bytes, &count,
                    reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data()));
            }
            format += microseconds() - start;
            instances = count;
        }
        printf("PDH %-14s collect %8.1f us", gpu ? "CPU+GPU(*)" : "CPU only", collect / samples);
        if (gpu) printf(" | GPU array size+format %8.1f us (%lu instances)", format / samples, instances);
        puts("");
        PdhCloseQuery(query);
    }
}

static void benchmarkTelemetry() {
    double start = microseconds();
    Telemetry telemetry;
    printf("Telemetry construction %10.1f us\n", microseconds() - start);
    Sleep(1000);
    double total = 0;
    constexpr int samples = 6;
    for (int i = 0; i < samples; ++i) {
        Sleep(500);
        start = microseconds();
        Stats stats = telemetry.sample();
        total += microseconds() - start;
        (void)stats;
    }
    printf("Telemetry::sample      %10.1f us (average of %d)\n", total / samples, samples);
    printf("readFans               %10.1f us\n", averageUs(2000, [](int) { (void)readFans(); }));
}

int main() {
    benchmarkRendering();
    benchmarkPdh();
    benchmarkTelemetry();
    DeleteObject(g_metricsFont); DeleteObject(g_smallFont);
    if (g_intelLogo) DeleteObject(g_intelLogo);
    DeleteObject(g_backgroundBrush);
    return 0;
}
