#pragma once

#include "core/Logger.h"

#include <chrono>
#include <string>

namespace backtrack {

// Measure an operation and log a rate-limited WARN only when it breaches its
// latency budget. The logger's text-keyed limiter collapses repeat breaches,
// so a hot path can call this every iteration. Header-only so suites that link
// core/Logger.cpp need no extra object file.
class ScopedLatencyWarn {
public:
    ScopedLatencyWarn(const wchar_t* subsystem, std::wstring message, std::chrono::milliseconds budget)
        : subsystem_(subsystem), message_(std::move(message)), budget_(budget),
          start_(std::chrono::steady_clock::now()) {}

    ~ScopedLatencyWarn() {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_);
        if (elapsed >= budget_) {
            Logger::instance().warning(subsystem_,
                message_ + L" (duration_ms=" + std::to_wstring(elapsed.count()) +
                L" budget_ms=" + std::to_wstring(budget_.count()) + L")");
        }
    }

private:
    const wchar_t* subsystem_;
    std::wstring message_;
    std::chrono::milliseconds budget_;
    std::chrono::steady_clock::time_point start_;
};

// One-shot form: log a WARN when the elapsed time since `start` exceeds
// `budget`, with optional extra key=value pairs appended.
inline void logLatencyWarn(const wchar_t* subsystem, std::wstring message,
    std::chrono::steady_clock::time_point start, std::chrono::milliseconds budget,
    std::wstring extra = {}) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    if (elapsed >= budget) {
        Logger::instance().warning(subsystem,
            std::move(message) + L" (duration_ms=" + std::to_wstring(elapsed.count()) +
            L" budget_ms=" + std::to_wstring(budget.count()) +
            (extra.empty() ? std::wstring{} : L" " + std::move(extra)) + L")");
    }
}

} // namespace backtrack
