#include "core/Logger.h"

#include "core/AppVersion.h"
#include "core/LogContext.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace backtrack {

namespace {

constexpr size_t kRecentLineCapacity = 200;
constexpr auto kRateWindow = std::chrono::seconds(10);

const wchar_t* levelName(LogLevel level) {
    switch (level) {
    case LogLevel::Trace:
        return L"TRACE";
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
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch() % std::chrono::seconds(1)).count();
    std::wstringstream stream;
    stream << std::put_time(&localTime, L"%H:%M:%S") << L"."
           << std::setw(3) << std::setfill(L'0') << millis;
    return stream.str();
}

// Real OS build number. RtlGetVersion is the unmanifested, non-deprecated
// source: GetVersionExW lies (returns 6.2 without a compatibility manifest)
// and is deprecated. The struct mirrors RTL_OSVERSIONINFOW so winternl.h is
// not needed.
struct OsVersionInfo {
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR szCSDVersion[128];
};
using RtlGetVersionFn = LONG(WINAPI*)(OsVersionInfo*);

std::wstring osVersionString() {
    if (const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll")) {
        if (const auto fn = reinterpret_cast<RtlGetVersionFn>(
                ::GetProcAddress(ntdll, "RtlGetVersion"))) {
            OsVersionInfo info{};
            info.dwOSVersionInfoSize = sizeof(info);
            if (fn(&info) == 0) {  // STATUS_SUCCESS
                // 10.0 build >= 22000 is Windows 11 (10.0 otherwise).
                const std::wstring friendly =
                    info.dwMajorVersion == 10 && info.dwBuildNumber >= 22000
                    ? L"Windows 11"
                    : L"Windows " + std::to_wstring(info.dwMajorVersion) + L"." +
                          std::to_wstring(info.dwMinorVersion);
                return friendly + L" (build " + std::to_wstring(info.dwBuildNumber) + L")";
            }
        }
    }
    return L"unknown";
}

std::wstring dateString() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    localtime_s(&localTime, &time);
    std::wstringstream stream;
    stream << std::put_time(&localTime, L"%Y-%m-%d");
    return stream.str();
}

// Best-effort return addresses of the current call stack, as hex pointers.
// PDBs are needed to turn these into symbols; they still distinguish "which
// path logged this" when the same text has multiple callers.
std::wstring captureStackTrace() {
    void* frames[16] = {};
    const USHORT count = RtlCaptureStackBackTrace(2, 16, frames, nullptr);
    if (count == 0) {
        return {};
    }
    std::wstring result;
    for (USHORT index = 0; index < count; ++index) {
        if (index > 0) {
            result += L" ";
        }
        wchar_t buffer[32] = {};
        swprintf_s(buffer, L"%p", frames[index]);
        result += buffer;
    }
    return result;
}

