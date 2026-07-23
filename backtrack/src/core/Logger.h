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
    void write(LogLevel level, const std::wstring& message);
    void write(LogLevel level, const wchar_t* subsystem, const std::wstring& message);
    std::vector<std::wstring> recentLines(size_t maxLines) const;
    std::filesystem::path currentPath() const;
    void flush();

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
    Logger() = default;

    void writeLine(const std::wstring& text);

    mutable std::mutex mutex_;
    std::filesystem::path logPath_;
    std::ofstream stream_;
    LogLevel minLevel_ = LogLevel::Debug;
    std::deque<std::wstring> recentLines_;

    // Windowed per-message rate limiting. Each distinct message text is allowed
    // a fixed number of lines per rolling time window; further occurrences are
    // counted and collapsed into a single "(rate limited N occurrences)" line
    // when the window rolls over. This catches interleaved spam (A,B,A,B...)
    // that consecutive-duplicate suppression alone would miss.
    struct RateState {
        std::chrono::steady_clock::time_point windowStart{};
        size_t emitted = 0;      // lines written this window
        size_t suppressed = 0;   // lines dropped this window
        LogLevel level = LogLevel::Info;
        DWORD threadId = 0;
        std::wstring subsystem;
    };
    std::unordered_map<std::wstring, RateState> rateStates_;
};

std::wstring hresultToString(HRESULT hr);
std::wstring utf8ToWide(const std::string& value);
std::string wideToUtf8(const std::wstring& value);
const wchar_t* logLevelName(LogLevel level);
LogLevel logLevelFromName(const std::wstring& value, LogLevel fallback = LogLevel::Debug);

} // namespace backtrack
