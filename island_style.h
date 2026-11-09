#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>

// Pure colour logic shared by rendering and invalidation. No GDI state.
enum class LoadLevel {
    Normal,
    Yellow,
    Red
};

inline LoadLevel levelForPercent(unsigned value) {
    if (value >= 80) return LoadLevel::Red;
    if (value >= 50) return LoadLevel::Yellow;
    return LoadLevel::Normal;
}

inline BYTE channel(double value) {
    return static_cast<BYTE>((std::max)(0.0, (std::min)(255.0, value)));
}

inline COLORREF pulseColor(LoadLevel level, double phase) {
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

inline COLORREF animatedBorderColor(LoadLevel level, ULONGLONG now) {
    double period = level == LoadLevel::Normal ? 2400.0 : (level == LoadLevel::Yellow ? 1400.0 : 1050.0);
    double phase = 6.283185307179586 * (static_cast<double>(now % static_cast<ULONGLONG>(period)) / period);
    return pulseColor(level, phase);
}

inline COLORREF cpuLabelColor(double load, ULONGLONG now) {
    if (load < 0) return RGB(110, 110, 116);
    const auto level = levelForPercent(static_cast<unsigned>(std::lround(std::clamp(load, 0.0, 100.0))));
    if (level == LoadLevel::Normal) {
        const double phase = 6.283185307179586 * (static_cast<double>(now % 2400) / 2400.0);
        const double breath = 0.65 + 0.35 * (std::sin(phase + 0.55) + 1.0) * 0.5;
        return RGB(0, channel(128 * breath), channel(255 * breath));
    }
    return animatedBorderColor(level, now);
}