// Mask values of known credential key names (token, secret, password, ...). No
// call site logs secrets today; this keeps bundled logs safe if one is added.
// Paths and window titles are handled at the call sites, never auto-masked
// here - mangling a legitimate path is worse than an unredacted warning.
void redactSecrets(std::wstring& line) {
    // Every written line passes through here while the mutex that capture and
    // encode threads share is held. A line with no '=' cannot name a key below,
    // so the lowercase copy and the per-key scans are skipped for the text that
    // dominates the log.
    if (line.find(L'=') == std::wstring::npos) {
        return;
    }
    static const wchar_t* const kKeys[] = {
        L"token", L"secret", L"password", L"passwd", L"authorization",
        L"api_key", L"apikey", L"api-key", L"access_token",
    };
    std::wstring lower = line;
    for (wchar_t& ch : lower) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    std::vector<std::pair<size_t, size_t>> ranges;  // value spans to mask
    for (const wchar_t* key : kKeys) {
        const std::wstring needle = std::wstring(key) + L"=";
        for (size_t pos = lower.find(needle); pos != std::wstring::npos;
             pos = lower.find(needle, pos + needle.size())) {
            if (pos > 0 && (std::iswalnum(lower[pos - 1]) || lower[pos - 1] == L'_')) {
                continue;  // part of a larger word ("mytoken=" is not the key)
            }
            const size_t valueStart = pos + needle.size();
            const size_t valueEnd = line.find_first_of(L" \t,()", valueStart);
            ranges.emplace_back(valueStart, valueEnd == std::wstring::npos ? line.size() : valueEnd);
        }
    }
    std::sort(ranges.begin(), ranges.end(),
        [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& range : ranges) {
        line.replace(range.first, range.second - range.first, L"***");
    }
}

// Returns the path initialize() may truncate. When an existing log cannot be
// renamed aside, truncating it would destroy the previous session's log, so the
// new session opens a sidecar beside it instead.
std::filesystem::path archivePreviousLog(const std::filesystem::path& logPath) {
    std::error_code error;
    if (!std::filesystem::exists(logPath, error) || error) {
        return logPath;
    }

    const std::filesystem::path archivedPath(logPath.wstring() + L".old");
    std::filesystem::remove(archivedPath, error);
    error = {};
    std::filesystem::rename(logPath, archivedPath, error);
    if (!error) {
        return logPath;
    }

    const std::filesystem::path sidecar(
        logPath.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".log");
    OutputDebugStringW((L"Could not archive previous Backtrack log file: " + logPath.wstring() +
        L", writing " + sidecar.wstring() + L"\n").c_str());
    return sidecar;
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
    // Reset before any early return: a failed initialize must not leave the
    // previous session's rate-limit, recent-line, or pending-group state behind.
    rateStates_.clear();
    recentLines_.clear();
    pendingGroup_ = {};
    warnings_ = 0;
    errors_ = 0;
    rateLimitedLines_ = 0;
    if (!logPath.parent_path().empty()) {
        std::error_code error;
        std::filesystem::create_directories(logPath.parent_path(), error);
        if (error) {
            OutputDebugStringW((L"Could not create Backtrack log directory: " + logPath.parent_path().wstring() + L"\n").c_str());
            logPath_.clear();  // nothing was opened, so currentPath() must not name a file
            return;
        }
    }
    logPath_ = archivePreviousLog(logPath);
    stream_.open(logPath_, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!stream_.is_open()) {
        OutputDebugStringW((L"Could not open Backtrack log file: " + logPath_.wstring() + L"\n").c_str());
    }
}

void Logger::setMinLevel(LogLevel level) {
    std::scoped_lock lock(mutex_);
    minLevel_ = level;
}

LogLevel Logger::minLevel() const {
    std::scoped_lock lock(mutex_);
    return minLevel_;
}

void Logger::setEnabled(bool enabled) {
    std::scoped_lock lock(mutex_);
    enabled_ = enabled;
}

bool Logger::enabled() const {
    std::scoped_lock lock(mutex_);
    return enabled_;
}

void Logger::writeLine(const std::wstring& text) {
    // Caller holds mutex_.
    std::wstring redacted = text;
    redactSecrets(redacted);
    std::wstring line = redacted;
    while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n')) {
        line.pop_back();
    }
    recentLines_.push_back(std::move(line));
    while (recentLines_.size() > kRecentLineCapacity) {
        recentLines_.pop_front();
    }
    if (stream_.is_open()) {
        // No per-line flush: it holds the global mutex across a disk write, so log
        // spam stalls the capture and encode threads. Lines reach disk when the
        // stream buffer fills, on Logger::flush(), or when the process exits.
        stream_ << wideToUtf8(redacted);
    }
    OutputDebugStringW(redacted.c_str());
}

void Logger::write(LogLevel level, const std::wstring& message) {
    write(level, nullptr, message);
}

void Logger::write(LogLevel level, const wchar_t* subsystem, const std::wstring& message) {
    std::scoped_lock lock(mutex_);
    writeLocked(level, subsystem, message, std::chrono::steady_clock::now());
}

