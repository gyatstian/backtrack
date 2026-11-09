#pragma once

#include <chrono>
#include <cstdint>

namespace backtrack {

// Redeclared here so a timestamp-only header can convert units without pulling
// in core/Types.h (Windows, D3D11, DXGI, WRL).
using SteadyClock = std::chrono::steady_clock;

// 100 ns is the unit every module boundary carries (pts100ns, dts100ns,
// duration100ns). Signed, and zero and negative are legal values.
inline constexpr int64_t kHundredNanosecondsPerSecond = 10'000'000;

inline int64_t secondsTo100ns(uint32_t seconds) {
    return static_cast<int64_t>(seconds) * kHundredNanosecondsPerSecond;
}

} // namespace backtrack
