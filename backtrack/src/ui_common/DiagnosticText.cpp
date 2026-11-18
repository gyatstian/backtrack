#include "ui_common/DiagnosticText.h"
#include "ui_common/DiagnosticsLog.h"
#include "ui_common/WindowTools.h"

#include "app/RecorderController.h"
#include "core/AppVersion.h"
#include "core/Logger.h"
#include "platform/Win32Util.h"
#include "settings/SettingsStore.h"

#include <commdlg.h>
#include <iomanip>
#include <sstream>

namespace backtrack {

// hdrRejected covers two cases; split them here so a WGC washout never reads
// as a driver refusal. stats() fields stay as-is. Backend + format + flip
// count ride the same line so the HDR friend self-serves without a new row.
// Format names mirror app's hdrFormatName; both stay local so the platform
// contract stays limited to the backend display name.
static std::wstring hdrFormatName(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return L"R10G10B10A2";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return L"scRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return L"BGRA";
    default:
        return L"unknown";
    }
}

static std::wstring activeBackendName(const RecordingStats& stats) {
    return stats.captureBackendActive ? captureBackendDisplayName(stats.activeCaptureBackend) : L"inactive";
}

static std::wstring hdrStatusLine(const RecordingStats& stats) {
    const std::wstring detail =
        std::wstring(L", ") + captureBackendDisplayName(stats.activeCaptureBackend) +
        L", " + hdrFormatName(stats.hdrFormat) +
        L", flips " + std::to_wstring(stats.hdrFlipCount);
    if (stats.hdrActive && stats.hdrRejected) {
        return L"HDR: rejected (tone-mapping refused" + detail + L")";
    }
    if (stats.hdrActive) {
        return L"HDR: active (tone-mapping" + detail + L")";
    }
    if (stats.hdrRejected) {
        return L"HDR: SDR on HDR monitor (WGC/DWM" + detail + L")";
    }
    return L"HDR: SDR";
}

std::wstring exportDiagnostics(HWND window, RecorderController& controller) {
    const auto source = Logger::instance().currentPath();
    if (source.empty()) {
        return L"No log file available to save";
    }

    std::error_code existsError;
    if (!std::filesystem::exists(source, existsError) || existsError) {
        return L"Log file not found";
    }

    wchar_t fileName[MAX_PATH]{};
    wcscpy_s(fileName, L"backtrack-log.txt");

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window;
    dialog.lpstrFilter = L"Log files (*.txt;*.log)\0*.txt;*.log\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = fileName;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrTitle = L"Save log copy";
    dialog.lpstrDefExt = L"txt";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetSaveFileNameW(&dialog)) {
        return {};
    }

    // Bug-report attachment: carry the HDR witness with the log (same line
    // updateStats renders).
    const RecordingStats tailStats = controller.stats();
    std::wstringstream tail;
    tail << L"\nActive backend: " << activeBackendName(tailStats);
    tail << L"\n" << hdrStatusLine(tailStats) << L"\n";
    // Runtime snapshot: the counters that matter when triaging a log. It is
    // appended at save time, so it reflects the session so far, not shutdown.
    tail << L"Runtime:\n";
    tail << L"Average encode: " << tailStats.encoder.averageEncodeMs << L" ms\n";
    tail << L"Encoder submissions/encoded/keyframes: " << tailStats.encoder.submittedFrames << L"/"
         << tailStats.encoder.encodedFrames << L"/" << tailStats.encoder.keyFrames << L"\n";
    tail << L"Frames dropped: " << tailStats.droppedFrames + tailStats.encoder.droppedFrames
         << L" (gpu-protection " << tailStats.gpuProtectionDrops
         << L", frame-queue " << tailStats.frameQueueDrops << L")\n";
    tail << L"Source frames: " << tailStats.sourceFrames << L" (" << tailStats.sourceFramesPerSecond
         << L"/s), timeline intervals: " << tailStats.capturedFrames << L" ("
         << tailStats.timelineIntervalsPerSecond << L"/s)\n";
    tail << L"Cadence dup / catch-up dup / coalesced idle: " << tailStats.cadenceDuplicateFrames << L"/"
         << tailStats.catchUpDuplicateFrames << L"/" << tailStats.coalescedIdleIntervals << L"\n";
    tail << L"Replay packets: " << tailStats.replayVideoPackets << L", keyframes: "
         << tailStats.replayKeyFrames << L"\n";
    tail << L"System audio queue drops: " << tailStats.systemAudioQueueDrops
         << L", mic queue drops: " << tailStats.microphoneAudioQueueDrops << L"\n";
    tail << L"Frame queue depth: " << tailStats.encoder.queueDepth << L"\n";
    if (const auto error = saveDiagnosticsLog(fileName, wideToUtf8(tail.str())); error) {
        return L"Could not save log: " + utf8ToWide(error.message());
    }
    return std::wstring(L"Log saved to ") + fileName;
}

