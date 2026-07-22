#include "core/Logger.h"

#include <Windows.h>

#include <chrono>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>

namespace backtrack {

namespace {

constexpr uintmax_t kMaxLogBytes = 8ull * 1024ull * 1024ull;
constexpr uintmax_t kRecentLogReadBytes = 256ull * 1024ull;

const wchar_t* levelName(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return L"DEBUG";
    case LogLevel::Info:
        return L"INFO";
    case LogLevel::Warning:
        return L"WARN";
    case LogLevel::Error:
        return L"ERROR";
    }
    return L"INFO";
}

std::wstring timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    localtime_s(&localTime, &time);

    std::wstringstream stream;
    stream << std::put_time(&localTime, L"%Y-%m-%d %H:%M:%S");
    return stream.str();
}

void rotateLogIfNeeded(const std::filesystem::path& logPath) {
    std::error_code error;
    if (!std::filesystem::exists(logPath, error) || error) {
        return;
    }
    const auto bytes = std::filesystem::file_size(logPath, error);
    if (error || bytes < kMaxLogBytes) {
        return;
    }

    const std::filesystem::path archivedPath(logPath.wstring() + L".old");
    std::filesystem::remove(archivedPath, error);
    error = {};
    std::filesystem::rename(logPath, archivedPath, error);
    if (error) {
        OutputDebugStringW((L"Could not rotate Backtrack log file: " + logPath.wstring() + L"\n").c_str());
    }
}

std::wstring logBytesToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (size > 0) {
        std::wstring result(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            size);
        return result;
    }
    return {value.begin(), value.end()};
}

} // namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::initialize(const std::filesystem::path& logPath) {
    std::scoped_lock lock(mutex_);
    if (stream_.is_open()) {
        stream_.close();
    }
    if (!logPath.parent_path().empty()) {
        std::error_code error;
        std::filesystem::create_directories(logPath.parent_path(), error);
        if (error) {
            OutputDebugStringW((L"Could not create Backtrack log directory: " + logPath.parent_path().wstring() + L"\n").c_str());
            return;
        }
    }
    rotateLogIfNeeded(logPath);
    logPath_ = logPath;
    stream_.open(logPath, std::ios::out | std::ios::binary | std::ios::app);
    if (!stream_.is_open()) {
        OutputDebugStringW((L"Could not open Backtrack log file: " + logPath.wstring() + L"\n").c_str());
    }
    rateStates_.clear();
}

void Logger::setMinLevel(LogLevel level) {
    std::scoped_lock lock(mutex_);
    minLevel_ = level;
}

LogLevel Logger::minLevel() const {
    std::scoped_lock lock(mutex_);
    return minLevel_;
}

void Logger::writeLine(const std::wstring& text) {
    // Caller holds mutex_.
    if (stream_.is_open()) {
        stream_ << wideToUtf8(text);
        stream_.flush();
    }
    OutputDebugStringW(text.c_str());
}

void Logger::write(LogLevel level, const std::wstring& message) {
    std::scoped_lock lock(mutex_);

    if (level < minLevel_) {
        return;
    }

    // Windowed per-message rate limiting. Errors are never throttled so real
    // failures always surface. Any other level is capped at kMaxPerWindow lines
    // per kWindow; excess occurrences are counted and reported as a single
    // summary line when the window rolls over. This bounds interleaved spam
    // (e.g. monitor-switch churn, retry loops) that previously produced tens of
    // thousands of lines, without hiding sporadic events.
    constexpr auto kWindow = std::chrono::seconds(10);
    constexpr size_t kMaxPerWindow = 3;

    const auto now = std::chrono::steady_clock::now();
    const bool throttleable = level != LogLevel::Error;

    if (throttleable) {
        // Bound memory: messages with embedded values (PIDs, dimensions) create
        // distinct keys, so prune entries whose window has long since expired.
        constexpr size_t kMaxRateEntries = 512;
        if (rateStates_.size() > kMaxRateEntries) {
            for (auto it = rateStates_.begin(); it != rateStates_.end();) {
                if (it->second.suppressed == 0 && now - it->second.windowStart >= kWindow) {
                    it = rateStates_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        auto& state = rateStates_[message];
        state.level = level;
        if (state.windowStart == std::chrono::steady_clock::time_point{} ||
            now - state.windowStart >= kWindow) {
            // Window rolled over: report anything suppressed in the prior window.
            if (state.suppressed > 0) {
                std::wstringstream summary;
                summary << L"[" << timestamp() << L"] [" << levelName(state.level) << L"] "
                        << message << L" (rate limited " << state.suppressed
                        << L" occurrence" << (state.suppressed == 1 ? L"" : L"s")
                        << L" in previous " << std::chrono::duration_cast<std::chrono::seconds>(kWindow).count()
                        << L"s)\n";
                writeLine(summary.str());
            }
            state.windowStart = now;
            state.emitted = 0;
            state.suppressed = 0;
        }

        if (state.emitted >= kMaxPerWindow) {
            ++state.suppressed;
            return;
        }
        ++state.emitted;
    }

    std::wstringstream line;
    line << L"[" << timestamp() << L"] [" << levelName(level) << L"] " << message << L"\n";
    writeLine(line.str());
}

std::filesystem::path Logger::currentPath() const {
    std::scoped_lock lock(mutex_);
    return logPath_;
}

std::vector<std::wstring> Logger::recentLines(size_t maxLines) const {
    std::filesystem::path path;
    {
        std::scoped_lock lock(mutex_);
        path = logPath_;
    }
    if (path.empty() || maxLines == 0) {
        return {};
    }

    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    if (error) {
        return {};
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return {};
    }

    const bool truncated = bytes > kRecentLogReadBytes;
    if (truncated) {
        stream.seekg(static_cast<std::streamoff>(bytes - kRecentLogReadBytes), std::ios::beg);
    }
    std::string data((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (truncated) {
        const auto firstNewline = data.find('\n');
        if (firstNewline != std::string::npos) {
            data.erase(0, firstNewline + 1);
        }
    }

    std::deque<std::wstring> lines;
    std::istringstream input(data);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(logBytesToWide(line));
        while (lines.size() > maxLines) {
            lines.pop_front();
        }
    }
    return {lines.begin(), lines.end()};
}

std::wstring hresultToString(HRESULT hr) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(hr),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    std::wstringstream fallback;
    fallback << L"HRESULT 0x" << std::uppercase << std::hex << static_cast<unsigned long>(hr);
    std::wstring message = fallback.str();
    if (length > 0 && buffer) {
        message.assign(buffer, buffer + length);
        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
            message.pop_back();
        }
        LocalFree(buffer);
    }
    return message;
}

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        return {value.begin(), value.end()};
    }
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        std::string fallback;
        fallback.reserve(value.size());
        for (wchar_t ch : value) {
            fallback.push_back(ch >= 0 && ch <= 0x7f ? static_cast<char>(ch) : '?');
        }
        return fallback;
    }
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

} // namespace backtrack
