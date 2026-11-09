#include "core/AppVersion.h"
#include "core/LogContext.h"
#include "core/Logger.h"
#include "ui/DiagnosticsLog.h"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace backtrack {

struct LoggerTestAccess {
    static void writeAt(Logger& logger, std::chrono::steady_clock::time_point now,
        LogLevel level, const wchar_t* subsystem, const std::wstring& message) {
        std::scoped_lock lock(logger.mutex_);
        logger.writeLocked(level, subsystem, message, now);
    }

    static bool hasRateState(Logger& logger, const std::wstring& message) {
        std::scoped_lock lock(logger.mutex_);
        return logger.rateStates_.contains(message);
    }

    static size_t rateStateCount(Logger& logger) {
        std::scoped_lock lock(logger.mutex_);
        return logger.rateStates_.size();
    }

    static void failStream(Logger& logger) {
        std::scoped_lock lock(logger.mutex_);
        logger.stream_.setstate(std::ios::badbit);
    }

    // Emit a deferred duplicate group without a following write, so rate-state
    // accounting is deterministic in tests. `now` is the caller's timeline
    // (the tests drive rate limiting with synthetic time points).
    static void flushPending(Logger& logger, std::chrono::steady_clock::time_point now) {
        std::scoped_lock lock(logger.mutex_);
        logger.flushPendingGroupLocked(now);
    }

    static void reset(Logger& logger) {
        std::scoped_lock lock(logger.mutex_);
        logger.stream_.close();
        logger.logPath_.clear();
        logger.rateStates_.clear();
        logger.recentLines_.clear();
        logger.pendingGroup_ = {};
        logger.warnings_ = 0;
        logger.errors_ = 0;
        logger.rateLimitedLines_ = 0;
        logger.minLevel_ = LogLevel::Debug;
        logger.enabled_ = true;
    }
};

} // namespace backtrack