std::array<std::wstring, 3> diagnosticText(RecorderController& controller, const RecordingStats& stats,
    const VoiceCommandStats& voiceStats, const LeagueIntegrationStats& leagueStats) {
    std::array<std::wstring, 3> text;
    {
        std::wstringstream stream;
        // Build identity first: a diagnostics screenshot or "Save Log" copy is
        // useless for triage without it.
        stream << L"Backtrack " << kCurrentAppVersion << L"\r\n";
        stream << L"Recording: " << (stats.recording ? L"yes" : L"no") << L"\r\n";
        stream << L"Replay buffer: " << (stats.replayEnabled ? L"enabled" : L"disabled") << L"\r\n";
        stream << L"Selected backend: " << captureBackendDisplayName(stats.selectedCaptureBackend) << L"\r\n";
        stream << L"Active backend: " << activeBackendName(stats);
        if (stats.captureBackendFallbackUsed) {
            stream << L" (fallback)";
        }
        stream << L"\r\n";
        if (!stats.captureBackendStatus.empty()) {
            stream << L"Backend status: " << stats.captureBackendStatus << L"\r\n";
        }
        stream << L"Capture size: " << stats.captureWidth << L"x" << stats.captureHeight << L"\r\n";
        stream << L"Encode size: " << stats.encodeWidth << L"x" << stats.encodeHeight << L"\r\n";
        stream << hdrStatusLine(stats) << L"\r\n";
        stream << L"Timeline intervals: " << stats.capturedFrames << L"\r\n";
        stream << L"New source frames: " << stats.sourceFrames << L"\r\n";
        stream << L"Source frames/sec: " << stats.sourceFramesPerSecond << L"\r\n";
        stream << L"Timeline intervals/sec: " << stats.timelineIntervalsPerSecond << L"\r\n";
        stream << L"Cadence duplicates: " << stats.cadenceDuplicateFrames << L"\r\n";
        stream << L"Catch-up duplicates: " << stats.catchUpDuplicateFrames << L"\r\n";
        stream << L"Coalesced idle intervals: " << stats.coalescedIdleIntervals << L"\r\n";
        stream << L"Dropped frames: " << stats.droppedFrames + stats.encoder.droppedFrames << L"\r\n";
        stream << L"GPU protection drops: " << stats.gpuProtectionDrops << L"\r\n";
        stream << L"Frame queue drops: " << stats.frameQueueDrops << L"\r\n";
        stream << L"Cursor-only desktop frames: " << stats.cursorOnlyFrames << L"\r\n";
        stream << L"System audio queue drops: " << stats.systemAudioQueueDrops << L"\r\n";
        stream << L"Microphone audio queue drops: " << stats.microphoneAudioQueueDrops << L"\r\n";
        stream << L"System audio input: "
               << (stats.systemAudioActive ? L"running" : L"not running");
        if (stats.systemAudioActive) {
            stream << L", last packet " << (stats.systemAudioSilenceMs / 1000) << L"."
                   << std::setw(3) << std::setfill(L'0') << (stats.systemAudioSilenceMs % 1000) << L"s ago";
        }
        stream << L"\r\n";
        stream << L"Microphone input: "
               << (stats.microphoneAudioActive ? L"running" : L"not running");
        if (stats.microphoneAudioActive) {
            stream << L", last packet " << (stats.microphoneSilenceMs / 1000) << L"."
                   << std::setw(3) << std::setfill(L'0') << (stats.microphoneSilenceMs % 1000) << L"s ago";
        }
        stream << L"\r\n";
        stream << L"Frame queue depth: " << stats.encoder.queueDepth << L"\r\n";
        stream << L"Encoder submissions: " << stats.encoder.submittedFrames << L"\r\n";
        stream << L"Encoded frames: " << stats.encoder.encodedFrames << L"\r\n";
        stream << L"Encoded keyframes: " << stats.encoder.keyFrames << L"\r\n";
        stream << L"Replay packets: " << stats.replayVideoPackets << L"\r\n";
        stream << L"Replay keyframes: " << stats.replayKeyFrames << L"\r\n";
        stream << L"Voice command: " << voiceStats.status << L"\r\n";
        stream << L"Voice accepted/rejected: " << voiceStats.accepted << L"/" << voiceStats.rejected << L"\r\n";
        stream << L"League live client: " << (leagueStats.gameAvailable ? L"available" : L"not detected") << L"\r\n";
        if (!leagueStats.lastError.empty()) {
            stream << L"League last error: " << leagueStats.lastError << L"\r\n";
        }
        if (const std::wstring saveFailure = trimText(controller.lastRecordingError()); !saveFailure.empty()) {
            stream << L"Last save failure: " << saveFailure << L"\r\n";
        }
        // The disk-ring checkboxes stay checked when a ring cannot be used, so
        // this line is the only place the user learns replay video/audio is
        // really sitting in RAM.
        if (const std::wstring storageFallback = trimText(controller.replayStorageFallback()); !storageFallback.empty()) {
            stream << L"Replay storage: " << storageFallback << L"\r\n";
        }
        stream << L"Average encode: " << stats.encoder.averageEncodeMs << L" ms";
        text[0] = stream.str();
    }

    {
        const auto caps = controller.encoderCapabilities();
        std::wstringstream stream;
        stream << L"Adapter: " << caps.adapterName << L"\r\n";
        const std::wstring backendName = caps.backendName.empty() ? L"Hardware encoder" : caps.backendName;
        stream << backendName << L": " << (caps.available ? L"available" : L"unavailable") << L"\r\n";
        stream << L"H.264: " << (caps.h264 ? L"yes" : L"no") << L"\r\n";
        stream << L"HEVC: " << (caps.hevc ? L"yes" : L"no") << L"\r\n";
        stream << L"Max size: " << caps.maxWidth << L"x" << caps.maxHeight << L"\r\n";
        stream << L"Profile: " << encoderProfileName(caps.effective.profile) << L"\r\n";
        stream << L"Preset/mode: " << encoderPresetName(caps.effective.preset)
               << L" / " << encoderModeName(caps.effective.mode) << L"\r\n";
        stream << L"Lookahead: " << yesNo(caps.effective.lookahead)
               << L" depth " << caps.effective.lookaheadDepth << L"\r\n";
        stream << L"Spatial AQ: " << yesNo(caps.effective.spatialAQ)
               << L" strength " << caps.effective.aqStrength << L"\r\n";
        stream << L"Temporal AQ: " << yesNo(caps.effective.temporalAQ) << L"\r\n";
        stream << L"Multipass: " << encoderMultipassName(caps.effective.multipass) << L"\r\n";
        stream << L"B-frames: " << yesNo(caps.effective.bFrames)
               << L", adaptive B: " << yesNo(caps.effective.adaptiveBFrames)
               << L", adaptive I: " << yesNo(caps.effective.adaptiveIFrames) << L"\r\n";
        stream << L"Reference frames: " << caps.effective.referenceFrames
               << L" (multiple refs: " << yesNo(caps.multipleReferenceFrames) << L")\r\n";
        stream << L"Zero reorder delay: " << yesNo(caps.effective.zeroReorderDelay) << L"\r\n";
        stream << caps.detail;
        text[1] = stream.str();
    }

    {
        if (!Logger::instance().enabled()) {
            text[2] = L"Logging is disabled";
        } else {
            const auto lines = Logger::instance().recentLines(12);
            std::wstringstream stream;
            if (lines.empty()) {
                stream << L"No log lines available";
            } else {
                // Collapse runs of identical consecutive lines into "line (Nx)".
                // Compare message after structured timestamp/level/thread/tag fields.
                auto messageKey = [](const std::wstring& line) -> std::wstring {
                    size_t pos = 0;
                    while (pos < line.size() && line[pos] == L'[') {
                        const size_t close = line.find(L']', pos + 1);
                        if (close == std::wstring::npos) {
                            return line;
                        }
                        pos = close + 1;
                        while (pos < line.size() && line[pos] == L' ') {
                            ++pos;
                        }
                    }
                    return line.substr(pos);
                };
                for (size_t i = 0; i < lines.size();) {
                    const std::wstring key = messageKey(lines[i]);
                    size_t count = 1;
                    while (i + count < lines.size() && messageKey(lines[i + count]) == key) {
                        ++count;
                    }
                    stream << lines[i];
                    if (count > 1) {
                        stream << L" (" << count << L"x)";
                    }
                    stream << L"\r\n";
                    i += count;
                }
            }
            text[2] = stream.str();
        }
    }

    return text;
}

std::wstring audioLivenessWarning(const RecordingStats& stats) {
    // Live audio-health warning for the status bar. Edge-triggered: the text
    // only changes (and the status bar only repaints) on a transition, so a
    // permanently dead input does not spam. Only enabled inputs are checked,
    // and only while recording - a silent input between recordings is not a
    // problem worth alarming about.
    std::wstring warning;
    if (stats.recording) {
        constexpr uint64_t kAudioStallWarnMs = 2000;
        const auto describe = [&](bool active, uint64_t silenceMs, const wchar_t* label) -> std::wstring {
            if (!active || silenceMs < kAudioStallWarnMs) {
                return {};
            }
            std::wstringstream stream;
            stream << label << L": no audio for " << (silenceMs / 1000)
                   << L"s \u2014 recording may be video-only";
            return stream.str();
        };
        warning = describe(stats.systemAudioActive, stats.systemAudioSilenceMs, L"System audio");
        const std::wstring mic = describe(stats.microphoneAudioActive, stats.microphoneSilenceMs, L"Microphone");
        if (!mic.empty()) {
            warning = warning.empty() ? mic : warning + L"  \u00b7  " + mic;
        }
    }
    return warning;
}

} // namespace backtrack
