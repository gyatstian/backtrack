#include "ui/MainWindow.h"
#include "ui/UiHelpers.h"
#include "ui/UiIds.h"

#include "audio/WasapiCapture.h"
#include "core/Logger.h"
#include "platform/Win32Util.h"
#include "resource.h"
#include "settings/SettingsStore.h"

#include <CommCtrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace backtrack {
void MainWindow::saveLog() {
    const auto source = Logger::instance().currentPath();
    if (source.empty()) {
        setStatus(L"No log file available to save");
        return;
    }

    std::error_code existsError;
    if (!std::filesystem::exists(source, existsError) || existsError) {
        setStatus(L"Log file not found");
        return;
    }

    wchar_t fileName[MAX_PATH]{};
    wcscpy_s(fileName, L"backtrack-log.txt");

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"Log files (*.txt;*.log)\0*.txt;*.log\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = fileName;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrTitle = L"Save log copy";
    dialog.lpstrDefExt = L"txt";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetSaveFileNameW(&dialog)) {
        return;
    }

    std::error_code copyError;
    std::filesystem::copy_file(
        source,
        std::filesystem::path(fileName),
        std::filesystem::copy_options::overwrite_existing,
        copyError);
    if (copyError) {
        setStatus(L"Could not save log: " + utf8ToWide(copyError.message()));
        return;
    }
    setStatus(std::wstring(L"Log saved to ") + fileName);
}

void MainWindow::updateStats() {
    const auto stats = controller_.stats();
    if (statsLabel_) {
        std::wstringstream stream;
        stream << L"Recording: " << (stats.recording ? L"yes" : L"no") << L"\r\n";
        stream << L"Replay buffer: " << (stats.replayEnabled ? L"enabled" : L"disabled") << L"\r\n";
        stream << L"Selected backend: " << captureBackendDisplayName(stats.selectedCaptureBackend) << L"\r\n";
        stream << L"Active backend: ";
        if (stats.captureBackendActive) {
            stream << captureBackendDisplayName(stats.activeCaptureBackend);
        } else {
            stream << L"inactive";
        }
        if (stats.captureBackendFallbackUsed) {
            stream << L" (fallback)";
        }
        stream << L"\r\n";
        if (!stats.captureBackendStatus.empty()) {
            stream << L"Backend status: " << stats.captureBackendStatus << L"\r\n";
        }
        stream << L"Capture size: " << stats.captureWidth << L"x" << stats.captureHeight << L"\r\n";
        stream << L"Encode size: " << stats.encodeWidth << L"x" << stats.encodeHeight << L"\r\n";
        stream << L"Timeline intervals: " << stats.capturedFrames << L"\r\n";
        stream << L"New source frames: " << stats.sourceFrames << L"\r\n";
        stream << L"Cadence duplicates: " << stats.cadenceDuplicateFrames << L"\r\n";
        stream << L"Catch-up duplicates: " << stats.catchUpDuplicateFrames << L"\r\n";
        stream << L"Coalesced idle intervals: " << stats.coalescedIdleIntervals << L"\r\n";
        stream << L"Dropped frames: " << stats.droppedFrames + stats.encoder.droppedFrames << L"\r\n";
        stream << L"GPU protection drops: " << stats.gpuProtectionDrops << L"\r\n";
        stream << L"System audio queue drops: " << stats.systemAudioQueueDrops << L"\r\n";
        stream << L"Microphone audio queue drops: " << stats.microphoneAudioQueueDrops << L"\r\n";
        stream << L"Frame queue depth: " << stats.encoder.queueDepth << L"\r\n";
        stream << L"Encoder submissions: " << stats.encoder.submittedFrames << L"\r\n";
        stream << L"Encoded frames: " << stats.encoder.encodedFrames << L"\r\n";
        stream << L"Encoded keyframes: " << stats.encoder.keyFrames << L"\r\n";
        stream << L"Replay packets: " << stats.replayVideoPackets << L"\r\n";
        stream << L"Replay keyframes: " << stats.replayKeyFrames << L"\r\n";
        stream << L"Average encode: " << stats.encoder.averageEncodeMs << L" ms";
        setText(statsLabel_, stream.str());
    }

    if (capsLabel_) {
        const auto caps = controller_.encoderCapabilities();
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
        setText(capsLabel_, stream.str());
    }

    if (diagnosticLogLabel_) {
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
        setText(diagnosticLogLabel_, stream.str());
    }
}

} // namespace backtrack
