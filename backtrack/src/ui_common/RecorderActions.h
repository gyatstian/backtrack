#pragma once

#include "core/Types.h"

#include <atomic>
#include <optional>

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