namespace {

using backtrack::Logger;
using backtrack::LoggerTestAccess;
using backtrack::LogLevel;
using Clock = std::chrono::steady_clock;

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct TestDirectory {
    std::filesystem::path path;

    TestDirectory() {
        static unsigned counter = 0;
        for (;;) {
            path = std::filesystem::temp_directory_path() /
                (L"backtrack-logger-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                    std::to_wstring(counter++));
            std::error_code error;
            if (std::filesystem::create_directory(path, error)) {
                break;
            }
            if (error) {
                throw std::runtime_error("could not create test directory");
            }
        }
    }

    ~TestDirectory() {
        LoggerTestAccess::reset(Logger::instance());
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

struct FileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~FileHandle() {
        if (value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
    }
};

void writeFile(const std::filesystem::path& path, const char* content) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << content;
    expect(static_cast<bool>(stream), "could not write test file");
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    expect(static_cast<bool>(stream), "could not read test file");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::wstring stagingName(unsigned long long offset) {
    return L"BT" + std::to_wstring((GetCurrentProcessId() + offset) % 1000000) + L".TMP";
}

void expectNoStagingFiles(const std::filesystem::path& directory,
    const std::filesystem::path& allowed = {}) {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!allowed.empty() && std::filesystem::equivalent(entry.path(), allowed)) {
            continue;
        }
        const auto name = entry.path().filename().wstring();
        expect(!(name.starts_with(L"BT") && name.ends_with(L".TMP")),
            "an owned staging file was not removed");
    }
}

size_t countLines(const std::vector<std::wstring>& lines, const std::wstring& text) {
    size_t count = 0;
    for (const auto& line : lines) {
        if (line.find(text) != std::wstring::npos) {
            ++count;
        }
    }
    return count;
}

void testExportFlushesAndPreservesSibling() {
    TestDirectory dir;
    auto& logger = Logger::instance();
    logger.initialize(dir.path / L"source.log");
    const auto destination = dir.path / L"export.log";
    const auto sibling = dir.path / L"export.log.tmp";
    const auto collision = dir.path / stagingName(0);
    writeFile(destination, "previous export");
    writeFile(sibling, "unrelated sibling");
    writeFile(collision, "occupied staging name");

    logger.error(L"test", L"immediate export marker");
    const auto error = backtrack::saveDiagnosticsLog(destination, "\nHDR witness\n");
    expect(!error, "export over an existing file failed");
    const auto content = readFile(destination);
    expect(content.find("immediate export marker") != std::string::npos,
        "export missed the immediately preceding buffered error");
    expect(content.find("HDR witness") != std::string::npos, "export missed the diagnostics tail");
    expect(content.find("previous export") == std::string::npos, "destination was not replaced");
    expect(readFile(sibling) == "unrelated sibling", "successful export changed the sibling");
    expect(readFile(collision) == "occupied staging name", "export overwrote an occupied staging name");
    expect(std::filesystem::remove(collision), "could not remove collision fixture");
    expectNoStagingFiles(dir.path);
}

void testDestinationMatchesNextStagingName() {
    TestDirectory dir;
    auto& logger = Logger::instance();
    logger.initialize(dir.path / L"source.log");
    logger.error(L"test", L"reserved destination marker");
    // the first export consumed candidate 0 (collision) and candidate 1.
    const auto destination = dir.path /
        (L"bt" + std::to_wstring((GetCurrentProcessId() + 2ULL) % 1000000) + L".tmp");
    expect(!backtrack::saveDiagnosticsLog(destination, "\ntail\n"),
        "export to a staging-shaped destination failed");
    expect(readFile(destination).find("reserved destination marker") != std::string::npos,
        "staging-shaped destination was not published");
    expectNoStagingFiles(dir.path, destination);
}

void testEmptyAndLargeSnapshots() {
    TestDirectory dir;
    auto& logger = Logger::instance();
    const auto source = dir.path / L"source.log";
    const auto destination = dir.path / L"export.log";
    logger.initialize(source);
    expect(!backtrack::saveDiagnosticsLog(destination, "empty tail"), "empty log export failed");
    expect(readFile(destination) == "empty tail", "empty log export changed the tail");

    logger.error(L"test", L"large begin " + std::wstring(150000, L'x') + L" large end");
    expect(!backtrack::saveDiagnosticsLog(destination, "\nlarge tail\n"), "large log export failed");
    expect(readFile(destination) == readFile(source) + "\nlarge tail\n", "large snapshot was truncated");
    expectNoStagingFiles(dir.path);
}

void testFailedExportPreservesDestinationAndSibling() {
    TestDirectory dir;
    auto& logger = Logger::instance();
    logger.initialize(dir.path / L"source.log");
    logger.error(L"test", L"copy before failed rename");
    const auto destination = dir.path / L"export.log";
    const auto sibling = dir.path / L"export.log.tmp";
    writeFile(destination, "previous export");
    writeFile(sibling, "unrelated sibling");

    FileHandle locked;
    locked.value = CreateFileW(destination.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    expect(locked.value != INVALID_HANDLE_VALUE, "could not lock destination");
    expect(static_cast<bool>(backtrack::saveDiagnosticsLog(destination, "\ntail\n")),
        "export over a locked destination unexpectedly succeeded");
    expect(readFile(destination) == "previous export", "failed export changed the destination");
    expect(readFile(sibling) == "unrelated sibling", "failed export deleted or changed the sibling");
    expectNoStagingFiles(dir.path);
}

void testPublishFailureCleansOnlyOwnedStage() {
    TestDirectory dir;
    auto& logger = Logger::instance();
    logger.initialize(dir.path / L"source.log");
    logger.error(L"test", L"copy before publish failure");
    const auto destination = dir.path / L"export.log";
    const auto sibling = dir.path / L"export.log.tmp";
    expect(std::filesystem::create_directory(destination), "could not create destination directory");
    writeFile(destination / L"inside.txt", "existing directory contents");
    writeFile(sibling, "unrelated sibling");

    expect(static_cast<bool>(backtrack::saveDiagnosticsLog(destination, "\ntail\n")),
        "export over a nonempty directory unexpectedly succeeded");
    expect(readFile(destination / L"inside.txt") == "existing directory contents",
        "failed publish changed the destination directory");
    expect(readFile(sibling) == "unrelated sibling", "failed publish changed the sibling");
    expectNoStagingFiles(dir.path);
}

void testFlushFailureDoesNotReplaceDestination() {
    TestDirectory dir;
    auto& logger = Logger::instance();
    logger.initialize(dir.path / L"source.log");
    logger.error(L"test", L"buffered error");
    const auto destination = dir.path / L"export.log";
    const auto sibling = dir.path / L"export.log.tmp";
    writeFile(destination, "previous export");
    writeFile(sibling, "unrelated sibling");
    LoggerTestAccess::failStream(logger);

    expect(static_cast<bool>(backtrack::saveDiagnosticsLog(destination, "\ntail\n")),
        "export succeeded after logger flush failed");
    expect(readFile(destination) == "previous export", "failed flush replaced the destination");
    expect(readFile(sibling) == "unrelated sibling", "failed flush changed the sibling");
    expectNoStagingFiles(dir.path);
}

void testHardLinksAreNotWrittenThrough() {
    TestDirectory dir;
    auto& logger = Logger::instance();
    logger.initialize(dir.path / L"source.log");
    logger.error(L"test", L"hard link export marker");
    const auto destination = dir.path / L"export.log";
    const auto sibling = dir.path / L"export.log.tmp";
    const auto target = dir.path / L"unrelated.txt";
    writeFile(target, "hard link target");
    expect(CreateHardLinkW(sibling.c_str(), target.c_str(), nullptr) != FALSE,
        "could not create staging sibling hard link");
    expect(CreateHardLinkW(destination.c_str(), target.c_str(), nullptr) != FALSE,
        "could not create destination hard link");

    expect(!backtrack::saveDiagnosticsLog(destination, "\ntail\n"), "hard link export failed");
    expect(readFile(target) == "hard link target", "export wrote through a hard link");
    expect(readFile(sibling) == "hard link target", "export removed the sibling hard link");
    expect(readFile(destination).find("hard link export marker") != std::string::npos,
        "destination hard link was not replaced by the export");
    expect(!std::filesystem::equivalent(destination, target), "destination still refers to the target");
    const auto previousExport = readFile(destination);
    FileHandle locked;
    locked.value = CreateFileW(destination.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    expect(locked.value != INVALID_HANDLE_VALUE, "could not lock destination for hard link test");
    expect(static_cast<bool>(backtrack::saveDiagnosticsLog(destination, "\ntail\n")),
        "hard link export over a locked destination unexpectedly succeeded");
    expect(readFile(destination) == previousExport, "failed export changed the previous export");
    expect(readFile(target) == "hard link target" && readFile(sibling) == "hard link target",
        "failed export changed or removed a linked sibling");
    expectNoStagingFiles(dir.path);
}

void testSymbolicLinksAreNotWrittenThrough() {
    TestDirectory dir;
    auto& logger = Logger::instance();
    logger.initialize(dir.path / L"source.log");
    logger.error(L"test", L"symbolic link export marker");
    const auto destination = dir.path / L"export.log";
    const auto sibling = dir.path / L"export.log.tmp";
    const auto target = dir.path / L"unrelated.txt";
    writeFile(target, "symbolic link target");
    DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    if (!CreateSymbolicLinkW(sibling.c_str(), target.c_str(), flags)) {
        DWORD error = GetLastError();
        if (error == ERROR_INVALID_PARAMETER) {
            flags = 0;
            if (CreateSymbolicLinkW(sibling.c_str(), target.c_str(), flags)) {
                error = ERROR_SUCCESS;
            } else {
                error = GetLastError();
            }
        }
        if (error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION) {
            std::cout << "symbolic link test skipped: unavailable\n";
            return;
        }
        expect(error == ERROR_SUCCESS, "could not create staging sibling symbolic link");
    }
    expect(CreateSymbolicLinkW(destination.c_str(), target.c_str(), flags) != FALSE,
        "could not create destination symbolic link");

    expect(!backtrack::saveDiagnosticsLog(destination, "\ntail\n"), "symbolic link export failed");
    expect(readFile(target) == "symbolic link target", "export wrote through a symbolic link");
    expect(readFile(sibling) == "symbolic link target", "export removed the sibling symbolic link");
    expect(readFile(destination).find("symbolic link export marker") != std::string::npos,
        "destination symbolic link was not replaced by the export");
    expect(!std::filesystem::equivalent(destination, target), "destination still refers to the target");
    expectNoStagingFiles(dir.path);
}

void testLiveLogCannotBeReplaced() {
    TestDirectory dir;
    auto& logger = Logger::instance();
    const auto source = dir.path / L"source.log";
    logger.initialize(source);
    logger.error(L"test", L"before attempted self export");
    expect(static_cast<bool>(backtrack::saveDiagnosticsLog(source, "\ntail\n")),
        "export over the live log unexpectedly succeeded");
    const auto alias = dir.path / L"source-alias.log";
    expect(CreateHardLinkW(alias.c_str(), source.c_str(), nullptr) != FALSE,
        "could not create live log hard link");
    expect(static_cast<bool>(backtrack::saveDiagnosticsLog(alias, "\ntail\n")),
        "export over a live log alias unexpectedly succeeded");
    expect(std::filesystem::equivalent(source, alias), "live log alias was replaced");
    logger.error(L"test", L"after attempted self export");
    logger.flush();
    const auto content = readFile(source);
    expect(content.find("before attempted self export") != std::string::npos &&
        content.find("after attempted self export") != std::string::npos,
        "self export displaced the live log");
    expectNoStagingFiles(dir.path);
}

void testExpiredStatesRetireWithSummaries() {
    TestDirectory dir;
    auto& logger = Logger::instance();
    logger.initialize(dir.path / L"source.log");
    const Clock::time_point start{};
    const auto at = [&](Clock::time_point time, LogLevel level, const wchar_t* subsystem, const std::wstring& message) {
        LoggerTestAccess::writeAt(logger, time, level, subsystem, message);
    };
    // Interleave so repeated keys build rate-limit suppression instead of
    // collapsing into consecutive-duplicate groups.
    for (int i = 0; i < 4; ++i) {
        at(start, LogLevel::Info, L"first", L"expired suppressed A");
        at(start, LogLevel::Info, L"second", L"expired suppressed B");
    }
    at(start, LogLevel::Warning, L"previous", L"expired suppressed A");  // 5th A: suppressed, pins metadata
    at(start, LogLevel::Info, L"test", L"expired unsuppressed");
    for (int i = 0; i < 510; ++i) {
        at(start, LogLevel::Info, L"test", L"filler " + std::to_wstring(i));
    }
    LoggerTestAccess::flushPending(logger, start);
    // A x5 (3 emitted, 2 suppressed), B x4 (3 emitted, 1 suppressed), one
    // unsuppressed, and filler 0..509: 1 + 1 + 1 + 510 = 513 keys.
    expect(LoggerTestAccess::rateStateCount(logger) == 513, "setup did not exceed prune threshold");
    at(start + std::chrono::seconds(9), LogLevel::Info, L"test", L"still active");
    LoggerTestAccess::flushPending(logger, start + std::chrono::seconds(9));
    expect(LoggerTestAccess::rateStateCount(logger) == 514, "pruning retired unexpired states");
    at(start + std::chrono::seconds(10), LogLevel::Info, L"test", L"trigger prune");
    LoggerTestAccess::flushPending(logger, start + std::chrono::seconds(10));

    expect(LoggerTestAccess::rateStateCount(logger) == 2, "expired states were not all retired");
    expect(!LoggerTestAccess::hasRateState(logger, L"expired suppressed A") &&
        !LoggerTestAccess::hasRateState(logger, L"expired suppressed B") &&
        !LoggerTestAccess::hasRateState(logger, L"expired unsuppressed"),
        "expired suppressed or unsuppressed key remains");
    expect(LoggerTestAccess::hasRateState(logger, L"still active"), "unexpired key was retired");
    auto lines = logger.recentLines(200);
    expect(countLines(lines, L"expired suppressed A (rate limited 2 occurrences in previous 10s)") == 1,
        "pruning lost or duplicated the first summary");
    expect(countLines(lines, L"expired suppressed B (rate limited 1 occurrence in previous 10s)") == 1,
        "pruning lost or duplicated the second summary");
    expect(countLines(lines, L"expired unsuppressed (rate limited") == 0,
        "unsuppressed key generated a summary");
    bool previousMetadata = false;
    for (const auto& line : lines) {
        if (line.find(L"expired suppressed A (rate limited") != std::wstring::npos) {
            previousMetadata = line.find(L"[WARN]") != std::wstring::npos &&
                line.find(L"[previous]") != std::wstring::npos;
        }
    }
    expect(previousMetadata, "pruning did not preserve the suppressed window's metadata");

    // A fresh window: a consecutive burst collapses into a duplicate group
    // rather than a suppression summary.
    for (int i = 0; i < 4; ++i) {
        at(start + std::chrono::seconds(10), LogLevel::Info, L"new", L"expired suppressed A");
    }
    at(start + std::chrono::seconds(10), LogLevel::Info, L"new", L"window flush");
    lines = logger.recentLines(200);
    expect(countLines(lines, L"expired suppressed A (rate limited 2 occurrences in previous 10s)") == 1,
        "old suppressed occurrences were counted twice");
    expect(countLines(lines, L"expired suppressed A (4x)") == 1,
        "new-window burst was not grouped");
    expect(countLines(lines, L"expired suppressed A (rate limited 1 occurrence in previous 10s)") == 0,
        "grouped burst produced a suppression summary");
    expect(logger.flush(), "could not flush rate summaries");
    const auto content = readFile(dir.path / L"source.log");
    expect(content.find("expired suppressed A (rate limited 2 occurrences in previous 10s)") != std::string::npos &&
        content.find("expired suppressed B (rate limited 1 occurrence in previous 10s)") != std::string::npos,
        "pruned summaries did not reach the log file");
}

void testPruningTriggerCanBeTheExpiredKey() {
    TestDirectory dir;
    auto& logger = Logger::instance();
    logger.initialize(dir.path / L"source.log");
    const Clock::time_point start{};
    // Interleave "same key" with distinct fillers so it builds suppression
    // instead of a duplicate group, and exceed the prune threshold.
    for (int i = 0; i < 520; ++i) {
        LoggerTestAccess::writeAt(logger, start, LogLevel::Info, L"test", L"filler " + std::to_wstring(i));
        if (i % 4 == 0) {
            LoggerTestAccess::writeAt(logger, start, LogLevel::Info, L"test", L"same key");
        }
    }
    LoggerTestAccess::flushPending(logger, start);
    // Writing the same key again when the map is past the prune threshold
    // retires every expired state and writes the key's previous-window summary.
    LoggerTestAccess::writeAt(logger, start + std::chrono::seconds(10), LogLevel::Info, L"test", L"same key");
    LoggerTestAccess::flushPending(logger, start + std::chrono::seconds(10));
    const auto lines = logger.recentLines(200);
    expect(countLines(lines, L"same key (rate limited") == 1,
        "prune-triggering key lost or duplicated its previous summary");
    expect(LoggerTestAccess::rateStateCount(logger) == 1,
        "prune-triggering key did not get a fresh budget");
}

void testGroupedConsecutiveDuplicates() {
    TestDirectory dir;
    auto& logger = Logger::instance();
    logger.initialize(dir.path / L"source.log");
    for (int i = 0; i < 5; ++i) {
        logger.info(L"test", L"repeated marker");
    }
    logger.info(L"test", L"distinct marker");
    const auto lines = logger.recentLines(200);
    expect(countLines(lines, L"repeated marker (5x)") == 1,
        "burst was not grouped into one (5x) line");
    expect(countLines(lines, L"repeated marker") == 1,
        "burst produced more than one line");
    expect(countLines(lines, L"distinct marker") == 1, "distinct line was lost");
}

void testGroupedErrorsCarryCount() {
    TestDirectory dir;
    auto& logger = Logger::instance();
    logger.initialize(dir.path / L"source.log");
    for (int i = 0; i < 6; ++i) {
        logger.error(L"test", L"hard fault");
    }
    logger.info(L"test", L"after burst");
    const auto lines = logger.recentLines(200);
    expect(countLines(lines, L"hard fault (6x)") == 1,
        "error burst was not grouped into one (6x) line");
}

void testErrorStackTraceAttached() {
    TestDirectory dir;
    auto& logger = Logger::instance();
    logger.initialize(dir.path / L"source.log");
    logger.error(L"test", L"stack witness");
    logger.info(L"test", L"flush trigger");
    const auto lines = logger.recentLines(200);
    bool hasStack = false;
    for (const auto& line : lines) {
        if (line.find(L"stack witness") != std::wstring::npos) {
            hasStack = line.find(L"stack=") != std::wstring::npos;
        }
    }
    expect(hasStack, "error did not carry a stack trace");
}

void testHeaderAndFooter() {
    TestDirectory dir;
    auto& logger = Logger::instance();
    logger.initialize(dir.path / L"source.log");
    logger.writeHeader();
    logger.info(L"test", L"middle marker");
    logger.warning(L"test", L"warn marker");
    logger.error(L"test", L"error marker");
    logger.writeFooter();
    logger.flush();
    const std::string content = readFile(dir.path / L"source.log");
    const std::string expectedHeader = "===== Backtrack " +
        std::string(backtrack::kCurrentAppVersion.begin(), backtrack::kCurrentAppVersion.end()) + " =====";
    expect(content.find(expectedHeader) != std::string::npos, "header missing project version");
    expect(content.find("date=") != std::string::npos, "header missing date");
    expect(content.find("environment=release") != std::string::npos ||
        content.find("environment=debug") != std::string::npos, "header missing environment");
    expect(content.find("shutdown: errors=1 warnings=1") != std::string::npos,
        "footer summary has wrong counts");
}

void testRedactsSecrets() {
    TestDirectory dir;
    auto& logger = Logger::instance();
    logger.initialize(dir.path / L"source.log");
    logger.info(L"test", L"call with token=abcdef123456 and password=hunter2 done");
    logger.info(L"test", L"flush trigger");
    logger.flush();
    const std::string content = readFile(dir.path / L"source.log");
    expect(content.find("token=***") != std::string::npos &&
        content.find("password=***") != std::string::npos, "secrets were not masked");
    expect(content.find("abcdef123456") == std::string::npos &&
        content.find("hunter2") == std::string::npos, "secret value leaked");
}

void testCorrelationTaskLabel() {
    TestDirectory dir;
    auto& logger = Logger::instance();
    logger.initialize(dir.path / L"source.log");
    {
        backtrack::LogContext::ScopedTask task(L"testTask");
        logger.info(L"test", L"inside task");
    }
    logger.info(L"test", L"outside task");
    const auto lines = logger.recentLines(200);
    bool insideHasTask = false;
    bool outsideHasTask = false;
    for (const auto& line : lines) {
        if (line.find(L"inside task") != std::wstring::npos) {
            insideHasTask = line.find(L"task=testTask") != std::wstring::npos;
        }
        if (line.find(L"outside task") != std::wstring::npos) {
            outsideHasTask = line.find(L"task=") == std::wstring::npos;
        }
    }
    expect(insideHasTask, "task label was not attached inside the scope");
    expect(outsideHasTask, "task label leaked outside the scope");
}

void testBreadcrumbTrailOnError() {
    TestDirectory dir;
    auto& logger = Logger::instance();
    logger.initialize(dir.path / L"source.log");
    {
        backtrack::LogContext::ScopedTask task(L"testTask");
        backtrack::LogContext::breadcrumb(L"first step");
        backtrack::LogContext::breadcrumb(L"second step");
        logger.error(L"test", L"boom");
    }
    const auto lines = logger.recentLines(200);
    bool trailAttached = false;
    for (const auto& line : lines) {
        if (line.find(L"boom") != std::wstring::npos) {
            trailAttached = line.find(L"trail=") != std::wstring::npos &&
                line.find(L"first step") != std::wstring::npos &&
                line.find(L"second step") != std::wstring::npos;
        }
    }
    expect(trailAttached, "error did not attach the breadcrumb trail");
}

void testUnarchivableLogIsNotTruncated() {
    TestDirectory dir;
    auto& logger = Logger::instance();
    const auto source = dir.path / L"source.log";
    writeFile(source, "previous session line");

    // Read-only handle without FILE_SHARE_DELETE: the rename to .old fails, so
    // initialize() must archive around the held file instead of truncating it.
    FileHandle locked;
    locked.value = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    expect(locked.value != INVALID_HANDLE_VALUE, "could not lock the previous log");

    logger.initialize(source);
    expect(logger.currentPath() != source, "an unarchivable previous log was reopened for truncation");
    expect(readFile(source) == "previous session line", "the previous session's log was destroyed");
}

void testRecentLinesDoesNotEmitPendingGroup() {
    TestDirectory dir;
    auto& logger = Logger::instance();
    logger.initialize(dir.path / L"source.log");
    for (int i = 0; i < 4; ++i) {
        logger.info(L"test", L"poll marker");
    }
    // A diagnostics poll must report the deferred group without emitting it, so
    // the group stays open and keeps accumulating.
    auto lines = logger.recentLines(200);
    expect(countLines(lines, L"poll marker (4x)") == 1,
        "a diagnostics poll did not report the pending duplicate group");
    expect(LoggerTestAccess::rateStateCount(logger) == 0,
        "a diagnostics poll spent the pending group's rate-limit budget");
    for (int i = 0; i < 3; ++i) {
        logger.info(L"test", L"poll marker");
    }
    logger.info(L"test", L"after poll");
    lines = logger.recentLines(200);
    expect(countLines(lines, L"poll marker (7x)") == 1,
        "a diagnostics poll closed the duplicate group early");
    expect(countLines(lines, L"poll marker") == 1, "the pending group was reported twice");
    expect(logger.flush(), "could not flush the grouped line");
    const auto content = readFile(dir.path / L"source.log");
    expect(content.find("poll marker (7x)") != std::string::npos &&
        content.find("poll marker (4x)") == std::string::npos,
        "the group was emitted by the poll instead of the flush");
}

} // namespace

int main() {
    try {
        testExportFlushesAndPreservesSibling();
        testDestinationMatchesNextStagingName();
        testEmptyAndLargeSnapshots();
        testFailedExportPreservesDestinationAndSibling();
        testPublishFailureCleansOnlyOwnedStage();
        testFlushFailureDoesNotReplaceDestination();
        testHardLinksAreNotWrittenThrough();
        testSymbolicLinksAreNotWrittenThrough();
        testLiveLogCannotBeReplaced();
        testExpiredStatesRetireWithSummaries();
        testPruningTriggerCanBeTheExpiredKey();
        testGroupedConsecutiveDuplicates();
        testGroupedErrorsCarryCount();
        testErrorStackTraceAttached();
        testHeaderAndFooter();
        testRedactsSecrets();
        testCorrelationTaskLabel();
        testBreadcrumbTrailOnError();
        testUnarchivableLogIsNotTruncated();
        testRecentLinesDoesNotEmitPendingGroup();
        std::cout << "LoggerDiagnosticsTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "LoggerDiagnosticsTests failed: " << error.what() << '\n';
        return 1;
    }
}
