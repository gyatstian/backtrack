#pragma once

#include "core/Types.h"

#include <chrono>
#include <cstdint>

namespace backtrack {

// QPC ticks -> 100ns units at the given performance-counter frequency. Pure
// conversion so WASAPI timestamp calibration shares one expression.
inline int64_t qpcTo100ns(int64_t ticks, int64_t frequency) {
    if (frequency <= 0) {
        return 0;
    }
    return static_cast<int64_t>(
        (static_cast<long double>(ticks) * kHundredNanosecondsPerSecond) /
        static_cast<long double>(frequency));
}

// Current performance-counter reading in 100ns units; 0 when unavailable.
inline int64_t qpcNow100ns() {
    LARGE_INTEGER frequency{};
    LARGE_INTEGER counter{};
    if (!QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&counter) ||
        frequency.QuadPart <= 0) {
        return 0;
    }
    return qpcTo100ns(counter.QuadPart, frequency.QuadPart);
}

inline int64_t steadyNow100ns() {
    return std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline int64_t steadyTimePoint100ns(SteadyClock::time_point time) {
    return std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>>(
               time.time_since_epoch())
        .count();
}

// Inverse of steadyTimePoint100ns. WGC SystemRelativeTime and MSVC steady_clock
// are both QPC-derived (same epoch), so a source present time in 100ns units maps
// back to a steady_clock instant for emit-grid phase locking.
inline SteadyClock::time_point steadyPointFrom100ns(int64_t value100ns) {
    return SteadyClock::time_point(
        std::chrono::duration_cast<SteadyClock::duration>(
            std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>(value100ns)));
}

} // namespace backtrack
