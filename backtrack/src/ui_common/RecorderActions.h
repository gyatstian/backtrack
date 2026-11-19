#pragma once

#include "core/Types.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace backtrack {

class RecorderController;

enum class ControllerActionKind {
    Startup,
    ApplySettings,
    ToggleRecording,
    SaveReplay,
    RecoverFailedRecording,
};

struct ControllerAction {
    ControllerActionKind kind = ControllerActionKind::Startup;
    AppSettings settings;
    bool hotkeysOk = true;
    // Queued by the auto record watcher rather than by the user; the
    // completion uses it to decide recording ownership.
    bool automatic = false;
    std::wstring hotkeyError;
    std::wstring replayTag;
};

struct ControllerActionResult {
    ControllerActionKind kind = ControllerActionKind::Startup;
    bool ok = false;
    // Startup/ApplySettings only: false when the hotkeys of that action could
    // not be registered, so the window can keep the warning on screen.
    bool hotkeysOk = true;
    bool automatic = false;
    bool startedRecording = false;
    bool stoppedRecording = false;
    bool savedReplay = false;
    bool recoveredRecording = false;
    bool refreshLibrary = false;
    std::filesystem::path clipPath;
    std::wstring status;
};

struct UpdateCheckTarget {
    std::wstring host;
    uint16_t port;
    DWORD accessType;
    bool secure;
    std::chrono::milliseconds timeout;
};

ControllerActionResult executeRecorderAction(RecorderController& controller, const ControllerAction& action);
std::optional<std::wstring> fetchReleaseVersion(const UpdateCheckTarget& target, const std::atomic<bool>& stopping);
UpdateCheckTarget releaseCheckTarget();

// Release tags are dotted numbers with an optional leading 'v' ("v1.97").
// A tag with no numeric part parses as zero, so an unexpected tag never looks
// newer than the running build.
inline std::vector<uint64_t> versionParts(std::wstring_view text) {
    std::vector<uint64_t> parts;
    if (!text.empty() && (text.front() == L'v' || text.front() == L'V')) text.remove_prefix(1);
    for (;;) {
        uint64_t value = 0;
        size_t used = 0;
        while (used < text.size() && text[used] >= L'0' && text[used] <= L'9') {
            value = value * 10 + static_cast<uint64_t>(text[used] - L'0');
            ++used;
        }
        parts.push_back(value);
        const size_t dot = text.find(L'.');
        if (dot == std::wstring_view::npos) break;
        text.remove_prefix(dot + 1);
    }
    return parts;
}

inline bool isNewerVersion(std::wstring_view remote, std::wstring_view current) {
    const auto newer = versionParts(remote), running = versionParts(current);
    const size_t count = newer.size() > running.size() ? newer.size() : running.size();
    for (size_t i = 0; i < count; ++i) {
        const uint64_t left = i < newer.size() ? newer[i] : 0;
        const uint64_t right = i < running.size() ? running[i] : 0;
        if (left != right) return left > right;
    }
    return false;
}

// Who owns the current recording, and whether a user override blocks the next
// auto start. The decision is pure so the sequences that used to end auto
// record silently are testable without a window.
struct AutoRecordOwnership {
    bool owns = false;
    bool suppressStart = false;
};

// ToggleRecording completion: only an automatic start takes ownership, an
// automatic stop is not a user override, and a manual stop suppresses the
// restart while the selected app stays open.
inline AutoRecordOwnership reconcileAutoRecordOwnership(
    AutoRecordOwnership current, bool automatic, bool startedRecording, bool stoppedRecording,
    bool appRunning) {
    if (automatic) {
        return {startedRecording, stoppedRecording ? false : current.suppressStart};
    }
    return {false, stoppedRecording && appRunning};
}

// ApplySettings/Startup completion: a pipeline restart ends an auto-owned
// recording without a ToggleRecording completion, so drop ownership unsuppressed
// and let the next poll start it again.
inline AutoRecordOwnership reconcileAutoRecordAfterSettings(AutoRecordOwnership current, bool recording) {
    return current.owns && !recording ? AutoRecordOwnership{} : current;
}

} // namespace backtrack
