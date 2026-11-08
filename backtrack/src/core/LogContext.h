#pragma once

#include <chrono>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace backtrack {

// Thread-local logging context for the active task on this thread: a
// correlation label plus a bounded breadcrumb flight recorder. C++ has no
// async/await, so the task context is the thread (each Win32 worker runs one
// task at a time); this is the equivalent of AsyncLocalStorage without
// polluting function signatures. The logger appends `task=<label>` to every
// line written from a labeled thread and attaches the breadcrumb trail to the
// next ERROR.
//
// Header-only: the thread_local statics live in inline functions, so suites
// that link core/Logger.cpp need no extra object file.
class LogContext {
public:
    // Start/stop a task label on the calling thread. Contexts nest; the ring
    // is cleared only when the outermost context starts and ends, so an inner
    // task never wipes an outer task's trail.
    static void begin(const std::wstring& label);
    static void end();

    // Innermost active label, empty when no task is active.
    static const std::wstring& current();

    // Flight recorder: record a silent state transition for the active task.
    // Kept in a bounded ring; the trail is attached to the next ERROR from
    // this thread and discarded on task completion.
    static void breadcrumb(const std::wstring& text);

    // Relative-ms trail of the active task's breadcrumbs, empty when none.
    static std::wstring trail();

    // RAII task scope: begin on construction, end on destruction.
    class ScopedTask {
    public:
        explicit ScopedTask(std::wstring label) { LogContext::begin(std::move(label)); }
        ~ScopedTask() { LogContext::end(); }
    };

private:
    static std::vector<std::wstring>& stack();
    static std::deque<std::pair<std::chrono::steady_clock::time_point, std::wstring>>& ring();
};

inline std::vector<std::wstring>& LogContext::stack() {
    static thread_local std::vector<std::wstring> instance;
    return instance;
}

inline std::deque<std::pair<std::chrono::steady_clock::time_point, std::wstring>>& LogContext::ring() {
    static thread_local std::deque<std::pair<std::chrono::steady_clock::time_point, std::wstring>> instance;
    return instance;
}

inline void LogContext::begin(const std::wstring& label) {
    auto& current = stack();
    if (current.empty()) {
        ring().clear();
    }
    current.push_back(label);
}

inline void LogContext::end() {
    auto& current = stack();
    if (!current.empty()) {
        current.pop_back();
    }
    if (current.empty()) {
        ring().clear();
    }
}

inline const std::wstring& LogContext::current() {
    static const std::wstring kEmpty;
    auto& current = stack();
    return current.empty() ? kEmpty : current.back();
}

inline void LogContext::breadcrumb(const std::wstring& text) {
    auto& trail = ring();
    constexpr size_t kMaxBreadcrumbs = 25;
    trail.emplace_back(std::chrono::steady_clock::now(), text);
    while (trail.size() > kMaxBreadcrumbs) {
        trail.pop_front();
    }
}

inline std::wstring LogContext::trail() {
    auto& trail = ring();
    if (trail.empty()) {
        return {};
    }
    std::wstring result;
    for (const auto& entry : trail) {
        if (!result.empty()) {
            result += L"; ";
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            entry.first - trail.front().first).count();
        result += std::to_wstring(ms) + L"ms " + entry.second;
    }
    return result;
}

} // namespace backtrack
