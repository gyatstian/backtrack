#pragma once

#include "core/Types.h"

#include <Windows.h>

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace backtrack {

class Logger {
public:
    static Logger& instance();

    void initialize(const std::filesystem::path& logPath);
    void setMinLevel(LogLevel level);
    LogLevel minLevel() const;
    void setEnabled(bool enabled);
    bool enabled() const;
    void write(LogLevel level, const std::wstring& message);
    void write(LogLevel level, const wchar_t* subsystem, const std::wstring& message);
    // Log the session header (project version, date, environment) at startup
    // and the warning/error summary at shutdown.
    void writeHeader();
    void writeFooter();
    std::vector<std::wstring> recentLines(size_t maxLines);
    std::filesystem::path currentPath() const;
    bool flush();

    void trace(const std::wstring& message) { write(LogLevel::Trace, message); }
    void debug(const std::wstring& message) { write(LogLevel::Debug, message); }
    void info(const std::wstring& message) { write(LogLevel::Info, message); }
    void warning(const std::wstring& message) { write(LogLevel::Warning, message); }
    void error(const std::wstring& message) { write(LogLevel::Error, message); }
    void trace(const wchar_t* subsystem, const std::wstring& message) { write(LogLevel::Trace, subsystem, message); }
    void debug(const wchar_t* subsystem, const std::wstring& message) { write(LogLevel::Debug, subsystem, message); }
    void info(const wchar_t* subsystem, const std::wstring& message) { write(LogLevel::Info, subsystem, message); }
    void warning(const wchar_t* subsystem, const std::wstring& message) { write(LogLevel::Warning, subsystem, message); }
    void error(const wchar_t* subsystem, const std::wstring& message) { write(LogLevel::Error, subsystem, message); }

private:
    friend struct LoggerTestAccess;
    Logger() = default;

    void writeLine(const std::wstring& text);
    void writeLocked(LogLevel level, const wchar_t* subsystem, const std::wstring& message,
        std::chrono::steady_clock::time_point now);
    // caller holds mutex_. Flushes the deferred consecutive-duplicate group, if
    // any, through the rate limiter.
    void flushPendingGroupLocked(std::chrono::steady_clock::time_point now);

    mutable std::mutex mutex_;
    std::filesystem::path logPath_;
    std::ofstream stream_;
    LogLevel minLevel_ = LogLevel::Trace;  // highest verbosity; always logs everything
    bool enabled_ = true;
    std::deque<std::wstring> recentLines_;

    // Windowed per-message rate limiting. Each distinct message text is allowed
    // a fixed number of lines per rolling time window; further occurrences are
    // counted and collapsed into a single "(rate limited N occurrences)" line
    // when the window rolls over or an expired key is pruned. This catches
    // interleaved spam that consecutive-duplicate suppression would miss.
    struct RateState {
        std::chrono::steady_clock::time_point windowStart{};
        size_t emitted = 0;      // lines written this window
        size_t suppressed = 0;   // lines dropped this window
        LogLevel level = LogLevel::Info;
        DWORD threadId = 0;
        std::wstring subsystem;
    };
    // caller holds mutex_; summaries must not re-enter the rate limiter.
    void writeRateSummary(const std::wstring& message, const RateState& state);
    std::unordered_map<std::wstring, RateState> rateStates_;

    // Consecutive-duplicate grouping: back-to-back identical messages collapse
    // into one line with a "(Nx)" suffix, flushed when a different message
    // arrives (or on flush/export/shutdown). Suppressed occurrences are still
    // counted for the shutdown summary.
    struct PendingGroup {
        LogLevel level = LogLevel::Info;
        std::wstring subsystem;
        std::wstring message;
        std::wstring task;   // correlation label captured at write time
        std::wstring trail;  // error only: breadcrumbs at write time
        std::wstring stack;  // error only: return addresses at write time
        size_t count = 0;
        bool active = false;
    };
    PendingGroup pendingGroup_;
    size_t warnings_ = 0;       // WARN events that passed minLevel
    size_t errors_ = 0;         // ERROR events that passed minLevel
    size_t rateLimitedLines_ = 0; // occurrences suppressed by the rate limiter
};

std::wstring hresultToString(HRESULT hr);
std::wstring utf8ToWide(const std::string& value);
std::string wideToUtf8(const std::wstring& value);

} // namespace backtrack