void Logger::writeRateSummary(const std::wstring& message, const RateState& state) {
    if (state.suppressed == 0) {
        return;
    }
    std::wstringstream summary;
    summary << L"[" << timestamp() << L"] [" << levelName(state.level) << L"] [tid="
            << state.threadId << L"]";
    if (!state.subsystem.empty()) {
        summary << L" [" << state.subsystem << L"]";
    }
    summary << L" " << message << L" (rate limited " << state.suppressed
            << L" occurrence" << (state.suppressed == 1 ? L"" : L"s")
            << L" in previous " << std::chrono::duration_cast<std::chrono::seconds>(kRateWindow).count()
            << L"s)\n";
    writeLine(summary.str());
}

void Logger::writeLocked(LogLevel level, const wchar_t* subsystem, const std::wstring& message,
    std::chrono::steady_clock::time_point now) {
    if (!enabled_) {
        return;
    }
    if (level < minLevel_) {
        return;
    }
    if (level == LogLevel::Warning) {
        ++warnings_;
    } else if (level == LogLevel::Error) {
        ++errors_;
    }

    // Consecutive-duplicate grouping runs before rate limiting: a burst of
    // back-to-back identical lines collapses into one "(Nx)" line, and the
    // rate limiter below still bounds the number of such lines per window for
    // interleaved spam that grouping cannot merge. Errors group too; the stack
    // and breadcrumb trail are captured at write time so a group flushed after
    // its task scope ended still carries the original context.
    const std::wstring subsystemStr = subsystem ? subsystem : L"";
    if (pendingGroup_.active && pendingGroup_.level == level &&
        pendingGroup_.subsystem == subsystemStr && pendingGroup_.message == message) {
        ++pendingGroup_.count;
        return;
    }
    flushPendingGroupLocked(now);
    pendingGroup_.level = level;
    pendingGroup_.subsystem = subsystemStr;
    pendingGroup_.message = message;
    pendingGroup_.task = LogContext::current();
    pendingGroup_.trail = level == LogLevel::Error ? LogContext::trail() : std::wstring{};
    pendingGroup_.stack = level == LogLevel::Error ? captureStackTrace() : std::wstring{};
    pendingGroup_.count = 1;
    pendingGroup_.active = true;
}

void Logger::flushPendingGroupLocked(std::chrono::steady_clock::time_point now) {
    if (!pendingGroup_.active) {
        return;
    }
    const PendingGroup group = std::move(pendingGroup_);
    pendingGroup_.active = false;

    // Windowed per-message rate limiting. Errors are never throttled so real
    // failures always surface. Any other level is capped at kMaxPerWindow lines
    // per kRateWindow; excess occurrences are counted and reported as a single
    // summary line when the window rolls over. This bounds interleaved spam
    // (e.g. monitor-switch churn, retry loops) that previously produced tens of
    // thousands of lines, without hiding sporadic events. Keyed on the message
    // text alone, so a grouped burst shares one budget with its ungrouped text.
    constexpr size_t kMaxPerWindow = 3;

    // Gate on level only. A minLevel_ condition here made the default Debug
    // configuration skip throttling entirely, so hot loops were never collapsed.
    const bool throttleable = group.level != LogLevel::Error;

    if (throttleable) {
        // Bound memory: messages with embedded values (PIDs, dimensions) create
        // distinct keys, so prune entries whose window has long since expired.
        // shortcut: 512 is a prune trigger, not a hard cap, so a burst of
        // distinct messages can exceed it until their windows age out; raise it if
        // that ever matters, the map holds one small RateState per live text.
        constexpr size_t kMaxRateEntries = 512;
        if (rateStates_.size() > kMaxRateEntries) {
            for (auto it = rateStates_.begin(); it != rateStates_.end();) {
                if (now - it->second.windowStart >= kRateWindow) {
                    writeRateSummary(it->first, it->second);
                    it = rateStates_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        auto [it, inserted] = rateStates_.try_emplace(group.message);
        auto& state = it->second;
        if (inserted || now - state.windowStart >= kRateWindow) {
            if (!inserted) {
                writeRateSummary(group.message, state);
            }
            state.windowStart = now;
            state.emitted = 0;
            state.suppressed = 0;
        }
        state.level = group.level;
        state.threadId = GetCurrentThreadId();
        state.subsystem = group.subsystem;

        if (state.emitted >= kMaxPerWindow) {
            ++state.suppressed;
            ++rateLimitedLines_;
            return;
        }
        ++state.emitted;
    }

    writeLine(formatGroupLine(group));
}

std::wstring Logger::formatGroupLine(const PendingGroup& group) const {
    std::wstringstream line;
    line << timestamp() << L" [" << levelName(group.level) << L"]";
    if (!group.subsystem.empty()) {
        line << L" [" << group.subsystem << L"]";
    }
    line << L" " << group.message;
    if (group.count > 1) {
        line << L" (" << group.count << L"x)";
    }

    // Structured tail: correlation id from the thread's task context, and on
    // ERROR the breadcrumb trail plus a stack trace.
    std::wstring pairs;
    if (!group.task.empty()) {
        pairs = L"task=" + group.task;
    }
    if (group.level == LogLevel::Error) {
        if (!group.trail.empty()) {
            pairs += pairs.empty() ? L"trail=" + group.trail : L" trail=" + group.trail;
        }
        if (!group.stack.empty()) {
            pairs += pairs.empty() ? L"stack=" + group.stack : L" stack=" + group.stack;
        }
    }
    if (!pairs.empty()) {
        line << L" (" << pairs << L")";
    }
    line << L"\n";
    return line.str();
}

void Logger::writeHeader() {
    std::scoped_lock lock(mutex_);
    if (!enabled_ || !stream_.is_open()) {
        return;
    }
    std::wstringstream line;
    line << L"===== Backtrack " << kCurrentAppVersion << L" =====\n";
    writeLine(line.str());
    line.str(L"");
    line << L"date=" << dateString() << L" environment="
#ifdef NDEBUG
         << L"release"
#else
         << L"debug"
#endif
         << L"\n";
    writeLine(line.str());
    line.str(L"");
    line << L"os=" << osVersionString() << L"\n";
    writeLine(line.str());
}

void Logger::writeFooter() {
    std::scoped_lock lock(mutex_);
    if (!enabled_) {
        return;
    }
    flushPendingGroupLocked(std::chrono::steady_clock::now());
    if (!stream_.is_open()) {
        return;
    }
    std::wstringstream line;
    line << L"===== shutdown: errors=" << errors_ << L" warnings=" << warnings_
         << L" rate-limited=" << rateLimitedLines_ << L" =====\n";
    writeLine(line.str());
    stream_.flush();
}

bool Logger::flush() {
    std::scoped_lock lock(mutex_);
    flushPendingGroupLocked(std::chrono::steady_clock::now());
    if (!stream_.is_open()) {
        return false;
    }
    stream_.flush();
    return static_cast<bool>(stream_);
}

std::filesystem::path Logger::currentPath() const {
    std::scoped_lock lock(mutex_);
    return logPath_;
}

std::vector<std::wstring> Logger::recentLines(size_t maxLines) {
    std::scoped_lock lock(mutex_);
    if (!enabled_ || maxLines == 0) {
        return {};
    }
    // A diagnostics poll must not change what gets logged, so the deferred
    // duplicate group is reported by formatting it here instead of emitting it:
    // emitting would end the group and spend its rate-limit budget. Only
    // flush/writeFooter/export emit.
    size_t count = std::min(maxLines, recentLines_.size());
    if (pendingGroup_.active) {
        count = std::min(count, maxLines - 1);
    }
    std::vector<std::wstring> lines(
        recentLines_.end() - static_cast<std::ptrdiff_t>(count), recentLines_.end());
    if (pendingGroup_.active) {
        lines.push_back(formatGroupLine(pendingGroup_));
    }
    return lines;
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

    std::wstringstream hexStream;
    hexStream << L"0x" << std::uppercase << std::hex << static_cast<unsigned long>(hr);
    const std::wstring hex = hexStream.str();

    std::wstring message = hex;
    if (length > 0 && buffer) {
        std::wstring text(buffer, buffer + length);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) {
            text.pop_back();
        }
        LocalFree(buffer);
        if (!text.empty()) {
            message = text + L" (" + hex + L")";
        }
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
