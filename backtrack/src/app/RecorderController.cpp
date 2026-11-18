#include "app/AudioDrain.h"
#include "app/AudioWriter.h"
#include "app/CaptureFrameHandoff.h"
#include "app/PendingDurationTracker.h"
#include "app/RecorderController.h"
#include "app/VideoDrain.h"
#include "app/VideoTimelineScheduler.h"

#include "audio/AudioConvert.h"
#include "audio/SessionMuteStore.h"
#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
#include "audio/RnnoiseDenoiser.h"
#endif
#include "capture/DesktopDuplicationCapture.h"
#include "capture/GpuPoolBuilder.h"
#include "capture/WgcCaptureSource.h"
#include "clips/ClipStorage.h"
#include "core/Clock.h"
#include "core/LogContext.h"
#include "core/LogTiming.h"
#include "core/Logger.h"
#include "mux/MuxedInputs.h"
#include "mux/RecoveryStore.h"
#include "platform/Win32Util.h"
#include "settings/SettingsStore.h"

#include <mmreg.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <cstring>
#include <limits>
#include <thread>
#include <unordered_set>
#include <utility>

namespace backtrack {

namespace {

// How long a pipeline worker may take to reach a checkpoint that the caller can
// observe, before the caller gives up on it and carries on. Applies to waits that
// have an abort path (the encode-loop discard barrier). The stop path's
// thread joins deliberately do NOT use it: std::thread::join has no timed form and
// abandoning a wedged vendor call would destroy the user's clip.
constexpr auto kGpuPipelineStopTimeout = std::chrono::seconds(30);

uint32_t captureTexturePoolSize(const GpuOptimizationSettings& settings) {
    // Single clamp owned by GpuPoolBuilder; +4 covers the capture loop's
    // lastFrame pin (keeps one scaler/capture slot in use).
    return GpuPoolBuilder::normalizePoolSize(settings.frameQueueLimit + 4);
}

// Wall time since the last user input (keyboard/mouse). Used to tell a
// legitimately idle desktop (DWM presents nothing, so WGC delivers no frames)
// from a wedged capture source. On failure, assume active so recovery still
// runs. GetTickCount64's low 32 bits wrap like LASTINPUTINFO.dwTime, so the
// unsigned delta is correct across the 49-day boundary.
uint64_t userInputIdleMs() {
    LASTINPUTINFO inputInfo{};
    inputInfo.cbSize = sizeof(inputInfo);
    if (!GetLastInputInfo(&inputInfo)) {
        return 0;
    }
    return static_cast<DWORD>(::GetTickCount64()) - inputInfo.dwTime;
}

// Short HDR surface name for logs/Diagnostics. Local (not platform-shared):
// only app logging and the ui HDR line need it, and the platform contract
// stays limited to the backend display name.
const wchar_t* hdrFormatName(DXGI_FORMAT format) {
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

CaptureTarget captureTargetForSettings(const AppSettings& settings) {
    CaptureTarget target;
    target.monitorIndex = settings.monitorIndex;
    if (settings.multiMonitorSupport) {
        target.monitor = settings.followMouseMonitor
            ? cursorMonitorOrFallback(settings.monitorIndex)
            : focusedMonitorOrFallback(settings.monitorIndex);
    } else {
        target.monitor = monitorFromIndex(settings.monitorIndex);
    }
    return target;
}

HMONITOR monitorForCaptureTarget(const CaptureTarget& target) {
    return target.monitor ? target.monitor : monitorFromIndex(target.monitorIndex);
}

bool foregroundWindowCoversMonitor(HMONITOR monitor) {
    const HWND foreground = GetForegroundWindow();
    if (!foreground || !monitor || IsIconic(foreground) || !IsWindowVisible(foreground)) {
        return false;
    }

    // A monitor-sized borderless window is only an exclusive candidate. Actual
    // loss needs a sustained capture stall below; this also excludes desktop,
    // tool, and child windows that happen to cover a monitor.
    if (GetWindow(foreground, GW_OWNER) ||
        (GetWindowLongPtrW(foreground, GWL_STYLE) & WS_CHILD) != 0) {
        return false;
    }

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    RECT windowRect{};
    return GetMonitorInfoW(monitor, &monitorInfo) && GetWindowRect(foreground, &windowRect) &&
           windowRect.left <= monitorInfo.rcMonitor.left &&
           windowRect.top <= monitorInfo.rcMonitor.top &&
           windowRect.right >= monitorInfo.rcMonitor.right &&
           windowRect.bottom >= monitorInfo.rcMonitor.bottom;
}

size_t frameQueueCapacityFor(const GpuOptimizationSettings& settings) {
    // The ceiling here is the frameQueue_ ring storage (RecorderController's
    // constructor), which is fixed for the queue's lifetime.
    return static_cast<size_t>(std::clamp<uint32_t>(settings.frameQueueLimit, 1, 16));
}

// On-disk ring capacity for disk-backed replay buffering: one replay window
// at the configured bitrate plus ~33% VBR headroom, clamped to a sane range.
// ReplayBuffer ignores this unless replay.diskBackedVideo is enabled.
uint64_t replayRingCapacityBytesFor(const AppSettings& settings) {
    constexpr uint64_t kMinCapacityBytes = 64ull * 1024 * 1024;
    constexpr uint64_t kMaxCapacityBytes = 16ull * 1024 * 1024 * 1024;
    const uint64_t windowSeconds = std::max<uint32_t>(1, settings.replay.seconds);
    const uint64_t bytesPerWindow =
        (static_cast<uint64_t>(settings.video.bitrateKbps) * 1000ull / 8ull) * windowSeconds;
    const uint64_t withHeadroom = bytesPerWindow + bytesPerWindow / 3;
    return std::clamp<uint64_t>(withHeadroom, kMinCapacityBytes, kMaxCapacityBytes);
}

// Audio ring capacity: one replay window of two PCM tracks (system +
// microphone) at 48 kHz stereo 16-bit, doubled for headroom. Ignored unless
// replay.diskBackedAudio is enabled.
uint64_t replayAudioRingCapacityBytesFor(const AppSettings& settings) {
    constexpr uint64_t kMinAudioCapacityBytes = 1024ull * 1024;
    constexpr uint64_t kMaxAudioCapacityBytes = 4ull * 1024 * 1024 * 1024;
    constexpr uint64_t kBytesPerSecond = 768000;
    const uint64_t windowSeconds = std::max<uint32_t>(1, settings.replay.seconds);
    return std::clamp<uint64_t>(windowSeconds * kBytesPerSecond, kMinAudioCapacityBytes, kMaxAudioCapacityBytes);
}

// Ring storage location: explicit override when set, otherwise next to the
// clip library. Keeping this resolution here means ReplayBuffer stays agnostic
// of where the user wants the file.
std::filesystem::path replayRingDirectoryFor(const AppSettings& settings) {
    return settings.replayRingDirectory.empty() ? settings.clipDirectory : settings.replayRingDirectory;
}

std::vector<std::wstring> mutedSoundSeparationExecutableKeys(const AppSettings& settings) {
    std::vector<std::wstring> keys;
    if (!settings.soundSeparationEnabled || !settings.captureSystemAudio) {
        return keys;
    }
    for (const auto& app : settings.soundSeparationApps) {
        if (!app.muted || app.executablePath.empty()) {
            continue;
        }
        const std::wstring key = normalizedPathKey(app.executablePath);
        if (!key.empty()) {
            keys.push_back(key);
        }
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

void restoreSessionMutes(const wchar_t* context) {
    const SessionMuteRestoreResult restore = WasapiCapture::clearSessionMutes();
    if (restore.unresolved > 0 || restore.journalFailed) {
        Logger::instance().warning(L"recorder",
            L"Sound separation could not restore " + std::to_wstring(restore.unresolved) +
            L" audio session(s); the journal keeps them " + context);
    }
}

} // namespace

RecorderController::RecorderController()
    // Storage ceiling matches frameQueueCapacityFor's 16 clamp: the ring is
    // fixed for the queue's lifetime, so resetCapacity can never request more
    // than the constructor sized it for.
    : frameQueue_(16),
      systemAudioQueue_(256),
      microphoneAudioQueue_(256),
      microphoneAgc_(std::make_unique<MicrophoneAgc>())
#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
      , microphoneDenoiser_(std::make_unique<RnnoiseDenoiser>()) {
    if (!microphoneDenoiser_->valid()) {
        // Allocation failure only; the feature stays disabled for this session.
        Logger::instance().warning(L"recorder", L"Microphone noise suppression unavailable (RNNoise init failed)");
    }
#else
    {
#endif
    frameEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    audioEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

RecorderController::~RecorderController() {
    shutdown();
    // Scope-guard for global system state: sound-separation mutes must be
    // restored on every destruction path, including ones where stopPipeline
    // early-outs (pipeline never reached running state after mutes were
    // applied). No-op when nothing is muted.
    restoreSessionMutes(L"for the next launch");
    if (frameEvent_) {
        CloseHandle(frameEvent_);
        frameEvent_ = nullptr;
    }
    if (audioEvent_) {
        CloseHandle(audioEvent_);
        audioEvent_ = nullptr;
    }
}

void RecorderController::publishLiveMirrors(const AppSettings& settings) {
    // Called wherever settings_ is assigned so the lock-free readers
    // (stats(), the stall watchdog, the capture-loop cursor poll, the audio
    // writer volumes) never drift from the sanitized source of truth.
    replayEnabledMirror_.store(settings.replay.enabled, std::memory_order_relaxed);
    videoGopSecondsMirror_.store(settings.video.gopSeconds, std::memory_order_relaxed);
    captureCursorMirror_.store(settings.captureCursor, std::memory_order_relaxed);
    captureSystemAudioMirror_.store(settings.captureSystemAudio, std::memory_order_relaxed);
    captureMicrophoneMirror_.store(settings.captureMicrophone, std::memory_order_relaxed);
    audioOutputVolumePercent_.store(settings.audioOutputVolumePercent, std::memory_order_relaxed);
    audioInputVolumePercent_.store(settings.audioInputVolumePercent, std::memory_order_relaxed);
    audioInputAutoGain_.store(settings.audioInputAutoGainEnabled, std::memory_order_relaxed);
#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
    audioInputNoiseSuppression_.store(settings.audioInputNoiseSuppressionEnabled, std::memory_order_relaxed);
#endif
}

void RecorderController::beginStartupSweep() {
    std::scoped_lock lock(startupSweepMutex_);
    if (startupSweepThread_.joinable()) {
        return; // A sweep is already in flight.
    }
    // Single snapshot: the path and the ring gate must come from one settings
    // read, not three separately allocated copies.
    const AppSettings snapshot = settings();
    // Capture the path: a settings change must not redirect the running sweep.
    const std::filesystem::path clipDirectory = snapshot.clipDirectory;
    // Only inspect legacy rings when neither track can have a live ring.
    const std::filesystem::path ringDirectory = replayRingDirectoryFor(snapshot);
    const bool sweepOrphanedRing = !snapshot.replay.diskBackedVideo && !snapshot.replay.diskBackedAudio;
    Logger::instance().info(L"app", L"Startup recovery sweep started in background");
    startupSweepThread_ = std::thread([clipDirectory, ringDirectory, sweepOrphanedRing]() {
        const LogContext::ScopedTask taskContext(L"startup-sweep");
        try {
            if (sweepOrphanedRing) {
                ReplayBuffer::removeOrphanedRing(ringDirectory);
            }
            RecoveryStore::sweepOrphanedRecoveryData(clipDirectory);
            Logger::instance().info(L"app", L"Startup recovery sweep finished");
        } catch (const std::exception& error) {
            Logger::instance().info(L"app", L"Startup recovery sweep finished");
            Logger::instance().warning(L"app", std::wstring(L"Startup recovery sweep failed: ") + utf8ToWide(error.what()));
        } catch (...) {
            Logger::instance().info(L"app", L"Startup recovery sweep finished");
            Logger::instance().warning(L"app", L"Startup recovery sweep failed: unknown exception");
        }
    });
}

void RecorderController::waitForStartupSweep() {
    // Under the mutex so concurrent callers serialize instead of racing
    // join() on the same thread object; the second caller observes a finished,
    // non-joinable thread and returns immediately.
    std::scoped_lock lock(startupSweepMutex_);
    if (!startupSweepThread_.joinable()) {
        return;
    }
    Logger::instance().debug(L"recorder", L"Waiting for startup recovery sweep to finish");
    startupSweepThread_.join();
}

bool RecorderController::initialize(AppSettings settings) {
    settings = sanitizeSettings(std::move(settings));
    AppSettings applied;
    {
        std::scoped_lock lock(stateMutex_);
        settings_ = std::move(settings);
        applied = settings_;
        activeVideoSettings_ = settings_.video;
        activeGpuSettings_ = settings_.gpu;
        publishLiveMirrors(settings_);
        microphoneAgcResetPending_ = true;
#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
        microphoneDenoiserResetPending_ = true;
#endif
        replay_.clear();
        replay_.configure(settings_.replay, replayRingDirectoryFor(settings_), replayRingCapacityBytesFor(settings_), replayAudioRingCapacityBytesFor(settings_));
    }
    Logger::instance().info(L"recorder", std::wstring(L"Recorder initialized: clips=") + applied.clipDirectory.wstring() +
                            L", replay=" + (applied.replay.enabled ? L"enabled" : L"disabled") +
                            L", fps=" + std::to_wstring(applied.video.fps) +
                            L", bitrateKbps=" + std::to_wstring(applied.video.bitrateKbps));
    // Recover previous sessions in the background. Recording and recovery
    // actions wait so the same session cannot be published twice concurrently.
    beginStartupSweep();
    return true;
}

void RecorderController::setUiPreferences(bool libraryGalleryView, bool loggingEnabled) {
    std::scoped_lock lock(stateMutex_);
    settings_.libraryGalleryView = libraryGalleryView;
    settings_.loggingEnabled = loggingEnabled;
}

bool RecorderController::updateSettings(AppSettings settings) {
    settings = sanitizeSettings(std::move(settings));
    bool startReplayPipeline = false;
    {
        std::scoped_lock lock(stateMutex_);
        // A queued action can predate a separately committed UI preference.
        settings.loggingEnabled = settings_.loggingEnabled;
        settings.libraryGalleryView = settings_.libraryGalleryView;
        AppSettings currentWithoutVoice = settings_;
        currentWithoutVoice.gameIntegrations.voiceCommand = settings.gameIntegrations.voiceCommand;
        if (currentWithoutVoice == settings) {
            settings_.gameIntegrations.voiceCommand = settings.gameIntegrations.voiceCommand;
            // Startup queues updateSettings with identical settings after initialize().
            // Still start the replay pipeline on that path; otherwise the first save
            // only has the cold-start IDR (one frame / 0:00).
            if (settings_.replay.enabled && !pipelineRunning_.load()) {
                startReplayPipeline = true;
            } else {
                Logger::instance().info(L"recorder", L"Applied voice command setting without restarting capture pipeline");
                return true;
            }
        }
        // A change confined to replay.* or the ring folder (e.g. toggling the
        // disk↔RAM buffering backend, or the replay window) does not touch
        // video/audio/GPU state,
        // so apply it in place instead of stopping and restarting the capture
        // pipeline. The full stop path below joins pipeline threads that can be
        // wedged on a stalled GPU/NVENC; avoiding it here keeps a pure replay
        // setting change from hanging updateSettings (and thereby blocking every
        // later controller action) until the app restarts.
        const SettingsApplyPath applyPath = settingsApplyPath(
            currentWithoutVoice, settings, pipelineRunning_.load(), startReplayPipeline);
        if (applyPath == SettingsApplyPath::ReplayInPlace) {
            settings_.gameIntegrations.voiceCommand = settings.gameIntegrations.voiceCommand;
            settings_.replay = settings.replay;
            settings_.replayRingDirectory = settings.replayRingDirectory;
            replay_.configure(settings_.replay, replayRingDirectoryFor(settings_), replayRingCapacityBytesFor(settings_), replayAudioRingCapacityBytesFor(settings_));
            Logger::instance().info(L"recorder",
                L"Replay buffer settings changed; reconfigured the buffer in place without restarting capture pipeline");
            return true;
        }
        // Volume, AGC-enable, denoiser-enable, and cursor changes apply live:
        // the audio writer reads those mirrors per packet and the capture loop
        // polls the cursor mirror every second. Restarting the pipeline here
        // would split the replay timeline (and the active recording) on every
        // volume-drag tick, so store and publish instead of bouncing.
        // The comparison clears only the mirror-applied fields, so replay stays
        // part of it: a replay change that fell past the branch above (pipeline
        // stopped, or replay toggled) must still take the full restart path.
        // startReplayPipeline means nothing actually changed (voice branch fell
        // through): let it reach ensurePipeline below instead of returning here.
        if (applyPath == SettingsApplyPath::LiveInPlace) {
            const bool agcChanged =
                settings_.audioInputAutoGainEnabled != settings.audioInputAutoGainEnabled;
#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
            const bool denoiserChanged =
                settings_.audioInputNoiseSuppressionEnabled != settings.audioInputNoiseSuppressionEnabled;
#endif
            settings_.gameIntegrations.voiceCommand = settings.gameIntegrations.voiceCommand;
            settings_.audioOutputVolumePercent = settings.audioOutputVolumePercent;
            settings_.audioInputVolumePercent = settings.audioInputVolumePercent;
            settings_.audioInputAutoGainEnabled = settings.audioInputAutoGainEnabled;
#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
            settings_.audioInputNoiseSuppressionEnabled = settings.audioInputNoiseSuppressionEnabled;
#endif
            settings_.captureCursor = settings.captureCursor;
            publishLiveMirrors(settings_);
            // Reset envelopes only when their enable actually flipped; a
            // volume-drag tick must not restart AGC adaptation audibly.
            if (agcChanged) {
                microphoneAgcResetPending_ = true;
            }
#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
            if (denoiserChanged) {
                microphoneDenoiserResetPending_ = true;
            }
#endif
            Logger::instance().info(L"recorder",
                L"Audio volume/cursor settings changed; applied live without restarting capture pipeline");
            return true;
        }
    }
    if (startReplayPipeline) {
        Logger::instance().info(L"recorder",
            L"Settings unchanged; starting capture pipeline because replay is enabled and pipeline is stopped");
        return ensurePipeline();
    }
    const bool wasRunning = pipelineRunning_.load();
    const bool wasRecording = recording_.load();
    Logger::instance().info(L"recorder", std::wstring(L"Applying recorder settings: pipelineRunning=") + (wasRunning ? L"yes" : L"no") +
                            L", recording=" + (wasRecording ? L"yes" : L"no"));
    if (wasRecording) {
        stopRecording();
    }
    if (wasRunning) {
        stopPipeline();
    }

    // Read under the lock: a concurrent updateSettings/initialize may be
    // writing settings_ right now, and an unsynchronized read of a struct
    // holding paths/wstrings is UB (torn read).
    bool replayEnabledAfterApply = false;
    {
        std::scoped_lock lock(stateMutex_);
        const VideoSettings oldVideo = activeVideoSettings_;
        settings.loggingEnabled = settings_.loggingEnabled;
        settings.libraryGalleryView = settings_.libraryGalleryView;
        settings_ = std::move(settings);
        activeVideoSettings_ = settings_.video;
        activeGpuSettings_ = settings_.gpu;
        publishLiveMirrors(settings_);
        microphoneAgcResetPending_ = true;
#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
        microphoneDenoiserResetPending_ = true;
#endif
        replayEnabledAfterApply = settings_.replay.enabled;
        replay_.configure(settings_.replay, replayRingDirectoryFor(settings_), replayRingCapacityBytesFor(settings_), replayAudioRingCapacityBytesFor(settings_));
        // A codec/resolution/fps change makes every buffered packet's bitstream
        // incompatible with the new pipeline. Keep them and a replay save would
        // mix two SPS/PPS worlds into one clip. Drop the buffer so the timeline
        // restarts cleanly; lastSaveEnd is reset too, so a consecutive save does
        // not search for overlap across the encode-parameter switch.
        if (oldVideo.codec != activeVideoSettings_.codec ||
            oldVideo.width != activeVideoSettings_.width ||
            oldVideo.height != activeVideoSettings_.height ||
            oldVideo.fps != activeVideoSettings_.fps) {
            replay_.clear();
        }
    }

    if (replayEnabledAfterApply) {
        const bool started = ensurePipeline();
        if (!started) {
            Logger::instance().error(L"recorder", L"Could not restart capture pipeline after applying settings");
        }
        return started;
    }
    Logger::instance().info(L"recorder", wasRecording
                                ? L"Recorder settings applied; recording was stopped and replay is disabled, so the pipeline remains stopped"
                                : L"Recorder settings applied without starting pipeline because replay is disabled and no recording is active");
    return true;
}

bool RecorderController::startRecording() {
    if (recording_) {
        Logger::instance().info(L"recorder", L"Start recording requested while already recording");
        return true;
    }
    // The startup sweep may still be remuxing a crashed session's recovery
    // data; a new recording stages fresh state into the same directories.
    waitForStartupSweep();
    Logger::instance().info(L"recorder", L"Start recording requested");
    setLastRecordingError({});
    if (!ensurePipeline()) {
        Logger::instance().error(L"recorder", L"Recording could not start because the capture pipeline failed to start");
        return false;
    }

    const auto output = nextClipPath(L"recording", false);
    if (output.empty()) {
        setLastRecordingError(L"could not reserve a recording output name");
        if (!replayEnabledMirror_.load(std::memory_order_relaxed)) {
            stopPipeline();
        }
        return false;
    }
    VideoSettings videoSettings;
    {
        std::scoped_lock lock(stateMutex_);
        videoSettings = activeVideoSettings_;
    }
    if (!muxer_.startRecording(output, videoSettings)) {
        const std::wstring detail = L"could not create the recording temporary streams in " + output.parent_path().wstring();
        Logger::instance().error(L"recorder", L"Recording muxer could not start for output: " + output.wstring());
        // Release the clip name reserved by nextClipPath; nothing was written.
        removeEmptyOutputReservation(output);
        setLastRecordingError(detail);
        if (!replayEnabledMirror_.load(std::memory_order_relaxed)) {
            stopPipeline();
        }
        return false;
    }
    lastEncodedVideoPts100ns_ = 0;
    waitingForRecordingKeyFrame_ = true;
    recording_ = true;
    // Request the IDR via forceVideoHeartbeat_, consumed by the capture thread
    // in emitFrame. Locking pipelineGpuMutex_ here would block this caller for
    // as long as a concurrent pipeline recreate holds it (up to seconds).
    // The flag survives a recreate in progress and takes effect afterwards.
    forceVideoHeartbeat_ = true;
    Logger::instance().info(L"recorder", L"Recording started");
    return true;
}

std::filesystem::path RecorderController::stopRecording() {
    if (!recording_) {
        Logger::instance().info(L"recorder", L"Stop recording requested while no recording is active");
        return {};
    }
    Logger::instance().info(L"recorder", L"Stop recording requested");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (muxer_.videoPacketCount() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    const int64_t stopPts100ns = steadyNow100ns();
    const uint64_t cutoffGeneration = recordingCutoff_.beginStop(stopPts100ns);
    // The muxer keeps everything at or below the cut and holds the rest, so the
    // clip ends on a decode-order prefix even when a post-stop packet completes
    // before a pre-stop B picture (finding 04). Set next to the cutoff latch so
    // the two agree on one boundary.
    muxer_.setCutPts100ns(stopPts100ns);

    // Mirror, not settings(): this path needs one bool, not a whole struct
    // copy with paths and vectors.
    if (!replayEnabledMirror_.load(std::memory_order_relaxed)) {
        // Drains the frame queue and the encoder on this thread; every
        // pre-stop packet is published before the call returns, so the
        // synchronous drain is the acknowledgement.
        stopPipeline();
        recordingCutoff_.forceAcknowledge();
    } else {
        // The pipeline keeps running for replay, so the encode thread keeps
        // submitting frames and flushes the reorder buffer on its own. Wait
        // for the encode thread's acknowledgement that every pre-stop frame
        // was published; a PTS comparison cannot see frames still in flight
        // (finding 08).
        std::unique_lock lock(recordingCutoffMutex_);
        recordingCutoffComplete_.wait_for(lock, std::chrono::seconds(5), [&]() {
            return recordingCutoff_.acknowledged(cutoffGeneration);
        });
        if (!recordingCutoff_.acknowledged(cutoffGeneration)) {
            Logger::instance().warning(L"recorder",
                L"Recording cutoff drain timed out; the clip may be missing its final frames");
        }
    }

    recording_ = false;
    waitingForRecordingKeyFrame_ = false;
    const LogContext::ScopedTask saveTask(L"saveRecording");
    recordingCutoff_.endStop();
    auto output = muxer_.finalize();
    setLastRecordingError(output.empty() ? muxer_.lastError() : std::wstring());
    Logger::instance().info(L"recorder", output.empty()
                                ? std::wstring(L"Recording stopped without a finalized clip")
                                : std::wstring(L"Recording stopped and saved: ") + output.wstring());
    return output;
}

bool RecorderController::hasRecoverableRecording() {
    if (isRecording()) {
        return false;
    }
    waitForStartupSweep();
    return RecoveryStore::hasRecoverableRecording(settings().clipDirectory);
}

std::filesystem::path RecorderController::recoverFailedRecording() {
    const LogContext::ScopedTask taskContext(L"recoverFailedRecording");
    LogContext::breadcrumb(L"recover requested");
    Logger::instance().info(L"recorder", L"Recover failed recording requested");
    // Serialize with the startup sweep so a session is not recovered twice.
    waitForStartupSweep();
    if (recording_) {
        const std::wstring detail = L"Stop the active recording before recovering a failed recording";
        setLastRecordingError(detail);
        Logger::instance().warning(L"recorder", detail);
        return {};
    }

    std::wstring detail;
    const auto output = Mp4Muxer::recoverLatestFailedRecording(settings().clipDirectory, detail);
    setLastRecordingError(detail);
    if (output.empty()) {
        Logger::instance().warning(L"recorder", detail.empty() ? L"Failed recording recovery did not produce a clip" : detail);
    }
    return output;
}

std::filesystem::path RecorderController::saveReplay() {
    const LogContext::ScopedTask saveTask(L"saveReplay");
    LogContext::breadcrumb(L"save requested");
    Logger::instance().info(L"recorder", L"Save replay requested");
    // Replay saves stage MP4 data through the same .backtrack_tmp tree the
    // startup sweep maintains; never overlap with it.
    waitForStartupSweep();
    // Cleared up front so the UI never surfaces a stale reason from an
    // earlier action; every failure path below stores a specific cause.
    setLastRecordingError({});
    // Single snapshot for the whole save: the enable gate, the window math,
    // and the output folder must agree with each other.
    const AppSettings snapshot = settings();
    if (!snapshot.replay.enabled) {
        const std::wstring detail = L"instant replay is disabled in settings";
        Logger::instance().warning(L"recorder", L"Replay save ignored: " + detail);
        setLastRecordingError(detail);
        return {};
    }
    // Never mux a near-empty buffer (e.g. a single cold-start IDR or a capture
    // starved by exclusive fullscreen) into a one-frame clip. Collect at least
    // this much muxable video before saving. For short windows demand a healthy
    // fraction (the first keyframe can sit inside the window, so a full window
    // may be unreachable); for long windows keep the 2-second minimum.
    const int64_t playbackWindow100ns = secondsTo100ns(
        std::max<uint32_t>(1, snapshot.replay.seconds));
    const int64_t minReplayVideo100ns = std::min<int64_t>(
        2 * kHundredNanosecondsPerSecond,
        std::max<int64_t>(kHundredNanosecondsPerSecond / 2, (playbackWindow100ns * 2) / 3));
    // A buffer whose newest packet stopped advancing means the capture/encode
    // pipeline stalled (frozen GPU/NVENC). Trim keeps it passing the span gate
    // because heartbeats keep emitting with advancing timestamps, so a save
    // would mux the last real frame from before the stall. Treat a buffer
    // older than the window plus a few GOPs as needing a fresh keyframe.
    const int64_t freshnessSlack100ns = 3 * secondsTo100ns(
        std::max<uint32_t>(1, snapshot.video.gopSeconds));
    const int64_t staleWindow100ns = playbackWindow100ns + freshnessSlack100ns;
    auto bufferedVideoStale = [&]() {
        const int64_t newestPts = replay_.newestVideoPts100ns();
        return newestPts > 0 &&
               steadyNow100ns() - newestPts > staleWindow100ns;
    };
    const bool needsIdrRefresh = bufferedVideoStale();
    const bool bufferHasEnoughVideo =
        replay_.videoKeyFrameCount() > 0 &&
        replay_.videoSpan100ns() >= minReplayVideo100ns;
    // Buffer already has enough video and is fresh: mux without restarting
    // capture (avoids WGC recreate hangs and is unnecessary work when pipeline
    // is already warm).
    if (bufferHasEnoughVideo && !needsIdrRefresh) {
        Logger::instance().debug(L"recorder", L"Replay save using existing buffer; pipeline ensure skipped");
    } else {
        if (needsIdrRefresh) {
            Logger::instance().warning(L"recorder",
                L"Replay buffered video is stale (newest packet is " +
                std::to_wstring((steadyNow100ns() - replay_.newestVideoPts100ns()) / kHundredNanosecondsPerSecond) +
                L"s old); ensuring pipeline and requesting a fresh IDR");
        } else {
            Logger::instance().info(L"recorder",
                L"Replay save has no buffered keyframe or too little video (span=" +
                std::to_wstring(replay_.videoSpan100ns() / kHundredNanosecondsPerSecond) +
                L"s); ensuring pipeline and requesting IDR");
        }
        if (!ensurePipeline()) {
            const std::wstring detail = L"capture pipeline could not be started";
            Logger::instance().error(L"recorder", L"Replay could not be saved because the " + detail);
            setLastRecordingError(detail);
            return {};
        }
        // Same deferral as startRecording: forceVideoHeartbeat_ makes the
        // capture thread request the IDR (and emit a heartbeat frame) without
        // this thread waiting on pipelineGpuMutex_ during a recreate.
        forceVideoHeartbeat_ = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        // videoSpan100ns is O(buffered packets); sample it every ~10th poll
        // (200 ms) so the preflight's transient work stays bounded. The abort
        // checks below still re-read the live value.
        int64_t span100ns = replay_.videoSpan100ns();
        size_t pollCounter = 0;
        while (replay_.videoKeyFrameCount() == 0 ||
               span100ns < minReplayVideo100ns ||
               bufferedVideoStale()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (++pollCounter % 10 == 0) {
                span100ns = replay_.videoSpan100ns();
            }
        }
        if (replay_.videoKeyFrameCount() == 0) {
            const std::wstring detail = L"no keyframe buffered yet";
            Logger::instance().warning(L"recorder", L"Replay save aborted: " + detail + L" within 3s");
            setLastRecordingError(detail);
            return {};
        }
        if (replay_.videoSpan100ns() < minReplayVideo100ns) {
            const std::wstring detail =
                L"only " + std::to_wstring(replay_.videoSpan100ns() / kHundredNanosecondsPerSecond) +
                L"s of video buffered (need about " +
                std::to_wstring(minReplayVideo100ns / kHundredNanosecondsPerSecond) + L"s)";
            Logger::instance().warning(L"recorder", L"Replay save aborted: insufficient video buffered within 3s");
            setLastRecordingError(detail);
            return {};
        }
        if (bufferedVideoStale()) {
            const std::wstring detail =
                L"capture pipeline stalled; buffered video is " +
                std::to_wstring((steadyNow100ns() - replay_.newestVideoPts100ns()) / kHundredNanosecondsPerSecond) +
                L"s old and no fresh keyframe arrived";
            Logger::instance().error(L"recorder", L"Replay save aborted: " + detail);
            setLastRecordingError(detail);
            return {};
        }
    }
    const std::wstring gameName = foregroundApplicationName();
    const auto output = nextClipPath(
        gameName.empty() ? L"replay" : gameName.c_str(),
        snapshot.organizeClipsByGame && !gameName.empty());
    if (output.empty()) {
        setLastRecordingError(L"could not reserve a replay output name");
        return {};
    }
    // A reservation is a hidden empty file that holds the name. saveTo can fail
    // or throw before the muxer consumes it, which would leave an empty clip and
    // a blocked name behind; the guard releases it unless the save published.
    // removeEmptyOutputReservation only ever deletes a hidden zero-byte file, so
    // a published clip is never at risk.
    struct OutputReservationGuard {
        const std::filesystem::path& path;
        bool dismissed = false;
        ~OutputReservationGuard() {
            if (!dismissed) {
                removeEmptyOutputReservation(path);
            }
        }
    } reservation{output};
    VideoSettings videoSettings;
    {
        std::scoped_lock lock(stateMutex_);
        videoSettings = activeVideoSettings_;
    }
    Logger::instance().info(L"recorder", L"Replay saveTo begin: " + output.wstring());
    LogContext::breadcrumb(L"saveTo begin");
    const auto saveStart = SteadyClock::now();
    std::wstring saveDetail;
    const bool saved = replay_.saveTo(output, videoSettings, &saveDetail);
    if (saved) {
        reservation.dismissed = true;
    }
    // A replay save muxes already-compressed streams; longer than twice the
    // window (or 30 s, whichever is larger) means a slow disk is stalling the save.
    const auto saveBudgetMs = std::max<int64_t>(30000,
        (playbackWindow100ns * 2) / kHundredNanosecondsPerSecond * 1000);
    logLatencyWarn(L"recorder", L"Replay save exceeded budget", saveStart,
        std::chrono::milliseconds(saveBudgetMs),
        L"window_s=" + std::to_wstring(snapshot.replay.seconds));
    LogContext::breadcrumb(saved ? L"save completed" : L"save failed");
    if (!saved) {
        setLastRecordingError(saveDetail.empty()
            ? L"MP4 muxing failed; see log for details"
            : saveDetail);
        Logger::instance().warning(L"recorder", std::wstring(L"Replay save failed: ") + output.wstring());
    } else {
        setLastRecordingError({});
    }
    return saved ? output : std::filesystem::path();
}

void RecorderController::shutdown() {
    // Join the sweep worker before tearing down anything it could observe.
    waitForStartupSweep();
    if (recording_) {
        stopRecording();
    }
    stopPipeline();
    // Unconditional, and here rather than only in the destructor: the
    // single-instance mutex is still held on this path, so a restore that needs
    // to look at the running instance does not race a second launch. The
    // destructor's call stays as the last-resort guard (idempotent).
    restoreSessionMutes(L"for the next launch");
}

RecordingStats RecorderController::stats() const {
    RecordingStats stats;
    stats.recording = recording_.load();
    // Mirror, not settings(): the diagnostics timer calls here ~1/s and a
    // settings() copy allocates (paths, vectors) on every call.
    stats.replayEnabled = replayEnabledMirror_.load(std::memory_order_relaxed);
    {
        std::scoped_lock lock(captureStatusMutex_);
        stats.selectedCaptureBackend = selectedCaptureBackend_;
        stats.activeCaptureBackend = activeCaptureBackend_;
        stats.captureBackendActive = captureBackendActive_;
        stats.captureBackendFallbackUsed = captureBackendFallbackUsed_;
        stats.captureBackendStatus = captureBackendStatus_;
    }
    stats.capturedFrames = capturedFrames_.load();
    stats.sourceFrames = sourceFrames_.load();
    stats.sourceFramesPerSecond = sourceFramesPerSecond_.load();
    stats.timelineIntervalsPerSecond = timelineIntervalsPerSecond_.load();
    stats.cadenceDuplicateFrames = cadenceDuplicateFrames_.load();
    stats.catchUpDuplicateFrames = catchUpDuplicateFrames_.load();
    stats.coalescedIdleIntervals = coalescedIdleIntervals_.load();
    stats.droppedFrames = droppedFrames_.load();
    stats.gpuProtectionDrops = gpuProtectionDrops_.load();
    stats.frameQueueDrops = frameQueueDrops_.load();
    stats.idleFrameSkips = idleFrameSkips_.load();
    stats.cursorOnlyFrames = cursorOnlyFrames_.load();
    stats.systemAudioQueueDrops = systemAudioQueueDrops_.load();
    stats.microphoneAudioQueueDrops = microphoneAudioQueueDrops_.load();
    stats.systemAudioActive = systemAudio_.running();
    stats.microphoneAudioActive = microphoneAudio_.running();
    {
        const int64_t now100ns = steadyNow100ns();
        const auto silenceMs = [&](const std::atomic<int64_t>& lastPacketAt) -> uint64_t {
            const int64_t at = lastPacketAt.load(std::memory_order_relaxed);
            if (at <= 0 || now100ns <= at) {
                return 0;
            }
            return static_cast<uint64_t>((now100ns - at) / (kHundredNanosecondsPerSecond / 1000));
        };
        stats.systemAudioSilenceMs = silenceMs(systemAudioLastPacketAt100ns_);
        stats.microphoneSilenceMs = silenceMs(microphoneAudioLastPacketAt100ns_);
    }
    stats.replayVideoPackets = replay_.videoPacketCount();
    stats.replayKeyFrames = replay_.videoKeyFrameCount();
    stats.captureWidth = captureWidth_.load();
    stats.captureHeight = captureHeight_.load();
    stats.encodeWidth = encodeWidth_.load();
    stats.encodeHeight = encodeHeight_.load();
    stats.hdrActive = hdrActive_.load(std::memory_order_relaxed);
    const bool hdrMonitor = hdrMonitorActive_.load(std::memory_order_relaxed);
    bool scalerHdrRejected = false;
    {
        // Diagnostics refreshes on the UI thread and on the controller worker
        // thread, so the unlocked read below can race a publish. GPU recovery can
        // hold this lock while WGC waits for outstanding frame leases, so never
        // block UI for it either: the snapshot pointer is the only handoff.
        std::unique_lock gpuLock(pipelineGpuMutex_, std::try_to_lock);
        if (gpuLock.owns_lock()) {
            if (encoder_) {
                lastEncoderStats_.store(std::make_shared<const EncoderStats>(encoder_->stats()));
            }
            // The scaler is only touched under pipelineGpuMutex_ (both scale()
            // sites and every reset()), so this won-lock read of its sticky HDR
            // rejection is race-free and still never blocks the UI thread.
            scalerHdrRejected = scaler_.hdrInputRejected();
        }
        stats.encoder = *lastEncoderStats_.load();
    }
    // A driver that refused tone-mapping still sinks HDR frames, so the scaler
    // latch counts only alongside live HDR input (gating also keeps a stale
    // latch from painting SDR content as rejected); the monitor probe covers
    // the WGC/SDR-interpretation case instead.
    stats.hdrRejected = (stats.hdrActive && scalerHdrRejected) || (hdrMonitor && !stats.hdrActive);
    stats.hdrFlipCount = hdrFlipCount_.load(std::memory_order_relaxed);
    stats.hdrFormat = static_cast<DXGI_FORMAT>(hdrFormatCode_.load(std::memory_order_relaxed));
    stats.encoder.queueDepth = static_cast<uint32_t>(frameQueue_.size());
    detectVideoPipelineStall();
    detectSilentAudioInput();
    return stats;
}

void RecorderController::detectVideoPipelineStall() const {
    if (!pipelineRunning_.load() || stopRequested_.load() || !replayEnabledMirror_.load(std::memory_order_relaxed)) {
        videoStallWarningActive_.store(false, std::memory_order_relaxed);
        videoStallSeenPts100ns_.store(0, std::memory_order_relaxed);
        return;
    }
    const int64_t newestPts = lastEncodedVideoPts100ns_.load(std::memory_order_acquire);
    if (newestPts <= 0) {
        // Pipeline just (re)started: no encoded packet yet, cannot judge.
        videoStallWarningActive_.store(false, std::memory_order_relaxed);
        videoStallSeenPts100ns_.store(0, std::memory_order_relaxed);
        return;
    }
    const int64_t seenPts = videoStallSeenPts100ns_.load(std::memory_order_relaxed);
    if (seenPts > 0 && newestPts > seenPts) {
        // A new encoded packet arrived: any prior stall episode is over, so a
        // later stall warns again instead of staying silent.
        videoStallWarningActive_.store(false, std::memory_order_relaxed);
    }
    videoStallSeenPts100ns_.store(newestPts, std::memory_order_relaxed);

    const int64_t idleAge100ns = steadyNow100ns() - newestPts;
    // Heartbeats re-encode a frame every GOP, so a live-but-idle pipeline
    // advances newestPts within one GOP; only a real stall lets it age past
    // several GOPs. Threshold is at least 10s to avoid false alarms on a slow
    // heartbeat interval while still catching a wedged capture/encoder.
    const int64_t stallThreshold100ns = std::max<int64_t>(
        10 * kHundredNanosecondsPerSecond,
        3 * secondsTo100ns(std::max<uint32_t>(1, videoGopSecondsMirror_.load(std::memory_order_relaxed))));
    if (idleAge100ns < stallThreshold100ns) {
        return;
    }
    if (videoStallWarningActive_.load(std::memory_order_acq_rel)) {
        return;
    }
    videoStallWarningActive_.store(true, std::memory_order_release);
    Logger::instance().warning(L"recorder",
        L"Capture/encode pipeline appears stalled: no new video packet for " +
        std::to_wstring(idleAge100ns / kHundredNanosecondsPerSecond) +
        L"s (threshold " +
        std::to_wstring(stallThreshold100ns / kHundredNanosecondsPerSecond) +
        L"s); replay buffer is no longer advancing");
}

void RecorderController::detectSilentAudioInput() const {
    const auto check = [this](bool enabled, const std::atomic<int64_t>& lastPacketAt,
                              bool sourceRunning, std::atomic<bool>& warned,
                              const wchar_t* what) {
        if (!enabled) {
            warned.store(false, std::memory_order_relaxed);
            return;
        }
        const int64_t at = lastPacketAt.load(std::memory_order_relaxed);
        const int64_t now100ns = steadyNow100ns();
        const bool silent = at > 0 && now100ns > at &&
            (now100ns - at) >= 10 * kHundredNanosecondsPerSecond;
        if (!sourceRunning || !silent) {
            // A packet arrived (or the source is not running): clear the latch
            // so the next episode warns again instead of staying silent.
            warned.store(false, std::memory_order_relaxed);
            return;
        }
        if (warned.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        Logger::instance().warning(L"recorder",
            std::wstring(what) + L" is silent: no audio packet for " +
            std::to_wstring((now100ns - at) / kHundredNanosecondsPerSecond) + L"s");
    };
    check(captureSystemAudioMirror_.load(std::memory_order_relaxed),
          systemAudioLastPacketAt100ns_, systemAudio_.running(),
          systemAudioSilenceWarned_, L"System audio capture");
    check(captureMicrophoneMirror_.load(std::memory_order_relaxed),
          microphoneAudioLastPacketAt100ns_, microphoneAudio_.running(),
          microphoneAudioSilenceWarned_, L"Microphone capture");
}

EncoderCapabilities RecorderController::encoderCapabilities() const {
    // See stats(): reporting must not wait for GPU teardown or recreation.
    std::unique_lock gpuLock(pipelineGpuMutex_, std::try_to_lock);
    if (gpuLock.owns_lock() && encoder_) {
        lastEncoderCapabilities_.store(
            std::make_shared<const EncoderCapabilities>(encoder_->capabilities()));
    }
    return *lastEncoderCapabilities_.load();
}

AppSettings RecorderController::settings() const {
    std::scoped_lock lock(stateMutex_);
    return settings_;
}

std::wstring RecorderController::captureBackendStatus() const {
    std::scoped_lock lock(captureStatusMutex_);
    return captureBackendStatus_;
}

std::wstring RecorderController::lastRecordingError() const {
    std::scoped_lock lock(recordingErrorMutex_);
    return lastRecordingError_;
}

std::wstring RecorderController::replayStorageFallback() const {
    return replay_.storageFallbackReason();
}

bool RecorderController::ensurePipeline() {
    if (pipelineRunning_) {
        return true;
    }

    AppSettings snapshot = settings();
    Logger::instance().info(L"recorder", std::wstring(L"Starting capture pipeline: clips=") + snapshot.clipDirectory.wstring() +
                            L", captureBackend=" +
                            (snapshot.preferredCaptureBackend == CaptureBackend::DesktopDuplication
                                 ? L"Desktop Duplication"
                                 : L"WGC") +
                            L", multiMonitorSupport=" + (snapshot.multiMonitorSupport ? L"yes" : L"no") +
                            L", followMouseMonitor=" + (snapshot.followMouseMonitor ? L"yes" : L"no") +
                            L", systemAudio=" + (snapshot.captureSystemAudio ? L"yes" : L"no") +
                            L", microphone=" + (snapshot.captureMicrophone ? L"yes" : L"no"));
    std::error_code directoryError;
    std::filesystem::create_directories(snapshot.clipDirectory, directoryError);
    if (directoryError) {
        Logger::instance().error(L"recorder", std::wstring(L"Could not create clip directory: ") + snapshot.clipDirectory.wstring() +
                                 L" (" + utf8ToWide(directoryError.message()) + L")");
        std::scoped_lock lock(captureStatusMutex_);
        captureBackendActive_ = false;
        captureBackendStatus_ = L"Capture pipeline failed before capture backend start: clip folder could not be created";
        return false;
    }

    frameQueue_.resetCapacity(frameQueueCapacityFor(snapshot.gpu));

    const CaptureTarget target = captureTargetForSettings(snapshot);
    if (!recreateGpuPipeline(target, L"pipeline start", pipelineAdapterForTarget(target))) {
        Logger::instance().error(L"recorder", L"Capture pipeline start failed during GPU/capture initialization");
        return false;
    }

    const uint32_t sourceWidth = captureWidth_.load();
    const uint32_t sourceHeight = captureHeight_.load();

    replay_.configure(snapshot.replay, replayRingDirectoryFor(snapshot), replayRingCapacityBytesFor(snapshot), replayAudioRingCapacityBytesFor(snapshot));
    stopRequested_ = false;
    captureFinished_ = false;
    audioProducersFinished_ = false;
    discardQueuedFrames_ = false;
    recordingCutoff_.reset();
    durationUpdatePending_ = false;
    {
        std::scoped_lock lock(durationUpdateMutex_);
        pendingDurationPts100ns_ = 0;
        pendingDuration100ns_ = 0;
    }
    // recreateGpuPipeline may have requested a keyframe; keep that flag.
    capturedFrames_ = 0;
    sourceFrames_ = 0;
    sourceFramesPerSecond_ = 0;
    timelineIntervalsPerSecond_ = 0;
    cadenceDuplicateFrames_ = 0;
    catchUpDuplicateFrames_ = 0;
    coalescedIdleIntervals_ = 0;
    droppedFrames_ = 0;
    gpuProtectionDrops_ = 0;
    frameQueueDrops_ = 0;
    idleFrameSkips_ = 0;
    cursorOnlyFrames_ = 0;
    systemAudioQueueDrops_ = 0;
    microphoneAudioQueueDrops_ = 0;
    hdrFlipCount_ = 0;
    hdrFormatCode_.store(static_cast<int>(DXGI_FORMAT_UNKNOWN), std::memory_order_relaxed);
    // Audio-liveness clocks start at pipeline start so a never-delivering
    // input shows a growing silence age instead of 0.
    systemAudioLastPacketAt100ns_.store(steadyNow100ns(), std::memory_order_relaxed);
    microphoneAudioLastPacketAt100ns_.store(steadyNow100ns(), std::memory_order_relaxed);
    lastEncodedVideoPts100ns_ = 0;

    if (snapshot.captureSystemAudio) {
        refreshSystemAudioCapture(snapshot, mutedSoundSeparationExecutableKeys(snapshot));
    }
    if (snapshot.captureMicrophone) {
        microphoneAgcResetPending_ = true;
#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
        microphoneDenoiserResetPending_ = true;
#endif
        if (!microphoneAudio_.start(AudioTrack::Microphone, snapshot.audioInputDeviceId, [this](AudioPacket&& packet) { handleAudioPacket(std::move(packet)); })) {
            Logger::instance().warning(L"recorder", L"Microphone capture did not start");
        }
    }

    pipelineRunning_ = true;
    // The capture worker may request an encode-thread discard immediately.
    encodeThread_ = std::thread(&RecorderController::encodeLoop, this);
    captureThread_ = std::thread(&RecorderController::captureLoop, this);
    audioThread_ = std::thread(&RecorderController::audioLoop, this);
    VideoSettings videoSettings;
    {
        std::scoped_lock lock(stateMutex_);
        videoSettings = activeVideoSettings_;
    }
    Logger::instance().info(L"recorder", std::wstring(L"Capture/encode pipeline started: capture=") + std::to_wstring(sourceWidth) + L"x" + std::to_wstring(sourceHeight) +
                            L", encode=" + std::to_wstring(videoSettings.width) + L"x" + std::to_wstring(videoSettings.height));
    return true;
}

void RecorderController::stopPipeline() {
    if (!pipelineRunning_) {
        return;
    }

    Logger::instance().info(L"recorder", L"Capture/encode pipeline stop requested");
    stopRequested_ = true;
    // Drain (not discard) the remaining queue: encodeLoop exits only after the
    // queue is empty, so queued frames still reach replay and the recording
    // instead of silently vanishing on settings switches / shutdown.
    if (frameEvent_) {
        SetEvent(frameEvent_);
    }
    // Join the capture worker BEFORE stopping audio. Its 2 s sound-separation
    // poll restarts system audio through refreshSystemAudioCapture, and
    // WasapiCapture::start/stop share one unsynchronized std::thread - stopping
    // audio here while the loop is inside start() races on that object. The loop
    // exits on stopRequested_ alone; nothing below needs audio already stopped.
    if (captureThread_.joinable()) {
        Logger::instance().info(L"recorder", L"Waiting for capture worker to stop");
        captureThread_.join();
        Logger::instance().info(L"recorder", L"Capture worker stopped");
    }
    // Both joins are unbounded on purpose. A worker wedged inside a vendor call
    // (NVENC submit, WGC session close, a synchronous ring write) cannot be
    // abandoned: detaching would let it touch RecorderController after the
    // object dies, and terminating would throw away the user's clip.
    // std::thread::join has no timed form and kGpuPipelineStopTimeout does not
    // apply to a thread that must not be left running, so the info line above is
    // the diagnostic: a real hang ends the log at "Waiting for <worker>".
    if (encodeThread_.joinable()) {
        // The capture worker has been joined above, so it can no longer push; a
        // worker that never started (thread creation failed) would otherwise
        // leave the encode loop waiting on a producer that does not exist, and
        // these joins have no timeout.
        captureFinished_.store(true, std::memory_order_release);
        Logger::instance().info(L"recorder", L"Waiting for encode worker to stop");
        encodeThread_.join();
        Logger::instance().info(L"recorder", L"Encode worker stopped");
    }
    Logger::instance().debug(L"recorder", L"Capture/encode threads joined");

    systemAudio_.stop();
    microphoneAudio_.stop();
    // Both producers are joined — no more packets can be enqueued.
    // Signal the writer so it drains the queues and exits.
    audioProducersFinished_.store(true, std::memory_order_release);
    Logger::instance().info(L"recorder", L"Capture/encode pipeline audio stopped");
    if (audioEvent_) {
        SetEvent(audioEvent_);
    }
    if (audioThread_.joinable()) {
        Logger::instance().info(L"recorder", L"Waiting for audio writer to stop");
        audioThread_.join();
        Logger::instance().info(L"recorder", L"Audio writer stopped");
    }
    restoreSessionMutes(L"for the next launch");
    {
        std::scoped_lock lock(systemAudioCaptureMutex_);
        systemAudioMutedExecutableKeys_.clear();
        systemAudioExclusionFailed_ = false;
    }
    systemAudioQueue_.clear();
    microphoneAudioQueue_.clear();

    const RecordingStats finalStats = stats();
    Logger::instance().info(L"recorder",
        L"Capture/encode summary: timeline intervals=" +
        std::to_wstring(finalStats.capturedFrames) +
        L", source frames=" + std::to_wstring(finalStats.sourceFrames) +
        L", cadence duplicates=" +
        std::to_wstring(finalStats.cadenceDuplicateFrames) +
        L", catch-up duplicates=" +
        std::to_wstring(finalStats.catchUpDuplicateFrames) +
        L", coalesced intervals=" +
        std::to_wstring(finalStats.coalescedIdleIntervals) +
        L", capture drops=" + std::to_wstring(finalStats.droppedFrames) +
        L", GPU protection drops=" +
        std::to_wstring(finalStats.gpuProtectionDrops) +
        L", frame queue drops=" + std::to_wstring(finalStats.frameQueueDrops) +
        L", NVENC submissions=" +
        std::to_wstring(finalStats.encoder.submittedFrames) +
        L", encoded frames=" +
        std::to_wstring(finalStats.encoder.encodedFrames) +
        L", keyframes=" + std::to_wstring(finalStats.encoder.keyFrames) +
        L", encoder drops=" +
        std::to_wstring(finalStats.encoder.droppedFrames) +
        L", encoded bytes=" +
        std::to_wstring(finalStats.encoder.encodedBytes) +
        L", idle frame skips=" + std::to_wstring(finalStats.idleFrameSkips) +
        L", cursor-only frames=" + std::to_wstring(finalStats.cursorOnlyFrames) +
        L", audio queue drops (system/mic)=" +
        std::to_wstring(finalStats.systemAudioQueueDrops) + L"/" +
        std::to_wstring(finalStats.microphoneAudioQueueDrops) +
        L", avg encode=" +
        std::to_wstring(finalStats.encoder.averageEncodeMs) + L"ms" +
        L", frame queue size=" + std::to_wstring(frameQueue_.size()));

    {
        std::scoped_lock gpuLock(pipelineGpuMutex_);
        encoderZeroReorderDelay_.store(false, std::memory_order_relaxed);
        if (encoder_) {
            encoder_->shutdown();
            encoder_.reset();
        }
        frameQueue_.clear();
        discardedFrames_.clear();
        captureLastFrame_ = {};
        captureAcquiredFrame_ = {};
        scaler_.reset();
    }
    Logger::instance().debug(L"recorder", L"Capture source shutdown begin");
    if (capture_) {
        capture_->shutdown();
        capture_.reset();
    }
    Logger::instance().debug(L"recorder", L"Capture source shutdown done; D3D shutdown begin");
    d3d_.shutdown();
    {
        std::scoped_lock lock(captureStatusMutex_);
        captureBackendActive_ = false;
        if (captureBackendStatus_.empty()) {
            captureBackendStatus_ = L"Capture pipeline stopped";
        } else {
            captureBackendStatus_ += L"; stopped";
        }
    }

    pipelineRunning_ = false;
    Logger::instance().info(L"recorder", L"Capture/encode pipeline stopped");
}

uint32_t RecorderController::pipelineAdapterForTarget(const CaptureTarget& target) const {
    const DxgiOutputLocation location = dxgiOutputForMonitor(monitorForCaptureTarget(target));
    if (location.valid() && dxgiAdapterSupportsHardwareEncode(location.adapterIndex)) {
        return location.adapterIndex;
    }
    // WGC bridges capture adapters, so select an adapter with a supported encoder.
    return dxgiHardwareEncoderAdapterOr(d3d_.adapterIndex());
}

bool RecorderController::discardFrameQueueAndWait() {
    std::unique_lock discardLock(frameDiscardMutex_);
    try {
        discardedFrames_.reserve(discardedFrames_.size() + frameQueue_.capacity() + 1);
    } catch (const std::exception& error) {
        Logger::instance().error(L"recorder",
            std::wstring(L"Could not retain discarded capture frames: ") + utf8ToWide(error.what()));
        return false;
    }
    const uint64_t discardGeneration =
        discardRequestGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
    discardQueuedFrames_.store(true, std::memory_order_release);
    if (!encodeThread_.joinable()) {
        retainQueuedCaptureFrames(frameQueue_, discardedFrames_);
        discardQueuedFrames_.store(false, std::memory_order_release);
        discardCompletedGeneration_.store(discardGeneration, std::memory_order_release);
        return true;
    }
    if (frameEvent_) {
        SetEvent(frameEvent_);
    }
    if (!frameDiscardComplete_.wait_for(
            discardLock,
            kGpuPipelineStopTimeout,
            [this, discardGeneration] {
                return discardCompletedGeneration_.load(std::memory_order_acquire) >= discardGeneration;
            })) {
        Logger::instance().error(
            L"recorder", L"Timed out waiting for the encode loop to discard queued frames");
        return false;
    }
    return true;
}

bool RecorderController::recreateGpuPipeline(
    const CaptureTarget& target,
    const wchar_t* reason,
    uint32_t adapterIndex) {
    Logger::instance().info(L"recorder", std::wstring(L"Recreating GPU pipeline: ") + (reason ? reason : L"unspecified"));

    // No encoder exists from here until the new one initializes successfully.
    encoderZeroReorderDelay_.store(false, std::memory_order_relaxed);
    if (!discardFrameQueueAndWait()) {
        return false;
    }

    std::scoped_lock gpuLock(pipelineGpuMutex_);

    if (encoder_) {
        // Drain the reorder tail before teardown: a B-frame/lookahead session
        // holds submitted frames past the last encodeFrame output, and a bare
        // shutdown strands them (replay gap plus a PTS jump under the fresh
        // keyframe below). Same muxer keyframe gate as the encode-loop drain;
        // both sinks serialize internally, so this is safe off the encode thread.
        std::vector<EncodedPacket> tail;
        encoder_->drain(tail);
        for (auto& packet : tail) {
            publishVideoPacket(std::move(packet));
        }
    }
    auto releaseEncoder = [&]() {
        if (encoder_) {
            // A failed retirement still has driver work referencing capture
            // textures. Keep the whole old pipeline intact instead of freeing
            // them under the driver.
            if (!encoder_->resetInputResources()) {
                Logger::instance().error(L"recorder",
                    L"GPU pipeline recreate aborted; encoder inputs could not be retired");
                return false;
            }
            encoder_->shutdown();
            encoder_.reset();
        }
        std::scoped_lock discardLock(frameDiscardMutex_);
        discardedFrames_.clear();
        return true;
    };
    if (!releaseCaptureFrames(captureLastFrame_, captureAcquiredFrame_, releaseEncoder)) {
        return false;
    }
    scaler_.reset();
    if (capture_) {
        Logger::instance().debug(L"recorder", L"GPU recreate capture shutdown begin");
        capture_->shutdown();
        capture_.reset();
        Logger::instance().debug(L"recorder", L"GPU recreate capture shutdown done");
    }

    if (!d3d_.initialize(adapterIndex)) {
        Logger::instance().error(L"recorder", L"GPU pipeline recreate failed during D3D initialization");
        std::scoped_lock lock(captureStatusMutex_);
        captureBackendActive_ = false;
        captureBackendStatus_ = L"Capture pipeline failed: Direct3D initialization failed";
        return false;
    }
    Logger::instance().debug(L"recorder", L"GPU recreate D3D initialized; creating capture source");

    if (!createCaptureSource(target)) {
        Logger::instance().error(L"recorder", L"GPU pipeline recreate failed because no capture source could initialize");
        return false;
    }

    const uint32_t sourceWidth = capture_->width();
    const uint32_t sourceHeight = capture_->height();
    captureWidth_ = sourceWidth;
    captureHeight_ = sourceHeight;

    AppSettings snapshot = settings();
    // sanitizeSettings owns the even width/height limits; no second clamp here.
    const VideoSettings videoSettings = snapshot.video;
    const GpuOptimizationSettings gpuSettings = snapshot.gpu;
    {
        std::scoped_lock stateLock(stateMutex_);
        activeVideoSettings_ = videoSettings;
        activeGpuSettings_ = gpuSettings;
    }
    encodeWidth_ = videoSettings.width;
    encodeHeight_ = videoSettings.height;
    scaler_.setPoolSize(captureTexturePoolSize(gpuSettings));

    encoder_ = createEncoderForDevice(d3d_);
    if (!encoder_ || !encoder_->initialize(d3d_, videoSettings)) {
        Logger::instance().error(L"recorder", L"GPU pipeline recreate failed during encoder initialization");
        {
            std::scoped_lock lock(captureStatusMutex_);
            captureBackendActive_ = false;
            captureBackendStatus_ += L"; encoder initialization failed";
        }
        if (capture_) {
            capture_->shutdown();
            capture_.reset();
        }
        return false;
    }

    forceVideoHeartbeat_ = true;
    encoder_->requestKeyFrame();
    encoderZeroReorderDelay_.store(
        encoder_->capabilities().effective.zeroReorderDelay,
        std::memory_order_relaxed);
    // The scaler's output pool is the last pool the encoder reads from, so it
    // must hold every input the encoder retains (deep NVENC lookahead pins
    // accepted frames until their output is harvested) plus the queue and the
    // capture loop's held/scale slot. The queue-derived capture pools are
    // intermediate and stay at their size; only this feeding pool grows.
    scaler_.setPoolSize(encoderFeedingPoolSizeFor(
        activeFrameQueueLimit(gpuSettings),
        encoder_->inputRetentionBudget()));
    // A same-size WGC frame aliases the source texture instead of a scaler
    // output, so it pins a source-side slot and never reaches the feeding pool
    // above. Decide once per run, where the encoder's retention is known: an
    // encoder holding more inputs than the source side can pin gets owned
    // copies for the whole run (finding 03).
    const uint32_t aliasCapacity = wgcAliasInputCapacity(activeFrameQueueLimit(gpuSettings));
    const uint32_t encoderRetention = encoder_->inputRetentionBudget();
    const bool aliasFits = encoderAliasFitsRetention(aliasCapacity, encoderRetention);
    encoderAliasFitsRetention_.store(aliasFits, std::memory_order_relaxed);
    if (!aliasFits) {
        Logger::instance().info(L"recorder",
            L"WGC zero-copy aliasing disabled for this pipeline: encoder retains " +
            std::to_wstring(encoderRetention) + L" inputs but the source side can pin only " +
            std::to_wstring(aliasCapacity) + L"; frames are copied into the encoder-feeding pool");
    }
    Logger::instance().info(L"recorder",
        L"GPU pipeline ready: capture=" + std::to_wstring(sourceWidth) + L"x" + std::to_wstring(sourceHeight) +
        L", encode=" + std::to_wstring(videoSettings.width) + L"x" + std::to_wstring(videoSettings.height) +
        L", adapter=" + d3d_.adapterName());
    // Stable-for-the-session environment snapshot, written once per pipeline
    // (re)create so a settings change or device-loss rebuild re-logs the real
    // configuration. These values do not change frame to frame, so a saved
    // log's header carries them instead of per-line noise.
    const auto envCaps = encoder_->capabilities();
    Logger::instance().info(L"recorder",
        L"Environment: gpu=" + d3d_.adapterName() +
        L", encoder=" + (envCaps.backendName.empty() ? L"unknown" : envCaps.backendName) +
        L", available=" + (envCaps.available ? L"yes" : L"no") +
        L", codec=" + codecName(videoSettings.codec) +
        L", resolution=" + std::to_wstring(videoSettings.width) + L"x" + std::to_wstring(videoSettings.height) +
        L"@" + std::to_wstring(videoSettings.fps) +
        L", bitrateKbps=" + std::to_wstring(videoSettings.bitrateKbps) +
        L", gopSeconds=" + std::to_wstring(videoSettings.gopSeconds) +
        L", preset=" + encoderPresetName(envCaps.effective.preset) +
        L", mode=" + encoderModeName(envCaps.effective.mode) +
        L", profile=" + encoderProfileName(envCaps.effective.profile) +
        L", multipass=" + encoderMultipassName(envCaps.effective.multipass) +
        L", bFrames=" + (envCaps.effective.bFrames ? L"yes" : L"no") +
        L", lookahead=" + (envCaps.effective.lookahead ? L"yes" : L"no") +
        L", lookaheadDepth=" + std::to_wstring(envCaps.effective.lookaheadDepth) +
        L", spatialAQ=" + (envCaps.effective.spatialAQ ? L"yes" : L"no") +
        L", aqStrength=" + std::to_wstring(envCaps.effective.aqStrength) +
        L", temporalAQ=" + (envCaps.effective.temporalAQ ? L"yes" : L"no") +
        L", zeroReorderDelay=" + (envCaps.effective.zeroReorderDelay ? L"yes" : L"no") +
        L", hdrMonitor=" + (hdrMonitorActive_.load(std::memory_order_relaxed) ? L"yes" : L"no"));
    return true;
}

bool RecorderController::createCaptureSource(
    const CaptureTarget& target,
    bool exclusiveRecovery) {
    AppSettings snapshot = settings();
    const CaptureBackend selectedBackend = snapshot.preferredCaptureBackend;
    // Sticky-HDR input: what was live before this (re)bind. Read before the
    // reset below overwrites it; a DD session on an HDR monitor retries DD
    // before falling back to WGC so one ACCESS_LOST never flaps the backend.
    CaptureBackend prevActiveBackend = CaptureBackend::WindowsGraphicsCapture;
    {
        std::scoped_lock lock(captureStatusMutex_);
        prevActiveBackend = activeCaptureBackend_;
    }

    {
        std::scoped_lock lock(captureStatusMutex_);
        selectedCaptureBackend_ = selectedBackend;
        activeCaptureBackend_ = selectedBackend;
        captureBackendActive_ = false;
        captureBackendFallbackUsed_ = false;
        captureBackendStatus_ = std::wstring(L"Selected backend: ") + captureBackendDisplayName(selectedBackend) + L"; initializing";
    }

    // Desktop Duplication keeps exclusive ownership of its output. Release an
    // access-lost source before attempting DuplicateOutput for a replacement.
    if (capture_) {
        Logger::instance().info(L"recorder", L"Releasing previous capture source before replacement");
        capture_->shutdown();
        capture_.reset();
    }

    auto tryInitialize = [&](std::unique_ptr<ICaptureSource> source, const wchar_t* name,
                             const CaptureTarget& initTarget) -> std::unique_ptr<ICaptureSource> {
        Logger::instance().info(L"recorder", std::wstring(L"Initializing capture source: ") + name);
        if (source->initialize(d3d_, snapshot, initTarget)) {
            Logger::instance().info(L"recorder", std::wstring(L"Capture source initialized: ") + name);
            return std::move(source);
        }
        Logger::instance().warning(L"recorder", std::wstring(L"Capture source initialization failed: ") + name);
        source->shutdown();
        return {};
    };

    const HMONITOR monitor = monitorForCaptureTarget(target);
    const DxgiOutputLocation location = dxgiOutputForMonitor(monitor);
    const bool dxgiTargetEncodable = location.valid() &&
        dxgiAdapterSupportsHardwareEncode(location.adapterIndex);
    const bool dxgiTargetOnActiveAdapter = dxgiTargetEncodable &&
        location.adapterIndex == d3d_.adapterIndex();
    // Cache the advanced-color probe: stats() derives hdrRejected from it
    // without re-walking DXGI on the diagnostics path. An OS HDR toggle
    // mid-session on WGC stays invisible until the next rebind (WGC frames
    // carry no hdr flag); DD toggles still surface via the per-frame flag.
    const bool monitorHdr = monitor && monitorHasAdvancedColor(monitor);
    hdrMonitorActive_.store(monitorHdr, std::memory_order_relaxed);
    // Sticky while an HDR DD session is live: the rebind path already waited
    // (WaitStable + cooldown), so one immediate DD retry here costs nothing and
    // keeps a transient loss from washing out through a WGC fallback.
    // exclusiveRecovery keeps its monitor-only DD path untouched (not gated here).
    const bool stickyDdRetry = !exclusiveRecovery && monitorHdr &&
        prevActiveBackend == CaptureBackend::DesktopDuplication;

    CaptureTarget monitorTarget = target;
    monitorTarget.window = nullptr;
    CaptureTarget windowTarget = target;
    const bool hasWindowTarget = windowTarget.window && IsWindow(windowTarget.window);
    if (!hasWindowTarget) {
        windowTarget.window = nullptr;
    }

    const CaptureCandidateState candidateState{
        selectedBackend, exclusiveRecovery, hasWindowTarget, monitorHdr, dxgiTargetOnActiveAdapter};
    // DXGI skipped for target on every path that needs it: derived once from
    // the same adapter test the DD guards use, instead of only inside the
    // DesktopDuplication-selected fallback branch.
    bool dxgiSkippedForTarget = selectedBackend == CaptureBackend::DesktopDuplication &&
        !dxgiTargetOnActiveAdapter;
    if (dxgiSkippedForTarget) {
        const wchar_t* detail = !location.valid()
            ? L"monitor has no DXGI output"
            : !dxgiTargetEncodable
                ? L"monitor adapter has no supported hardware encoder"
                : L"target adapter differs from active D3D adapter";
        Logger::instance().warning(L"recorder",
            std::wstring(L"Desktop Duplication unavailable for target; using WGC: ") + detail);
    }

    // Ordered backend candidates: vector order is the selection priority, so a
    // wrong-monitor DXGI bind is impossible by construction (every DD entry
    // carries monitorTarget and runs only when the target output is on the
    // active D3D adapter). Window targeting never overrides an explicit DD
    // choice, exclusive recovery stays monitor-only DD, and the HDR-preferred
    // DD attempt is its own entry rather than nested in the WGC path.
    struct CaptureCandidate {
        const wchar_t* name;
        CaptureTarget target;
        bool (*eligible)(const CaptureCandidateState&);
        std::unique_ptr<ICaptureSource> (*create)();
        const wchar_t* attemptNote = nullptr;
    };
    std::unique_ptr<ICaptureSource> (*makeDd)() = []() -> std::unique_ptr<ICaptureSource> {
        return std::make_unique<DesktopDuplicationCapture>();
    };
    std::unique_ptr<ICaptureSource> (*makeWgc)() = []() -> std::unique_ptr<ICaptureSource> {
        return std::make_unique<WgcCaptureSource>();
    };
    // WGC fallbacks stay off the exclusive path: CreateForWindow against an
    // exclusive-fullscreen surface yields zero frames.
    bool (*wgcWindowFallbackEligible)(const CaptureCandidateState&) =
        [](const CaptureCandidateState& s) -> bool {
        return !s.exclusiveRecovery && captureWgcWindowFallbackEligible(s);
    };
    bool (*wgcMonitorFallbackEligible)(const CaptureCandidateState&) =
        [](const CaptureCandidateState& s) -> bool {
        return !s.exclusiveRecovery && captureWgcMonitorFallbackEligible(s);
    };
    const CaptureCandidate candidates[] = {
        {L"Desktop Duplication", monitorTarget, captureDdExplicitEligible, makeDd},
        {L"Windows Graphics Capture (window)", windowTarget, captureWgcWindowEligible, makeWgc},
        {L"Desktop Duplication", monitorTarget, captureDdRecoveryEligible, makeDd,
            L"Attempting Desktop Duplication recovery"},
        {L"Desktop Duplication", monitorTarget, captureDdHdrPreferredEligible, makeDd,
            L"HDR monitor detected; preferring Desktop Duplication for tone-mapping (WGC fallback if it fails)"},
        {L"Windows Graphics Capture", monitorTarget, captureWgcMonitorEligible, makeWgc},
        {L"Windows Graphics Capture (window)", windowTarget, wgcWindowFallbackEligible, makeWgc},
        {L"Windows Graphics Capture", monitorTarget, wgcMonitorFallbackEligible, makeWgc},
        {L"Desktop Duplication", monitorTarget, captureDdFallbackEligible, makeDd,
            L"WGC failed; attempting Desktop Duplication fallback"},
    };

    std::unique_ptr<ICaptureSource> initialized;
    for (const auto& candidate : candidates) {
        if (!candidate.eligible(candidateState)) {
            continue;
        }
        if (candidate.attemptNote) {
            Logger::instance().info(L"recorder", candidate.attemptNote);
        }
        initialized = tryInitialize(candidate.create(), candidate.name, candidate.target);
        if (!initialized && stickyDdRetry &&
            (candidate.eligible == captureDdHdrPreferredEligible ||
             candidate.eligible == captureDdExplicitEligible)) {
            // One transient DD loss (ACCESS_LOST/INVALID_CALL) must not flap
            // an HDR session to WGC: retry DD once before the WGC fallback below.
            Logger::instance().info(L"recorder",
                L"Retrying Desktop Duplication before WGC fallback (HDR session sticky)");
            initialized = tryInitialize(candidate.create(), candidate.name, candidate.target);
        }
        if (initialized) {
            break;
        }
    }

    // Exclusive recovery: if DXGI failed, leave capture null. Do not thrash WGC.

    if (!initialized) {
        std::scoped_lock lock(captureStatusMutex_);
        captureBackendActive_ = false;
        captureBackendFallbackUsed_ = false;
        captureBackendStatus_ = std::wstring(L"Selected backend: ") + captureBackendDisplayName(selectedBackend) +
            L"; initialization failed";
        return false;
    }

    const CaptureBackend activeBackend = initialized->backend();
    const bool fallbackUsed = selectedBackend != activeBackend;
    {
        std::scoped_lock lock(captureStatusMutex_);
        selectedCaptureBackend_ = selectedBackend;
        activeCaptureBackend_ = activeBackend;
        captureBackendActive_ = true;
        captureBackendFallbackUsed_ = fallbackUsed;
        captureBackendStatus_ = std::wstring(L"Selected backend: ") + captureBackendDisplayName(selectedBackend) +
            L"; active backend: " + captureBackendDisplayName(activeBackend);
        if (fallbackUsed) {
            captureBackendStatus_ += dxgiSkippedForTarget
                ? L" (DXGI unavailable for target adapter)"
                : selectedBackend == CaptureBackend::DesktopDuplication
                    ? L" (fallback after DXGI failed)"
                    : L" (fallback after WGC failed)";
        }
    }

    capture_ = std::move(initialized);
    activeCaptureTarget_ = target;
    return true;
}

uint32_t RecorderController::activeFrameQueueLimit(const GpuOptimizationSettings& gpuSettings) const {
    const uint32_t configured = std::clamp<uint32_t>(
        gpuSettings.frameQueueLimit,
        1,
        static_cast<uint32_t>(frameQueue_.capacity()));

    switch (gpuSettings.adaptiveMode) {
    case GpuAdaptiveMode::Aggressive:
        return std::min<uint32_t>(configured, 2);
    case GpuAdaptiveMode::Conservative:
        return std::min<uint32_t>(configured, 4);
    case GpuAdaptiveMode::Disabled:
        return configured;
    }
    return configured;
}

bool RecorderController::shouldDropForGpuProtection(
    bool duplicateFrame,
    const GpuOptimizationSettings& gpuSettings) const {
    const uint32_t queueDepth = static_cast<uint32_t>(frameQueue_.size());
    const uint32_t queueLimit = activeFrameQueueLimit(gpuSettings);
    if (queueDepth >= queueLimit) {
        return true;
    }

    switch (gpuSettings.adaptiveMode) {
    case GpuAdaptiveMode::Disabled:
        return false;
    case GpuAdaptiveMode::Conservative:
        return duplicateFrame && queueDepth > 0;
    case GpuAdaptiveMode::Aggressive:
        if (duplicateFrame) {
            return queueDepth > 0;
        }
        return queueDepth + 1 >= queueLimit && (capturedFrames_.load() % 2) == 0;
    }
    return false;
}

void RecorderController::captureLoop() {
    try {
        captureLoopImpl();
    } catch (const std::exception& error) {
        LogContext::breadcrumb(L"capture loop exited (crash)");
        Logger::instance().error(L"recorder",
            std::wstring(L"Capture worker crashed; capture degraded: ") + utf8ToWide(error.what()));
    } catch (...) {
        LogContext::breadcrumb(L"capture loop exited (crash)");
        Logger::instance().error(L"recorder", L"Capture worker crashed; capture degraded: unknown exception");
    }
    if (!stopRequested_.load(std::memory_order_acquire)) {
        // Crashed, not stopped. Surface the degradation instead of leaving the
        // UI reporting a healthy backend that no longer delivers frames.
        std::scoped_lock lock(captureStatusMutex_);
        captureBackendActive_ = false;
        captureBackendStatus_ += L"; capture worker stopped after an internal error";
    }
    // Last act of the worker, on every exit path: a single-threaded SPSC queue
    // has exactly one producer, so this flag (not a queue-size check) is what
    // proves no further frame can be pushed. The wakeup covers the case where
    // the encode loop is already waiting with an empty queue.
    captureFinished_.store(true, std::memory_order_release);
    if (frameEvent_) {
        SetEvent(frameEvent_);
    }
}

void RecorderController::captureLoopImpl() {
    setThreadDescriptionSafe(L"Backtrack capture");
    const LogContext::ScopedTask taskContext(L"capture");
    LogContext::breadcrumb(L"capture loop started");
    const HANDLE mmcssHandle = enableMmcssForCaptureThread();
    // Session-scoped: raises the global system timer resolution for the whole
    // capture/encode session, then restores it on loop exit. Without this,
    // coarse cadence waits quantize to the 15.6ms default and 60fps aliases to
    // ~30fps in windowed mode (exclusive fullscreen raises the period for us).
    const HighResolutionTimerScope timerScope;

    AppSettings captureSettings = settings();
    VideoSettings videoSettings;
    GpuOptimizationSettings gpuSettings;
    const auto reloadActiveSettings = [&]() {
        std::scoped_lock stateLock(stateMutex_);
        videoSettings = activeVideoSettings_;
        gpuSettings = activeGpuSettings_;
    };
    reloadActiveSettings();
    GpuFrame& lastFrame = captureLastFrame_;
    GpuFrame& frame = captureAcquiredFrame_;
    auto nextEmit = std::chrono::steady_clock::now();
    auto lastSourceFrameAt = nextEmit;
    auto nextMonitorPoll = std::chrono::steady_clock::now();
    auto nextSoundSeparationPoll = std::chrono::steady_clock::now();
    auto pendingMonitorSince = std::chrono::steady_clock::now();
    auto lastVideoSubmission = SteadyClock::time_point::min();
    auto statsSampleAt = std::chrono::steady_clock::now();
    uint64_t sourceFramesAtLastSample = 0;
    uint64_t timelineIntervalsAtLastSample = 0;
    HMONITOR pendingMonitor = nullptr;
    bool emitClockStarted = false;
    auto lastEmitTime = SteadyClock::time_point::min();
    // Encoder-fault recreate pacing: a fault that repeats before the rebuilt
    // pipeline published a packet is a broken pipeline, not a transient.
    EncoderFaultBackoff encoderFaultBackoff;
    int64_t encoderFaultPtsBaseline = lastEncodedVideoPts100ns_.load(std::memory_order_acquire);
    bool requestKeyFrameAfterAcceptedFrame = false;
    // Recovery state lives in the header-only CaptureSession supervisor; frame
    // data and clocks stay here.
    // Capture session under exclusive fullscreen:
    // Capturing → AccessLost → WaitStable → Rebind (DXGI only) → Capturing | Holding.
    // Never thrash WGC CreateForWindow for exclusive cover (zero frames).
    // Transitions are session.toX(); partial resets write session fields directly.
    CaptureSession session;
    // Cumulative across soft-stall episodes (persists past per-episode resets
    // so a repeatedly-wedging source eventually escalates to full recreate).
    int cumulativeSoftStallRecreates = 0;
    SteadyClock::time_point cumulativeSoftStallWindowStart = SteadyClock::time_point::min();
    SteadyClock::time_point cumulativeSoftStallEscalatedAt = SteadyClock::time_point::min();
    // Pipeline start time for startup-timeout diagnostics and time-to-first-frame.
    const auto pipelineStartedAt = std::chrono::steady_clock::now();
    bool startupStallWarned = false;

    // Tracks capture_->cursorOnlyFrames() from the last delivered frame. A DXGI
    // frame whose counter did not advance carries a real desktop present (proof of
    // composited/capturable content); a cursor-only stale frame advances it.
    uint64_t prevCursorOnlyFrames = 0;
    // Tracks capture_->resolutionChangeCount(); an advance means the source
    // resized in place and the next accepted frame must open a new GOP.
    uint64_t prevResolutionChanges = 0;
    bool pendingWgcResizeDrain = false;
    // Tracks the duplication surface HDR identity (GpuFrame::hdr + format). A
    // mid-session SDR->HDR upgrade flips them in place without a rebind; the next
    // accepted frame must open a new GOP so the tone-map switch never lands
    // mid-GOP. Seeded from the first acquired frame after each rebind. Rapid
    // flaps coalesce into one GOP restart per 2 s window, and a
    // >5-flips/min storm logs one warning per minute window.
    bool prevHdrSeen = false;
    bool prevHdr = false;
    DXGI_FORMAT prevHdrFormat = DXGI_FORMAT_UNKNOWN;
    SteadyClock::time_point lastHdrFlipAt = SteadyClock::time_point::min();
    SteadyClock::time_point hdrFlipWindowStart = SteadyClock::time_point::min();
    uint64_t hdrFlipWindowCount = 0;
    bool hdrFlipStormWarned = false;
    bool exclusiveCoverActive = false;
    // A monitor-sized borderless game remains an exclusive-cover candidate:
    // session.recoveryProbe periodically retries desktop capture while holding
    // so composition resume is detected without waiting for cover geometry.
    // Non-exclusive soft-stall episode tracking (see kSoftStallRecreateAfterMs).
    // min() begin time means no stall is in progress; a delivered frame or any
    // recovery transition ends the episode and re-arms the escalation budget.
    // Capture-target-loss escalation (finding 18): an unplugged or dead target
    // monitor would otherwise be retried forever while the recording holds one
    // frozen frame. After enough failed desktop rebinds, either retarget once
    // to another live display (when one exists) or latch a hard error that
    // surfaces in diagnostics instead of silently freezing.
    // Cursor-capture preference currently applied to the live source; polled
    // once per stats interval so a settings toggle applies without a rebind.
    bool appliedCursorPreference = captureSettings.captureCursor;
    DXGI_FORMAT encoderInputFormat;
    const auto reloadEncoderInputFormat = [&]() {
        std::scoped_lock gpuLock(pipelineGpuMutex_);
        encoderInputFormat = encoder_
            ? encoder_->preferredInputFormat()
            : DXGI_FORMAT_B8G8R8A8_UNORM;
    };
    reloadEncoderInputFormat();
    const auto fps = std::max<uint32_t>(1, videoSettings.fps);
    const auto frameInterval = std::chrono::nanoseconds(1'000'000'000 / fps);
    const int64_t nominalFrameDuration100ns = kHundredNanosecondsPerSecond / fps;
    VideoTimelineScheduler timelineScheduler(nominalFrameDuration100ns);
    const auto keyFrameHeartbeatInterval =
        std::chrono::seconds(std::max<uint32_t>(1, videoSettings.gopSeconds));
    const int64_t keyFrameHeartbeat100ns =
        static_cast<int64_t>(keyFrameHeartbeatInterval.count()) * kHundredNanosecondsPerSecond;
    // Exclusive-blind: coalesce only (no periodic GOP Submit of frozen texture).
    // One IDR on enter hold + on first real live frames after recover.
    auto enterAccessLost = [&](const wchar_t* reason) {
        // toAccessLost is a no-op past Capturing; log only the fresh transition.
        const bool fresh = session.state == CaptureSessionState::Capturing;
        session.toAccessLost(
            std::chrono::steady_clock::now(),
            exclusiveCoverActive ? GetForegroundWindow() : nullptr);
        if (fresh) {
            LogContext::breadcrumb(L"capture session access lost");
            Logger::instance().warning(L"recorder",
                std::wstring(L"Capture session AccessLost: ") + (reason ? reason : L"unspecified"));
        }
    };
    auto enterExclusiveHold = [&](const wchar_t* reason) {
        // toHolding reports the first hold of this cover episode; only then
        // emit the one IDR + log + status line.
        if (session.toHolding()) {
            LogContext::breadcrumb(L"exclusive fullscreen hold");
            forceVideoHeartbeat_ = true; // one IDR of freeze; no periodic slideshow
            Logger::instance().info(L"recorder",
                std::wstring(L"Exclusive fullscreen hold: ") +
                    (reason ? reason : L"desktop APIs blind") +
                    (lastFrame.texture ? L". Holding last frame." : L". Waiting for capture."));
            {
                std::scoped_lock lock(captureStatusMutex_);
                captureBackendStatus_ = lastFrame.texture
                    ? L"Exclusive: frozen (last frame) — game capture needed for live"
                    : L"Exclusive: waiting for capture — game capture needed for live";
            }
        }
    };
    auto idleCoalescingAllowed = [&]() {
        // Stall/hold: always coalesce duplicates (stop slideshow). Do not gate on
        // zeroReorderDelay — that was re-encoding frozen texture every cadence tick.
        if (session.stalled()) {
            return true;
        }
        // Cached mirror of "encoder_ && capabilities().effective.zeroReorderDelay",
        // refreshed by recreateGpuPipeline/stopPipeline. The per-frame
        // capabilities() copy (three wstrings under two mutexes) read exactly one
        // constant-per-build bool.
        return gpuSettings.allowIdleFrameSkipping &&
               encoderZeroReorderDelay_.load(std::memory_order_relaxed);
    };
    const auto monitorPollInterval = std::chrono::milliseconds(100);
    const auto soundSeparationPollInterval = std::chrono::seconds(2);
    // The poll's key list derives from captureSettings alone, which is fixed for
    // this loop; computing it once keeps the 2 s tick from rebuilding it.
    const std::vector<std::wstring> soundSeparationKeys =
        mutedSoundSeparationExecutableKeys(captureSettings);
    const auto monitorSwitchStableInterval = std::chrono::milliseconds(300);
    const auto monitorSwitchCooldown = std::chrono::milliseconds(750);
    auto lastMonitorSwitch = SteadyClock::time_point::min();

    // Emits one timeline interval from the last raw source frame. Scaling
    // happens here - at Submit time, once per encoded frame - not per acquired
    // frame: a 144 Hz source feeding a 60 fps timeline would otherwise pay a
    // full scale/copy for every acquire and throw most of the results away.
    // Coalesced intervals skip the work entirely.
    auto emitFrame = [&](const GpuFrame& rawSource, SteadyClock::time_point emitTime, uint32_t intervalCount) {
        const bool forcedHeartbeat = forceVideoHeartbeat_.exchange(false);
        // Stall/hold: suppress periodic GOP Submit of same texture (slideshow).
        // Forced heartbeat (enter hold / recover) still allowed for seekability.
        const bool periodicHeartbeat =
            !session.stallHold &&
            session.state == CaptureSessionState::Capturing &&
            lastVideoSubmission != SteadyClock::time_point::min() &&
            emitTime - lastVideoSubmission >= keyFrameHeartbeatInterval;
        const VideoTimelineEmission emission = timelineScheduler.plan(
            rawSource.frameIndex,
            steadyTimePoint100ns(emitTime),
            intervalCount,
            idleCoalescingAllowed(),
            forcedHeartbeat,
            periodicHeartbeat,
            keyFrameHeartbeat100ns);

        capturedFrames_.fetch_add(emission.intervalCount);
        if (emission.duplicateFrame) {
            cadenceDuplicateFrames_.fetch_add(emission.intervalCount);
        }
        if (emission.intervalCount > 1) {
            catchUpDuplicateFrames_.fetch_add(emission.intervalCount - 1);
        }

        if (emission.action == VideoTimelineAction::Coalesce) {
            {
                std::scoped_lock lock(durationUpdateMutex_);
                if (pendingDurationPts100ns_ != emission.coalescedFramePts100ns) {
                    pendingDurationPts100ns_ = emission.coalescedFramePts100ns;
                    pendingDuration100ns_ = emission.coalescedFrameDuration100ns;
                } else {
                    pendingDuration100ns_ = std::max(
                        pendingDuration100ns_,
                        emission.coalescedFrameDuration100ns);
                }
                durationUpdatePending_.store(true, std::memory_order_release);
            }
            coalescedIdleIntervals_.fetch_add(emission.intervalCount);
            idleFrameSkips_.fetch_add(emission.intervalCount);
            if (frameEvent_) {
                SetEvent(frameEvent_);
            }
            return;
        }

        GpuFrame encodeFrame;
        {
            std::scoped_lock gpuLock(pipelineGpuMutex_);
            // Zero-copy pass-through: a WGC source hands out frames that
            // already live in their own pool and whose lease the encoder pins
            // until its output is harvested, so a same-format, same-size frame
            // can skip the copy entirely. Desktop Duplication must copy: its
            // surface is only valid between AcquireNextFrame/ReleaseFrame
            // (released inside acquire). The multimonitor stability option
            // also opts into an owned copy per frame.
            //
            // lastFrame pins the WGC lease until a newer source frame replaces it.
            // Non-BGRA encoders may not release raw inputs on registration reset.
            // encoderAliasFitsRetention_ is the per-run verdict from
            // recreateGpuPipeline: an alias pins a source-side slot, so it is only
            // safe while the source side can hold what the encoder retains.
            const bool wgcZeroCopyEligible =
                capture_ &&
                capture_->backend() == CaptureBackend::WindowsGraphicsCapture &&
                gpuSettings.wgcZeroCopy &&
                encoderInputFormat == DXGI_FORMAT_B8G8R8A8_UNORM &&
                encoderAliasFitsRetention_.load(std::memory_order_relaxed);
            const bool forceOwnedTexture =
                !wgcZeroCopyEligible ||
                (captureSettings.multiMonitorSupport && gpuSettings.stableMultimonitorFrames);
            const uint64_t poolGeneration = scaler_.poolGeneration();
            const bool scaled = scaler_.scale(
                    d3d_,
                    rawSource,
                    videoSettings.width,
                    videoSettings.height,
                    encodeFrame,
                    captureSettings.multiMonitorSupport,
                    forceOwnedTexture,
                    encoderInputFormat);
            if (scaler_.poolGeneration() != poolGeneration && encoder_) {
                // The scaler tore down its output pool. Retire the encoder's
                // registrations so stale handles cannot outlive the pool; a
                // failed reset faults the encoder and the capture loop recreates
                // the pipeline.
                encoder_->resetInputResources();
            }
            if (!scaled) {
                ++droppedFrames_;
                Logger::instance().warning(L"recorder", L"Video frame dropped: scaler failed");
                if (forcedHeartbeat) {
                    forceVideoHeartbeat_ = true;
                }
                return;
            }
        }

        GpuFrame frame = std::move(encodeFrame);
        frame.frameIndex = capturedFrames_.load();
        frame.pts100ns = emission.pts100ns;
        frame.duration100ns = emission.duration100ns;
        frame.previousFramePts100ns = emission.previousFramePts100ns;
        frame.previousFrameDuration100ns = emission.previousFrameDuration100ns;
        frame.hasPreviousFrame = emission.hasPreviousFrame;
        if (emission.requestKeyFrame) {
            std::scoped_lock gpuLock(pipelineGpuMutex_);
            if (encoder_) {
                encoder_->requestKeyFrame();
            }
        }
        if (shouldDropForGpuProtection(emission.duplicateFrame, gpuSettings)) {
            if (forcedHeartbeat) {
                forceVideoHeartbeat_ = true;
            }
            ++droppedFrames_;
            ++gpuProtectionDrops_;
            Logger::instance().warning(L"recorder", L"Video frame dropped: GPU protection heuristic");
            return;
        }
        if (!frameQueue_.tryPush(std::move(frame))) {
            // Encoder backpressure, not the heuristic above: the queue filled
            // between its depth check and this push. Merging the two branches
            // back with || would bill the GPU protection for every stall.
            if (forcedHeartbeat) {
                forceVideoHeartbeat_ = true;
            }
            ++droppedFrames_;
            ++frameQueueDrops_;
            Logger::instance().warning(L"recorder", L"Video frame dropped: encoder queue full");
            return;
        }
        timelineScheduler.acceptSubmission(emission);
        lastVideoSubmission = emitTime;
        if (frameEvent_) {
            SetEvent(frameEvent_);
        }
    };

    auto refreshCaptureDimensions = [&]() {
        if (capture_) {
            captureWidth_ = capture_->width();
            captureHeight_ = capture_->height();
        }
    };

    auto releaseSourceFrames = [&]() {
        return releaseCaptureFrames(lastFrame, frame, [&]() {
            if (!discardFrameQueueAndWait()) {
                return false;
            }
            {
                std::scoped_lock gpuLock(pipelineGpuMutex_);
                if (encoder_ && !encoder_->resetInputResources()) {
                    return false;
                }
            }
            {
                std::scoped_lock discardLock(frameDiscardMutex_);
                discardedFrames_.clear();
            }
            return true;
        });
    };

    auto resetVideoHandoff = [&]() {
        discardQueuedFrames_.store(true, std::memory_order_release);
        durationUpdatePending_.store(false, std::memory_order_release);
        {
            std::scoped_lock lock(durationUpdateMutex_);
            pendingDurationPts100ns_ = 0;
            pendingDuration100ns_ = 0;
        }
        if (frameEvent_) {
            SetEvent(frameEvent_);
        }
        {
            std::scoped_lock gpuLock(pipelineGpuMutex_);
            if (encoder_) {
                encoder_->resetInputResources();
            }
            if (capture_) {
                scaler_.reset();
                scaler_.setPoolSize(encoderFeedingPoolSizeFor(
                    activeFrameQueueLimit(gpuSettings),
                    encoder_ ? encoder_->inputRetentionBudget() : 0));
            }
        }
        lastFrame = {};
        emitClockStarted = false;
        timelineScheduler.reset();
        lastVideoSubmission = SteadyClock::time_point::min();
        requestKeyFrameAfterAcceptedFrame = true;
        prevHdrSeen = false;
        // A DD(hdr=true) mirror must not survive the handoff gap: no HDR frame
        // flows again until the new source delivers one. Format resets with it;
        // the flip count stays cumulative per pipeline run for Diagnostics.
        hdrActive_.store(false, std::memory_order_relaxed);
        hdrFormatCode_.store(static_cast<int>(DXGI_FORMAT_UNKNOWN), std::memory_order_relaxed);
    };

    auto switchCaptureTarget = [&](const CaptureTarget& target,
                                    const wchar_t* reason,
                                    bool exclusiveRecovery = false) -> bool {
        const uint32_t targetAdapter = pipelineAdapterForTarget(target);
        if (targetAdapter != d3d_.adapterIndex() || d3d_.isDeviceRemoved() ||
            pipelineEncoderFaulted(encoder_.get())) {
            // No copy is safe on a removed device or from a mapped input.
            if (!recreateGpuPipeline(target, reason, targetAdapter)) {
                return false;
            }
            resetVideoHandoff();
            prevResolutionChanges = capture_ ? capture_->resolutionChangeCount() : 0;
            pendingWgcResizeDrain = false;
            reloadActiveSettings();
            reloadEncoderInputFormat();
            return true;
        }
        // The outgoing source cannot close until the queue and encoder release it.
        if (!releaseSourceFrames()) {
            return false;
        }
        emitClockStarted = false;
        {
            std::scoped_lock gpuLock(pipelineGpuMutex_);
            if (!createCaptureSource(target, exclusiveRecovery) || !capture_) {
                return false;
            }
            prevResolutionChanges = capture_->resolutionChangeCount();
            pendingWgcResizeDrain = false;
            prevHdrSeen = false;
            hdrActive_.store(false, std::memory_order_relaxed);
            hdrFormatCode_.store(static_cast<int>(DXGI_FORMAT_UNKNOWN), std::memory_order_relaxed);
            scaler_.reset();
            scaler_.setPoolSize(encoderFeedingPoolSizeFor(
                activeFrameQueueLimit(gpuSettings),
                encoder_ ? encoder_->inputRetentionBudget() : 0));
        }
        requestKeyFrameAfterAcceptedFrame = true;
        if (frameEvent_) {
            SetEvent(frameEvent_);
        }
        refreshCaptureDimensions();
        // Backend-switch proof: reason plus the resulting status the
        // Diagnostics page shows (carries selected/active names + fallback cause).
        std::wstring switchStatus;
        {
            std::scoped_lock lock(captureStatusMutex_);
            switchStatus = captureBackendStatus_;
        }
        Logger::instance().info(L"recorder", std::wstring(L"Capture source switched: ") + reason + L"; " + switchStatus);
        LogContext::breadcrumb(L"capture source switched");
        return true;
    };

    // A monitor handle stays valid only while its display is attached to the
    // desktop. Re-resolving the handle from its own rect detects unplugging
    // even though stale HMONITOR values keep comparing equal.
    auto monitorAlive = [](HMONITOR monitor) {
        if (!monitor) {
            return false;
        }
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) {
            return false;
        }
        return MonitorFromRect(&info.rcMonitor, MONITOR_DEFAULTTONULL) == monitor;
    };

    while (!stopRequested_) {
        frame = {};
        const auto loopNow = std::chrono::steady_clock::now();
        if (captureSettings.captureSystemAudio &&
            loopNow >= nextSoundSeparationPoll) {
            nextSoundSeparationPoll = loopNow + soundSeparationPollInterval;
            refreshSystemAudioCapture(captureSettings, soundSeparationKeys);
        }

        if (captureSettings.multiMonitorSupport && loopNow >= nextMonitorPoll) {
            nextMonitorPoll = loopNow + monitorPollInterval;
            CaptureTarget focusedTarget = captureTargetForSettings(captureSettings);
            if (!focusedTarget.monitor || focusedTarget.monitor == activeCaptureTarget_.monitor) {
                pendingMonitor = nullptr;
            } else if (focusedTarget.monitor != pendingMonitor) {
                pendingMonitor = focusedTarget.monitor;
                pendingMonitorSince = loopNow;
            } else if (loopNow - pendingMonitorSince >= monitorSwitchStableInterval &&
                       (lastMonitorSwitch == SteadyClock::time_point::min() ||
                        loopNow - lastMonitorSwitch >= monitorSwitchCooldown)) {
                const wchar_t* reason = captureSettings.followMouseMonitor
                    ? L"mouse monitor changed"
                    : L"focused monitor changed";
                if (switchCaptureTarget(focusedTarget, reason)) {
                    lastMonitorSwitch = loopNow;
                    pendingMonitor = nullptr;
                    session.state = CaptureSessionState::Capturing;
                    session.rebindAttempts = 0;
                    session.loggedHold = false;
                    session.lastRebindAt = loopNow;
                    lastSourceFrameAt = loopNow;
                    session.stallHold = true;
                    session.liveSince = SteadyClock::time_point::min();
                    // Target selection was already debounced. Do not hold a valid
                    // first frame behind the recovery gate intended for lost capture.
                    session.acceptFirstLive = true;
                }
            }
        }

        // Missing-capture recovery is handled below with cooldown.

        const auto nowLoop = std::chrono::steady_clock::now();
        exclusiveCoverActive =
            foregroundWindowCoversMonitor(monitorForCaptureTarget(activeCaptureTarget_));

        uint32_t timeoutMs = 16;
        if (lastFrame.texture) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextEmit) {
                timeoutMs = 0;
            } else {
                const auto untilEmitMs = std::chrono::duration_cast<std::chrono::milliseconds>(nextEmit - now).count();
                timeoutMs = static_cast<uint32_t>(std::clamp<int64_t>(untilEmitMs, 1, 16));
            }
        }

        // Keep acquiring while holding. A borderless game still covers its monitor,
        // so cover detection alone cannot tell that exclusive mode has ended.
        // Sustained desktop frames below are the proof that composition resumed.
        const bool skipAcquire =
            pendingWgcResizeDrain ||
            (session.state != CaptureSessionState::Capturing &&
             session.state != CaptureSessionState::Holding) ||
            (capture_ && capture_->isDeviceLost()) ||
            !capture_;
        const bool acquired =
            !skipAcquire && capture_ && capture_->acquireNextFrame(frame, timeoutMs);
        if (capture_) {
            // An in-place source resize (WGC FramePool.Recreate) must open a
            // new GOP: request an IDR so the size jump never lands mid-GOP.
            const uint64_t resolutionChanges = capture_->resolutionChangeCount();
            if (resolutionChanges != prevResolutionChanges) {
                prevResolutionChanges = resolutionChanges;
                requestKeyFrameAfterAcceptedFrame = true;
                if (capture_->backend() == CaptureBackend::WindowsGraphicsCapture) {
                    pendingWgcResizeDrain = true;
                }
                Logger::instance().info(L"recorder",
                    L"Capture source resized in place; keyframe requested at " +
                    std::to_wstring(capture_->width()) + L"x" + std::to_wstring(capture_->height()));
            }
        }

        if (pendingWgcResizeDrain && !d3d_.isDeviceRemoved() &&
            (!encoder_ || encoder_->encoderAvailable())) {
            if (!releaseSourceFrames()) {
                continue;
            }
            pendingWgcResizeDrain = false;
            emitClockStarted = false;
            lastSourceFrameAt = std::chrono::steady_clock::now();
        }

        // Recovery/wait states skip acquireNextFrame entirely. Without a wait
        // here that path spins the loop at full CPU while rebind and
        // stable-wait timers elapse. Pacing keeps stop/discard requests
        // responsive through frameEvent_ and preserves emit-grid timing by
        // sleeping only until the next due emission.
        if (skipAcquire && !stopRequested_.load(std::memory_order_acquire)) {
            const auto nowPace = std::chrono::steady_clock::now();
            const bool emissionDue =
                lastFrame.texture && emitClockStarted && nextEmit <= nowPace;
            uint32_t paceMs = 4;
            if (!emissionDue && lastFrame.texture && emitClockStarted) {
                const auto untilEmitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    nextEmit - nowPace).count();
                paceMs = static_cast<uint32_t>(std::clamp<int64_t>(untilEmitMs, 1, 16));
            }
            if (!emissionDue) {
                if (frameEvent_) {
                    WaitForSingleObject(frameEvent_, paceMs);
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(paceMs));
                }
            }
        }
        if (acquired) {
            lastSourceFrameAt = std::chrono::steady_clock::now();
            // Frames are flowing again: any soft-stall episode is over and the
            // escalation budget re-arms (per-episode counter only).
            // cumulativeSoftStallRecreates persists across episodes.
            session.softStallBeganAt = SteadyClock::time_point::min();
            session.softStallRecreates = 0;
            session.deadTargetFailures = 0;
            // Mid-session SDR->HDR upgrade changes the duplication surface in
            // place (no rebind, no isDeviceLost). A silent change would stretch
            // whitened frames into the running GOP, so open a new one.
            // Frames keep flowing (the scaler tone-maps live per frame); only
            // the GOP restart coalesces, so a flap storm costs one GOP, not
            // one per frame.
            hdrActive_.store(frame.hdr, std::memory_order_relaxed);
            hdrFormatCode_.store(static_cast<int>(frame.format), std::memory_order_relaxed);
            if (!prevHdrSeen) {
                prevHdr = frame.hdr;
                prevHdrFormat = frame.format;
                prevHdrSeen = true;
            } else if (frame.hdr != prevHdr || frame.format != prevHdrFormat) {
                prevHdr = frame.hdr;
                prevHdrFormat = frame.format;
                hdrFlipCount_.fetch_add(1, std::memory_order_relaxed);
                const auto flipNow = SteadyClock::now();
                // Flip-rate guard: >5 flips/min logs one warning per minute
                // window (fixed text except counts; logger rate-limit is backup).
                if (hdrFlipWindowStart == SteadyClock::time_point::min() ||
                    flipNow - hdrFlipWindowStart >= std::chrono::minutes(1)) {
                    hdrFlipWindowStart = flipNow;
                    hdrFlipWindowCount = 1;
                    hdrFlipStormWarned = false;
                } else {
                    ++hdrFlipWindowCount;
                }
                if (hdrFlipWindowCount > 5 && !hdrFlipStormWarned) {
                    hdrFlipStormWarned = true;
                    Logger::instance().warning(L"app",
                        std::wstring(L"HDR flip storm: ") + std::to_wstring(hdrFlipWindowCount) +
                        L" flips in the last minute (backend=" +
                        captureBackendDisplayName(capture_ ? capture_->backend() : CaptureBackend::WindowsGraphicsCapture) +
                        L", format=" + hdrFormatName(frame.format) +
                        L", hdr=" + (frame.hdr ? L"on" : L"off") +
                        L"); coalescing keyframes to one per 2s window");
                }
                // Single GOP per transition: coalesce rapid flips within a 2 s
                // window into the first flip's restart so a surface oscillation
                // does not restart GOPs back to back.
                if (lastHdrFlipAt == SteadyClock::time_point::min() ||
                    flipNow - lastHdrFlipAt >= std::chrono::seconds(2)) {
                    lastHdrFlipAt = flipNow;
                    requestKeyFrameAfterAcceptedFrame = true;
                    Logger::instance().info(L"app",
                        std::wstring(L"HDR state changed mid-session (hdr=") +
                        (frame.hdr ? L"on" : L"off") + L", backend=" +
                        captureBackendDisplayName(capture_ ? capture_->backend() : CaptureBackend::WindowsGraphicsCapture) +
                        L", format=" + hdrFormatName(frame.format) + L"); keyframe requested");
                }
            }
            // Exclusive + stall/hold/blind: desktop WGC/DXGI frames are sparse or
            // stale. Do not promote them into lastFrame (slideshow). Healthy
            // continuous desktop capture under borderless cover still encodes.
            const bool desktopBackend = capture_ != nullptr;
            // WGC captures flip-model / Fullscreen Optimizations fullscreen. Unlike
            // DXGI Desktop Duplication (which can hand back stale frames under true
            // exclusive), a delivered WGC frame is proof the surface is composited
            // and capturable, so it must be treated as live, not frozen/held.
            const bool wgcBackend =
                capture_ &&
                capture_->backend() == CaptureBackend::WindowsGraphicsCapture;
            // DXGI Desktop Duplication also captures flip-model / FSO fullscreen,
            // but (unlike WGC) it can deliver cursor-only stale frames. Per-frame
            // proof: a delivered DXGI frame that did NOT advance the cursor-only
            // counter carries a real desktop present -> surface is composited and
            // capturable, so treat it as live (not frozen/held). A cursor-only
            // stale frame advances the counter and stays frozen. Counter reset on
            // rebind can only yield a harmless one-frame false-negative, never a
            // false-positive that would let stale content through.
            const bool dxgiBackend =
                capture_ &&
                capture_->backend() == CaptureBackend::DesktopDuplication;
            const uint64_t curCursorOnlyFrames =
                capture_ ? capture_->cursorOnlyFrames() : prevCursorOnlyFrames;
            const bool dxgiPresentProof =
                dxgiBackend && curCursorOnlyFrames == prevCursorOnlyFrames;
            prevCursorOnlyFrames = curCursorOnlyFrames;
            const bool recoveryProbeActive =
                session.recoveryProbe &&
                exclusiveCoverActive &&
                desktopBackend;
            const bool freezeDesktopFrame =
                exclusiveCoverActive &&
                desktopBackend &&
                !wgcBackend &&
                !dxgiPresentProof &&
                !recoveryProbeActive &&
                (session.stallHold ||
                 session.exclusiveBlind ||
                 session.exclusiveDxgiAttempts >= 1 ||
                 session.state != CaptureSessionState::Capturing);
            if (freezeDesktopFrame) {
                // Drain acquire only; keep frozen lastFrame + coalesce.
            } else {
                if (session.liveSince == SteadyClock::time_point::min()) {
                    session.liveSince = lastSourceFrameAt;
                }
                ++session.sustainedLive;
                // Healthy only after sustained live frames (not one transient DXGI blip).
                // During a hold, sustained desktop frames prove a monitor-sized
                // borderless window has left exclusive mode.
                constexpr auto kSustainedLiveMs = std::chrono::milliseconds(500);
                constexpr int kSustainedLiveFrames = 10;
                const bool backendOkForExclusive =
                    !exclusiveCoverActive ||
                    wgcBackend ||
                    dxgiPresentProof ||
                    recoveryProbeActive ||
                    session.recoveryProbe;
                const bool sustained =
                    backendOkForExclusive &&
                    session.sustainedLive >= kSustainedLiveFrames &&
                    lastSourceFrameAt - session.liveSince >= kSustainedLiveMs &&
                    !(capture_ && capture_->isDeviceLost());
                ++sourceFrames_;
                cursorOnlyFrames_.store(capture_->cursorOnlyFrames(), std::memory_order_relaxed);
                if (frame.pts100ns <= lastFrame.pts100ns) {
                    frame.pts100ns = steadyNow100ns();
                }
                captureWidth_ = frame.width;
                captureHeight_ = frame.height;

                if (shouldDropForGpuProtection(false, gpuSettings)) {
                    ++droppedFrames_;
                    ++gpuProtectionDrops_;
                    continue;
                }

                if (session.stalled() &&
                    (session.acceptFirstLive || sustained)) {
                    session.toCapturing();
                    forceVideoHeartbeat_ = true;
                    Logger::instance().info(L"recorder", L"Capture session Capturing (live frames resumed)");
                }

                // Store the raw acquired frame. Scaling is deferred to
                // emitFrame (Submit time) so frames dropped by the emit grid
                // never pay a scale/copy. lastFrame's lease pins its capture
                // slot until the next accepted frame replaces it - within the
                // pool budget sized for exactly this pin.
                lastFrame = std::move(frame);
                if (requestKeyFrameAfterAcceptedFrame) {
                    discardQueuedFrames_ = true;
                    {
                        std::scoped_lock gpuLock(pipelineGpuMutex_);
                        if (encoder_) {
                            encoder_->requestKeyFrame();
                        }
                    }
                    requestKeyFrameAfterAcceptedFrame = false;
                    if (frameEvent_) {
                        SetEvent(frameEvent_);
                    }
                }
                if (!emitClockStarted) {
                    nextEmit = std::chrono::steady_clock::now();
                    emitClockStarted = true;
                    Logger::instance().info(L"recorder",
                        L"First captured frame after startup (" +
                        std::to_wstring(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - pipelineStartedAt).count()) +
                        L"ms)");
                } else {
                    // Track A: re-phase the emit grid to the frame's real DWM present
                    // time. The free-running grid otherwise beats against the (jittery,
                    // vsync) source phase, producing a periodic dup+skip micro-hitch even
                    // at matched 60->60. Re-locking makes output PTS track genuine
                    // delivery cadence, killing the beat.
                    //
                    // Use lastFrame.pts100ns (WGC SystemRelativeTime, QPC epoch == MSVC
                    // steady_clock) NOT the drain wall-clock (lastSourceFrameAt): the
                    // drain instant is scheduling-jittered and only trades the structural
                    // beat for arrival jitter. The present time is the evenly-spaced clock.
                    //
                    // Scope-gated: only healthy windowed desktop capture. Exclusive-
                    // fullscreen paths keep the free-running grid (their present
                    // times are already 1:1 with vsync -- no beat to fix, and we
                    // must not perturb the smooth path).
                    const bool relockPhase =
                        desktopBackend &&
                        !exclusiveCoverActive &&
                        session.state == CaptureSessionState::Capturing;
                    // Only snap forward and only past the last emitted instant, so the
                    // emit timeline stays strictly monotonic (advanceVideoCadence emits
                    // at emitTime == nextEmit; a backward snap would double-emit).
                    const auto presentInstant = steadyPointFrom100ns(lastFrame.pts100ns);
                    if (relockPhase &&
                        presentInstant > nextEmit &&
                        presentInstant > lastEmitTime) {
                        nextEmit = presentInstant;
                    }
                }
            }
        }

        // Startup-timeout diagnostic: warn if no frame within 5s.
        if (!emitClockStarted && !startupStallWarned &&
            std::chrono::steady_clock::now() - pipelineStartedAt > std::chrono::seconds(5)) {
            startupStallWarned = true;
            Logger::instance().warning(L"recorder", L"No capture frame within 5s of startup; GPU/capture init may be stalled");
        }

        // Health probes run lock-free. Every pointer read here is only ever
        // mutated on this thread while the pipeline runs (GPU recreation and
        // source rebinds execute on the capture thread; stopPipeline joins it
        // before teardown), so no lock is needed for a stable view.
        // encoderAvailable() is an atomic mirror; the previous
        // pipelineGpuMutex_ + stats() version serialized every iteration of
        // this loop behind in-flight encodes just to read one bool.
        bool captureLost = !capture_ || capture_->isDeviceLost();
        bool deviceRemoved = d3d_.isDeviceRemoved();
        bool encoderFaulted = pipelineEncoderFaulted(encoder_.get());

        if (deviceRemoved || encoderFaulted) {
            CaptureTarget target = captureSettings.multiMonitorSupport
                ? captureTargetForSettings(captureSettings)
                : activeCaptureTarget_;
            Logger::instance().warning(L"recorder",
                deviceRemoved
                    ? L"D3D device removed; recreating full GPU pipeline"
                    : L"Hardware encoder faulted; recreating full GPU pipeline");
            LogContext::breadcrumb(deviceRemoved
                ? L"device removed; recreating pipeline"
                : L"encoder fault; recreating pipeline");
            // A fault after a published packet is transient, so retry fast. A
            // fault that repeats before any output is not: tearing capture down
            // every 500 ms only churns the cursor and the replay, so escalate.
            std::chrono::milliseconds recreateDelay{500};
            if (encoderFaulted) {
                if (lastEncodedVideoPts100ns_.load(std::memory_order_acquire) != encoderFaultPtsBaseline) {
                    encoderFaultBackoff.reset();
                }
                recreateDelay = encoderFaultBackoff.next();
            }
            std::this_thread::sleep_for(recreateDelay);
            if (recreateGpuPipeline(
                    target,
                    deviceRemoved ? L"device removed" : L"encoder fault",
                    pipelineAdapterForTarget(target))) {
                // The drain above may have published the broken pipeline's tail;
                // only packets from the rebuilt pipeline reset the backoff.
                encoderFaultPtsBaseline = lastEncodedVideoPts100ns_.load(std::memory_order_acquire);
                reloadActiveSettings();
                resetVideoHandoff();
                prevResolutionChanges = capture_ ? capture_->resolutionChangeCount() : 0;
                pendingWgcResizeDrain = false;
                reloadEncoderInputFormat();
                if (recording_) {
                    waitingForRecordingKeyFrame_ = true;
                    forceVideoHeartbeat_ = true;
                    std::scoped_lock gpuLock(pipelineGpuMutex_);
                    if (encoder_) {
                        encoder_->requestKeyFrame();
                    }
                }
                session.state = CaptureSessionState::Capturing;
                session.rebindAttempts = 0;
                session.stallHold = true;
                lastSourceFrameAt = std::chrono::steady_clock::now();
            }
        } else if (session.targetLostLatched) {
            // Hard-error hold (finding 18): the capture target is gone and
            // could not be recovered or replaced. Stop rebind churn (which
            // would otherwise retry a dead monitor forever) and hold the last
            // frame while the cadence keeps the timeline continuous. Poll the
            // configured target once per second so replugging recovers.
            session.stallHold = true;
            if (session.nextTargetCheckAt == SteadyClock::time_point::min() ||
                nowLoop >= session.nextTargetCheckAt) {
                session.nextTargetCheckAt = nowLoop + std::chrono::seconds(1);
                const CaptureTarget desired = captureTargetForSettings(captureSettings);
                const bool targetBackOnline =
                    (desired.monitor && monitorAlive(desired.monitor)) ||
                    (desired.window && IsWindow(desired.window));
                if (targetBackOnline) {
                    LogContext::breadcrumb(L"capture display back online");
                    Logger::instance().info(L"recorder",
                        L"Capture display is back online; recovering capture source");
                    session.targetLostLatched = false;
                    session.retargetedAlternate = false;
                    session.deadTargetFailures = 0;
                    session.rebindAttempts = 0;
                    session.lastRebindAt = SteadyClock::time_point::min();
                    session.state = CaptureSessionState::WaitStable;
                    session.accessLostAt = nowLoop;
                    setLastRecordingError({});
                    std::scoped_lock lock(captureStatusMutex_);
                    captureBackendActive_ = false;
                    captureBackendStatus_ = L"Capture display returned; recovering capture source";
                }
            }
        } else {
            // ---- Capture session state machine (exclusive-aware) ----
            constexpr auto kWaitStableMs = std::chrono::milliseconds(1200);
            constexpr auto kRebindCooldownMs = std::chrono::milliseconds(2500);
            constexpr auto kHoldDesktopProbeMs = std::chrono::milliseconds(5000);
            // WGC legitimately remains quiet for unchanged borderless content.
            // Require a sustained stall before treating a full-monitor window
            // as exclusive and entering recovery hold.
            constexpr auto kWgcStallUnderExclusiveMs = std::chrono::milliseconds(2000);
            constexpr auto kExclusiveNoFrameMs = std::chrono::milliseconds(2000);
            constexpr auto kDxgiInstantDeathMs = std::chrono::milliseconds(400);
            constexpr auto kSoftStallMs = std::chrono::milliseconds(750);
            // Non-exclusive frozen-source recovery: a wedged WGC/DXGI source
            // can stop delivering frames without raising Closed/ACCESS_LOST,
            // which would record a freeze-frame forever. After a sustained
            // stall, recreate the capture source - bounded per stall episode
            // and rate-limited by a cooldown so legitimately static content
            // cannot trigger source churn.
            constexpr auto kSoftStallRecreateAfterMs = std::chrono::milliseconds(3000);
            constexpr int kMaxSoftStallRecreates = 3;
            constexpr auto kSoftStallRecreateCooldownMs = std::chrono::milliseconds(10000);
            // A static desktop with no user input is normal WGC silence, not a
            // wedge; skip the churn until the user interacts again. Chosen above
            // the recreate cadence (3 s + 10 s cooldown) so an idle stretch stops
            // recreating after the first attempt.
            constexpr auto kSoftStallCursorIdleMs = std::chrono::milliseconds(10000);
            // Cumulative soft-stall escalation: if a repeatedly-wedging source
            // hits this many recreates within the window, escalate to full
            // pipeline recreate (rebuilds D3D device + DWM session).
            constexpr int kCumulativeSoftStallThreshold = 10;
            constexpr auto kCumulativeSoftStallWindow = std::chrono::minutes(5);
            constexpr auto kCumulativeSoftStallEscalationCooldown = std::chrono::minutes(2);

            // Instant DXGI death after exclusive rebind → hard hold (no loop).
            if (exclusiveCoverActive &&
                session.state == CaptureSessionState::Capturing &&
                captureLost &&
                session.rebindBornAt != SteadyClock::time_point::min() &&
                nowLoop - session.rebindBornAt < kDxgiInstantDeathMs) {
                enterExclusiveHold(L"DXGI died immediately under exclusive");
            } else if (captureLost && session.state == CaptureSessionState::Capturing) {
                // Desktop Duplication reports ACCESS_LOST as exclusive mode takes
                // ownership. Rebind straight to the Present hook; one bad first
                // frame is preferable to freezing the replay for seconds.
                session.immediateRebind = exclusiveCoverActive && capture_ &&
                    capture_->backend() == CaptureBackend::DesktopDuplication;
                enterAccessLost(L"ACCESS_LOST / invalid duplication");
            }

            // WGC under exclusive: one recovery path (no CreateForWindow thrash).
            if (session.state == CaptureSessionState::Capturing &&
                capture_ &&
                capture_->backend() == CaptureBackend::WindowsGraphicsCapture &&
                exclusiveCoverActive &&
                !acquired &&
                nowLoop - lastSourceFrameAt >= kWgcStallUnderExclusiveMs) {
                enterAccessLost(L"WGC stall under exclusive cover");
            }

            if (session.state == CaptureSessionState::AccessLost) {
                // Already proven blind this cover session → skip rebind thrash.
                if (exclusiveCoverActive && session.exclusiveBlind) {
                    enterExclusiveHold(L"still exclusive-blind");
                } else {
                    session.state = CaptureSessionState::WaitStable;
                    session.accessLostAt = nowLoop;
                }
            }

            if (session.state == CaptureSessionState::WaitStable) {
                session.stallHold = true;
                if (session.immediateRebind || nowLoop - session.accessLostAt >= kWaitStableMs) {
                    session.state = CaptureSessionState::Rebind;
                }
            }

            if (session.state == CaptureSessionState::Rebind) {
                session.stallHold = true;
                if (session.lastRebindAt != SteadyClock::time_point::min() &&
                    nowLoop - session.lastRebindAt < kRebindCooldownMs &&
                    !session.immediateRebind) {
                    // Holding lastFrame between rebinds.
                } else {
                    const bool validatedExclusiveTarget =
                        exclusiveCoverActive &&
                        session.exclusiveWindow &&
                        GetForegroundWindow() == session.exclusiveWindow;
                    CaptureTarget target = captureSettings.multiMonitorSupport
                        ? captureTargetForSettings(captureSettings)
                        : activeCaptureTarget_;
                    target.window = validatedExclusiveTarget ? session.exclusiveWindow : nullptr;
                    ++session.rebindAttempts;
                    session.lastRebindAt = nowLoop;
                    // Exclusive + one DXGI death already observed: never thrash DXGI.
                    const bool skipDxgi =
                        validatedExclusiveTarget && session.exclusiveDxgiAttempts >= 1;
                    Logger::instance().warning(L"recorder",
                        L"Capture session Rebind (DXGI) attempt=" +
                        std::to_wstring(session.rebindAttempts) +
                        L", exclusiveCover=" + (exclusiveCoverActive ? L"yes" : L"no") +
                        L", skipDxgi=" + (skipDxgi ? L"yes" : L"no"));
                    LogContext::breadcrumb(L"capture rebind (DXGI) attempted");

                    const bool switched =
                        !skipDxgi &&
                        switchCaptureTarget(
                            target,
                            validatedExclusiveTarget
                                ? L"exclusive capture rebind"
                                : L"desktop capture rebind",
                            validatedExclusiveTarget);
                    if (validatedExclusiveTarget && switched && capture_ &&
                        capture_->backend() == CaptureBackend::DesktopDuplication) {
                        ++session.exclusiveDxgiAttempts;
                    }
                    if (switched) {
                        session.rebindBornAt = nowLoop;
                    }

                    session.stallHold = true;
                    session.liveSince = SteadyClock::time_point::min();
                    session.sustainedLive = 0;
                    lastSourceFrameAt = nowLoop;
                    const bool fastRebind = session.immediateRebind;
                    session.immediateRebind = false;
                    if (switched) {
                        if (fastRebind) {
                            session.acceptFirstLive = true;
                            Logger::instance().info(L"recorder",
                                L"Fast fullscreen transition: immediate Desktop Duplication rebind");
                        }
                        // Desktop mode and provisional DXGI recovery both need
                        // sustained live frames before removing stall hold.
                        session.state = CaptureSessionState::Capturing;
                    } else if (!validatedExclusiveTarget) {
                        // F11 or focus changed during recovery. A stale
                        // exclusive incident must not force a hold.
                        session.state = CaptureSessionState::WaitStable;
                        session.accessLostAt = nowLoop;
                    } else {
                        enterExclusiveHold(L"desktop APIs blind under exclusive");
                    }

                    // Dead-target escalation (finding 18): a desktop rebind
                    // that produced no capture source at all usually means the
                    // target display is unplugged or otherwise gone. Retrying
                    // forever would keep recording a frozen frame, so after
                    // enough failures either switch once to another live
                    // display or latch a hard error that diagnostics surface.
                    if (!switched && !validatedExclusiveTarget) {
                        ++session.deadTargetFailures;
                        constexpr int kMaxDeadTargetRebinds = 4;
                        if (session.deadTargetFailures >= kMaxDeadTargetRebinds &&
                            !session.targetLostLatched) {
                            session.deadTargetFailures = 0;
                            const HMONITOR targetMonitor =
                                activeCaptureTarget_.monitor
                                    ? activeCaptureTarget_.monitor
                                    : monitorFromIndex(activeCaptureTarget_.monitorIndex);
                            bool recovered = false;
                            if (targetMonitor && !monitorAlive(targetMonitor) &&
                                !session.retargetedAlternate) {
                                for (const auto& entry : enumerateMonitors()) {
                                    CaptureTarget alternate;
                                    alternate.monitorIndex = entry.index;
                                    alternate.monitor = monitorFromIndex(entry.index);
                                    if (!alternate.monitor || !monitorAlive(alternate.monitor)) {
                                        continue;
                                    }
                                    Logger::instance().warning(L"recorder",
                                        L"Capture display is gone; switching to live display " +
                                        std::to_wstring(entry.index));
                                    if (switchCaptureTarget(
                                            alternate,
                                            L"capture display lost; switched to a live display")) {
                                        session.retargetedAlternate = true;
                                        session.state = CaptureSessionState::Capturing;
                                        session.stallHold = true;
                                        session.acceptFirstLive = true;
                                        lastSourceFrameAt = nowLoop;
                                        forceVideoHeartbeat_ = true;
                                        {
                                            std::scoped_lock lock(captureStatusMutex_);
                                            captureBackendStatus_ +=
                                                L"; capture display lost, switched to a live display";
                                        }
                                        recovered = true;
                                        break;
                                    }
                                }
                            }
                            if (!recovered && !capture_) {
                                // No alternate display (or every candidate
                                // failed). Latch a hard error instead of
                                // retrying the dead target indefinitely.
                                session.targetLostLatched = true;
                                session.nextTargetCheckAt = SteadyClock::time_point::min();
                                const std::wstring detail =
                                    L"Capture display is unavailable. Reconnect the display "
                                    L"or change the capture monitor in settings.";
                                setLastRecordingError(detail);
                                Logger::instance().error(L"recorder", detail);
                                std::scoped_lock lock(captureStatusMutex_);
                                captureBackendActive_ = false;
                                captureBackendStatus_ = lastFrame.texture
                                    ? L"Capture display unavailable; holding last frame"
                                    : L"Capture display unavailable";
                            }
                        }
                    } else if (switched) {
                        session.deadTargetFailures = 0;
                    }
                }
            }

            if (session.state == CaptureSessionState::Holding) {
                session.stallHold = true;
                if (!exclusiveCoverActive) {
                    Logger::instance().info(L"recorder",
                        L"Exclusive cover ended; rebinding desktop capture");
                    session.rebindAttempts = 0;
                    session.exclusiveDxgiAttempts = 0;
                    session.exclusiveBlind = false;
                    session.exclusiveWindow = nullptr;
                    session.loggedHold = false;
                    session.lastRebindAt = SteadyClock::time_point::min();
                    session.rebindBornAt = SteadyClock::time_point::min();
                    session.sustainedLive = 0;
                    CaptureTarget target = captureSettings.multiMonitorSupport
                        ? captureTargetForSettings(captureSettings)
                        : activeCaptureTarget_;
                    target.window = nullptr;
                    if (switchCaptureTarget(target, L"exclusive cover ended")) {
                        session.state = CaptureSessionState::Capturing;
                        session.stallHold = true;
                        session.liveSince = SteadyClock::time_point::min();
                        lastSourceFrameAt = nowLoop;
                        forceVideoHeartbeat_ = true;
                    } else {
                        session.state = CaptureSessionState::Rebind;
                    }
                } else if (nowLoop - session.lastRebindAt >= kHoldDesktopProbeMs) {
                    // Cheap probe: check whether the exclusive condition truly
                    // changed before re-initializing DD. If the same window
                    // still covers the monitor, stay in Holding.
                    if (exclusiveCoverActive && session.exclusiveWindow &&
                        GetForegroundWindow() == session.exclusiveWindow) {
                        // Still exclusive; skip re-init.
                    } else {
                        // Cover changed or window no longer covers monitor.
                        session.exclusiveBlind = false;
                        session.exclusiveDxgiAttempts = 0;
                        session.rebindAttempts = 0;
                        session.recoveryProbe = true;
                        session.state = CaptureSessionState::Rebind;
                    }
                }
            }

            // Soft stall: coalesce only. Do not thrash from Holding.
            if (session.state == CaptureSessionState::Capturing &&
                !acquired && !captureLost) {
                const auto noFrameFor = nowLoop - lastSourceFrameAt;
                if (exclusiveCoverActive && noFrameFor >= kExclusiveNoFrameMs) {
                    if (session.exclusiveBlind || session.exclusiveDxgiAttempts >= 1) {
                        enterExclusiveHold(L"no live frames under exclusive cover");
                    } else {
                        enterAccessLost(L"no live frames under exclusive cover");
                    }
                } else if (!exclusiveCoverActive && noFrameFor >= kSoftStallMs) {
                    session.stallHold = true;
                    session.sustainedLive = 0;
                    // Escalate a sustained non-exclusive stall: coalescing
                    // alone keeps encoding a freeze-frame forever when the
                    // source wedges without reporting device loss. Recreate
                    // the desktop capture source after the stall persists,
                    // bounded per episode and cooldown-limited.
                    const bool desktopSourceRecoverable = capture_ != nullptr;
                    if (session.softStallBeganAt == SteadyClock::time_point::min()) {
                        session.softStallBeganAt = nowLoop - noFrameFor;
                    }
                    const bool recreateCooldownElapsed =
                        session.lastSoftStallAt == SteadyClock::time_point::min() ||
                        nowLoop - session.lastSoftStallAt >= kSoftStallRecreateCooldownMs;
                    if (desktopSourceRecoverable &&
                        userInputIdleMs() < static_cast<uint64_t>(kSoftStallCursorIdleMs.count()) &&
                        session.softStallRecreates < kMaxSoftStallRecreates &&
                        recreateCooldownElapsed &&
                        nowLoop - session.softStallBeganAt >= kSoftStallRecreateAfterMs) {
                        ++session.softStallRecreates;
                        session.lastSoftStallAt = nowLoop;
                        session.softStallBeganAt = nowLoop;

                        // Cumulative tracking across episodes.
                        const bool windowExpired = cumulativeSoftStallWindowStart != SteadyClock::time_point::min() &&
                            nowLoop - cumulativeSoftStallWindowStart > kCumulativeSoftStallWindow &&
                            cumulativeSoftStallRecreates < kCumulativeSoftStallThreshold;
                        if (windowExpired) {
                            cumulativeSoftStallRecreates = 0;
                            cumulativeSoftStallWindowStart = SteadyClock::time_point::min();
                        }
                        ++cumulativeSoftStallRecreates;
                        if (cumulativeSoftStallWindowStart == SteadyClock::time_point::min()) {
                            cumulativeSoftStallWindowStart = nowLoop;
                        }

                        CaptureTarget target = captureSettings.multiMonitorSupport
                            ? captureTargetForSettings(captureSettings)
                            : activeCaptureTarget_;
                        target.window = nullptr;

                        const bool escalate = cumulativeSoftStallRecreates >= kCumulativeSoftStallThreshold &&
                            nowLoop - cumulativeSoftStallWindowStart <= kCumulativeSoftStallWindow &&
                            (cumulativeSoftStallEscalatedAt == SteadyClock::time_point::min() ||
                             nowLoop - cumulativeSoftStallEscalatedAt >= kCumulativeSoftStallEscalationCooldown);

                        if (escalate) {
                            Logger::instance().info(L"recorder",
                                L"Soft-stall escalation: " +
                                std::to_wstring(cumulativeSoftStallRecreates) +
                                L" recreates in " +
                                std::to_wstring(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    nowLoop - cumulativeSoftStallWindowStart).count()) +
                                L"ms; full pipeline recreate");
                            if (recreateGpuPipeline(target, L"soft-stall escalation",
                                    pipelineAdapterForTarget(target))) {
                                cumulativeSoftStallRecreates = 0;
                                cumulativeSoftStallWindowStart = SteadyClock::time_point::min();
                                cumulativeSoftStallEscalatedAt = nowLoop;
                                lastSourceFrameAt = nowLoop;
                                prevCursorOnlyFrames = 0;
                            }
                        } else {
                            Logger::instance().warning(L"recorder",
                                L"No frames for " +
                                std::to_wstring(
                                    std::chrono::duration_cast<std::chrono::milliseconds>(nowLoop - lastSourceFrameAt).count()) +
                                L"ms without device loss; recreating capture source (attempt " +
                                std::to_wstring(session.softStallRecreates) + L"/" +
                                std::to_wstring(kMaxSoftStallRecreates) +
                                L", cumulative=" + std::to_wstring(cumulativeSoftStallRecreates) + L")");
                            // switchCaptureTarget discards queued frames and arms
                            // a keyframe on the next live frame.
                            if (switchCaptureTarget(target, L"frozen source recovery")) {
                                lastSourceFrameAt = nowLoop;
                                prevCursorOnlyFrames = 0;
                            }
                        }
                    }
                }
            }
        }

        // Emit held-frame cadence until a rebind retires the old source frame.
        // Stall hold forces coalescing so GOP IDRs do not produce a slideshow.
        if (!deviceRemoved && !encoderFaulted && lastFrame.texture && emitClockStarted) {
            const auto now = std::chrono::steady_clock::now();
            if (const auto advance = advanceVideoCadence(
                    now,
                    nextEmit,
                    frameInterval,
                    stopRequested_.load(std::memory_order_acquire))) {
                nextEmit = advance->nextEmit;
                lastEmitTime = advance->emitTime;
                emitFrame(lastFrame, advance->emitTime, advance->intervalCount);
            }
            if (now - nextEmit > std::chrono::seconds(1)) {
                nextEmit = now;
            }
        }

        const auto statsNow = std::chrono::steady_clock::now();
        if (statsNow - statsSampleAt >= std::chrono::seconds(1)) {
            const auto elapsedMs = std::max<int64_t>(1,
                std::chrono::duration_cast<std::chrono::milliseconds>(statsNow - statsSampleAt).count());
            const uint64_t sourceFrames = sourceFrames_.load(std::memory_order_relaxed);
            const uint64_t timelineIntervals = capturedFrames_.load(std::memory_order_relaxed);
            sourceFramesPerSecond_.store(
                (sourceFrames - sourceFramesAtLastSample) * 1000 / static_cast<uint64_t>(elapsedMs),
                std::memory_order_relaxed);
            timelineIntervalsPerSecond_.store(
                (timelineIntervals - timelineIntervalsAtLastSample) * 1000 / static_cast<uint64_t>(elapsedMs),
                std::memory_order_relaxed);
            sourceFramesAtLastSample = sourceFrames;
            timelineIntervalsAtLastSample = timelineIntervals;
            statsSampleAt = statsNow;
            // Cursor-capture preference changes normally arrive with a full
            // pipeline restart; apply them to the live source as well so the
            // setting takes effect immediately even when a rebind is not
            // possible (finding 19).
            const bool cursorPreference = captureCursorMirror_.load(std::memory_order_relaxed);
            if (cursorPreference != appliedCursorPreference && capture_) {
                std::unique_lock gpuLock(pipelineGpuMutex_, std::try_to_lock);
                if (gpuLock.owns_lock()) {
                    capture_->setCursorCaptureEnabled(cursorPreference);
                    appliedCursorPreference = cursorPreference;
                }
            }
        }
    }

    disableMmcssForThread(mmcssHandle);
}

void RecorderController::encodeLoop() {
    try {
        encodeLoopImpl();
    } catch (const std::exception& error) {
        LogContext::breadcrumb(L"encode loop exited (crash)");
        Logger::instance().error(L"recorder",
            std::wstring(L"Encode worker crashed; encoding degraded: ") + utf8ToWide(error.what()));
    } catch (...) {
        LogContext::breadcrumb(L"encode loop exited (crash)");
        Logger::instance().error(L"recorder", L"Encode worker crashed; encoding degraded: unknown exception");
    }
    // Harmless on the normal path (the drain already published this value) and
    // essential on the crash path: recreateGpuPipeline blocks until this
    // generation advances, so GPU teardown must never wait on a dead worker.
    // Publishing under frameDiscardMutex_ pairs with the waiter's predicate
    // check; without the lock a store+notify landing between the check and
    // the sleep would be lost and recreateGpuPipeline could hang forever on
    // this dead worker.
    try {
        std::scoped_lock lock(frameDiscardMutex_);
        const uint64_t requested = discardRequestGeneration_.load(std::memory_order_acquire);
        if (requested > discardCompletedGeneration_.load(std::memory_order_acquire)) {
            retainQueuedCaptureFrames(frameQueue_, discardedFrames_);
        }
        discardCompletedGeneration_.store(requested, std::memory_order_release);
        frameDiscardComplete_.notify_all();
    } catch (const std::exception& error) {
        Logger::instance().error(L"recorder",
            std::wstring(L"Could not retain discarded capture frames: ") + utf8ToWide(error.what()));
    } catch (...) {
        Logger::instance().error(L"recorder", L"Could not retain discarded capture frames");
    }
}

void RecorderController::encodeLoopImpl() {
    setThreadDescriptionSafe(L"Backtrack encode");
    const LogContext::ScopedTask taskContext(L"encode");
    LogContext::breadcrumb(L"encode loop started");
    // MMCSS "Games" keeps the encode loop above background system load. The
    // frame queue drops the newest frames under pressure, so a starved encoder
    // would silently lose the most recent replay/recorded frames exactly when
    // the machine is busy - the frames that matter most for a backtrack clip.
    const HANDLE mmcssHandle = enableMmcssForEncodeThread();

    PendingDurationTracker pendingDurations;
    uint64_t pendingDurationEvictions = 0;
    auto lastEncodeHealthCheck = SteadyClock::now();
    uint64_t lastHealthCheckSubmissions = 0;

    auto discardQueuedFrames = [&](GpuFrame* poppedFrame = nullptr) {
        // Serialize the whole discard with a new request, not just its acknowledgement.
        std::scoped_lock discardLock(frameDiscardMutex_);
        const bool holdFrames = discardRequestGeneration_.load(std::memory_order_acquire) >
            discardCompletedGeneration_.load(std::memory_order_acquire);
        if (holdFrames) {
            retainQueuedCaptureFrames(frameQueue_, discardedFrames_, poppedFrame);
        } else {
            frameQueue_.clear();
        }
        {
            std::scoped_lock lock(durationUpdateMutex_);
            pendingDurationPts100ns_ = 0;
            pendingDuration100ns_ = 0;
            durationUpdatePending_.store(false, std::memory_order_release);
        }
        pendingDurations.clear();
        discardCompletedGeneration_.store(
            discardRequestGeneration_.load(std::memory_order_acquire),
            std::memory_order_release);
        frameDiscardComplete_.notify_all();
    };

    auto extendVideoDuration = [&](int64_t pts100ns, int64_t duration100ns) {
        // No PTS test here: both callers already gate on an explicit validity
        // flag, and 0 is a legal presentation time. Only a zero/negative
        // duration leaves nothing to extend.
        if (duration100ns <= 0) {
            return;
        }
        const size_t evicted = pendingDurations.extend(pts100ns, duration100ns);
        if (evicted > 0) {
            pendingDurationEvictions += evicted;
            if (pendingDurationEvictions == 1 ||
                (pendingDurationEvictions % PendingDurationTracker::kMaxPending) == 0) {
                Logger::instance().warning(L"recorder",
                    L"Encoder output is falling behind; evicted old unmatched video durations count=" +
                    std::to_wstring(pendingDurationEvictions));
            }
        }
        const bool recordingExtended =
            recording_ && muxer_.active() &&
            muxer_.extendLastVideoDuration(pts100ns, duration100ns);
        const bool replayExtended = replay_.extendLastVideoDuration(pts100ns, duration100ns);
        if (recordingExtended || replayExtended) {
            pendingDurations.erase(pts100ns);
        }
    };

    auto consumeDurationUpdate = [&]() {
        int64_t pts100ns = 0;
        int64_t duration100ns = 0;
        {
            std::scoped_lock lock(durationUpdateMutex_);
            if (!durationUpdatePending_.load(std::memory_order_acquire)) {
                return false;
            }
            pts100ns = pendingDurationPts100ns_;
            duration100ns = pendingDuration100ns_;
            pendingDurationPts100ns_ = 0;
            pendingDuration100ns_ = 0;
            durationUpdatePending_.store(false, std::memory_order_release);
        }
        extendVideoDuration(pts100ns, duration100ns);
        return true;
    };

    auto publishPacket = [&](EncodedPacket packet) {
        if (packet.bytes.empty()) {
            return;
        }
        packet.duration100ns = pendingDurations.consume(packet.pts100ns, packet.duration100ns);
        publishVideoPacket(std::move(packet));
    };

    // Hoisted out of the frame loop: encodeFrame clears it, so the previous
    // frame's capacity is reused instead of freed and re-grown every frame.
    std::vector<EncodedPacket> ready;
    while (!videoEncodeLoopDone(
        stopRequested_.load(std::memory_order_acquire),
        captureFinished_.load(std::memory_order_acquire),
        frameQueue_.size() == 0,
        durationUpdatePending_.load(std::memory_order_acquire))) {
        // Periodic health probe: averageEncodeMs is already measured per frame
        // (EncoderCommon EMA) as submit->output latency, whose healthy floor is
        // about one frame period; see encoderBehindBudget for why the probe is
        // gated on real feed rate and a three-period threshold.
        const auto healthNow = SteadyClock::now();
        if (healthNow - lastEncodeHealthCheck >= std::chrono::seconds(5)) {
            // A recreate holding the GPU lock skipped this tick; leave the
            // window open so the probe retries on the next iteration.
            if (const auto healthStats = probeEncoderStats(pipelineGpuMutex_, encoder_)) {
                lastEncodeHealthCheck = healthNow;
                VideoSettings healthVideo;
                {
                    std::scoped_lock lock(stateMutex_);
                    healthVideo = activeVideoSettings_;
                }
                const uint64_t submittedThisWindow =
                    healthStats->submittedFrames - lastHealthCheckSubmissions;
                lastHealthCheckSubmissions = healthStats->submittedFrames;
                if (encoderBehindBudget(healthStats->averageEncodeMs, healthVideo.fps,
                                        submittedThisWindow, 5)) {
                    const double budgetMs =
                        healthVideo.fps > 0 ? 1000.0 / healthVideo.fps : 16.667;
                    Logger::instance().warning(L"recorder",
                        L"Encoder is behind the frame budget (average_encode_ms=" +
                        std::to_wstring(healthStats->averageEncodeMs) + L" budget_ms=" +
                        std::to_wstring(budgetMs * 3.0) + L" fps=" +
                        std::to_wstring(healthVideo.fps) + L")");
                }
            }
        }
        if (discardQueuedFrames_.exchange(false)) {
            discardQueuedFrames();
            continue;
        }

        consumeDurationUpdate();
        if (recordingCutoff_.acknowledgeIfDrained()) {
            notifyRecordingCutoffAcked();
        } else if (recordingCutoff_.stopping()) {
            // A faulted or absent encoder can never publish the pending inputs,
            // so a draining stop must write them off instead of waiting out its
            // timeout. Same try_to_lock read as the health probe: a recreate
            // holds the GPU lock, and its own drain publishes the tail.
            std::unique_lock probeLock(pipelineGpuMutex_, std::try_to_lock);
            if (probeLock.owns_lock() && pipelineEncoderFaulted(encoder_.get()) &&
                recordingCutoff_.abandonPending()) {
                notifyRecordingCutoffAcked();
            }
        }

        GpuFrame frame;
        if (!frameQueue_.tryPop(frame)) {
            if (frameEvent_) {
                WaitForSingleObject(frameEvent_, 16);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
            continue;
        }
        if (discardQueuedFrames_.exchange(false)) {
            discardQueuedFrames(&frame);
            continue;
        }

        recordingCutoff_.noteFrameSubmitted(frame.pts100ns);

        // The preceding submission is present when the flag says so, not when
        // its PTS happens to be nonzero: 0 is a legal presentation time.
        if (frame.hasPreviousFrame) {
            extendVideoDuration(frame.previousFramePts100ns, frame.previousFrameDuration100ns);
        }

        // Vector form: one submit can complete several outputs while
        // reordering/lookahead drains, and the single-packet form strands the
        // tail. publishPacket skips empties, same as the old encoded gate.
        {
            std::scoped_lock gpuLock(pipelineGpuMutex_);
            if (encoder_) {
                // A dropped input produces no packet, so the completion
                // accounting for a pending recording stop has to learn about it
                // here; otherwise the stop waits out its timeout.
                const uint64_t droppedBefore = encoder_->droppedInputs();
                encoder_->encodeFrame(frame, ready);
                if (encoder_->droppedInputs() != droppedBefore) {
                    recordingCutoff_.noteInputDropped();
                }
            }
        }
        for (auto& packet : ready) {
            publishPacket(std::move(packet));
        }
    }

    consumeDurationUpdate();
    std::vector<EncodedPacket> drained;
    {
        std::scoped_lock gpuLock(pipelineGpuMutex_);
        if (encoder_) {
            encoder_->drain(drained);
        }
    }
    for (auto& packet : drained) {
        publishPacket(std::move(packet));
    }
    disableMmcssForThread(mmcssHandle);
}

void RecorderController::publishVideoPacket(EncodedPacket packet) {
    if (packet.bytes.empty()) {
        return;
    }
    const int64_t pts100ns = packet.pts100ns;
    lastEncodedVideoPts100ns_.store(pts100ns, std::memory_order_release);

    // The muxer owns the recording cut (muxer_.setCutPts100ns): it writes the
    // packets at or below the cut and holds the rest, so a stop keeps a
    // decode-order prefix (finding 04). recording_ alone cannot gate this
    // because it is cleared only after the drain.
    if (recording_ && muxer_.active()) {
        if (waitingForRecordingKeyFrame_) {
            if (packet.keyFrame) {
                waitingForRecordingKeyFrame_ = false;
                muxer_.writeVideoPacket(packet);
            }
        } else {
            muxer_.writeVideoPacket(packet);
        }
    }
    replay_.pushVideo(std::move(packet));
    // Every arriving packet - including the held post-cutoff ones - has to
    // settle the completion accounting, or the stop could not tell a slow
    // reorder tail from a lost one.
    if (recordingCutoff_.notePacketPublished(pts100ns)) {
        notifyRecordingCutoffAcked();
    }
}

void RecorderController::notifyRecordingCutoffAcked() {
    // Lock pairs with the waiter's predicate; without it a store+notify
    // landing between the waiter's check and its sleep would be lost.
    std::scoped_lock lock(recordingCutoffMutex_);
    recordingCutoffComplete_.notify_all();
}

void RecorderController::handleAudioPacket(AudioPacket&& packet) {
    const AudioTrack track = packet.track;
    // Liveness: record arrival before the packet can be dropped by a full
    // queue - a starved queue must not read as a dead input. Also clears the
    // silent-input watchdog latch: a packet ends the silent episode.
    (track == AudioTrack::System
        ? systemAudioLastPacketAt100ns_
        : microphoneAudioLastPacketAt100ns_)
        .store(steadyNow100ns(), std::memory_order_relaxed);
    (track == AudioTrack::System
        ? systemAudioSilenceWarned_
        : microphoneAudioSilenceWarned_)
        .store(false, std::memory_order_relaxed);
    SpscQueue<AudioPacket>& queue = track == AudioTrack::System
        ? systemAudioQueue_
        : microphoneAudioQueue_;
    if (!queue.tryPush(std::move(packet))) {
        std::atomic<uint64_t>& drops = track == AudioTrack::System
            ? systemAudioQueueDrops_
            : microphoneAudioQueueDrops_;
        const uint64_t dropped = drops.fetch_add(1, std::memory_order_relaxed) + 1;
        if (dropped == 1 || dropped % 256 == 0) {
            Logger::instance().warning(L"recorder",
                std::wstring(L"Audio queue full; dropped ") +
                (track == AudioTrack::System ? L"system" : L"microphone") +
                L" packet(s): " + std::to_wstring(dropped));
        }
        return;
    }
    if (audioEvent_) {
        SetEvent(audioEvent_);
    }
}

void RecorderController::writeAudioPacket(AudioPacket&& packet) {
    // Canonicalize at ingest: every packet is converted to stereo float32 at
    // the mux rate before it reaches the recording WAV writers or the replay
    // buffer. A WASAPI reconnect (headset swap, default-device change) can
    // silently change the capture mix format; without this conversion the
    // timeline writers would append raw bytes in a new format to streams
    // declared with the old one, producing pitch-shifted audio and duration
    // skew. Unsupported formats cannot be converted and are dropped here
    // instead of corrupting downstream timelines.
    if (!convertAudioPacketToCanonical(packet, audioConvertScratch_)) {
        Logger::instance().warning(L"recorder",
            L"Audio packet dropped: unsupported capture format (track=" +
            std::wstring(packet.track == AudioTrack::System ? L"system" : L"microphone") + L")");
        return;
    }
#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
    if (microphoneDenoiserResetPending_.exchange(false, std::memory_order_relaxed)) {
        microphoneDenoiser_->reset();
    }
    if (packet.track == AudioTrack::Microphone &&
        audioInputNoiseSuppression_.load(std::memory_order_relaxed) &&
        packet.format) {
        const AudioFormatInfo format = parseAudioFormat(*packet.format);
        const size_t produced = applyMicrophoneNoiseSuppression(
            packet, format, *microphoneDenoiser_, microphoneDenoiseScratch_, microphoneDenoiseOutputScratch_);
        if (produced == 0) {
            // Everything is buffered inside the denoiser awaiting a full
            // 480-frame; nothing to write or buffer downstream yet.
            return;
        }
        packet.frameCount = static_cast<uint32_t>(produced);
    }
#endif
    if (microphoneAgcResetPending_.exchange(false, std::memory_order_relaxed)) {
        microphoneAgc_->reset();
    }
    if (packet.track == AudioTrack::Microphone &&
        audioInputAutoGain_.load(std::memory_order_relaxed) &&
        packet.format) {
        const AudioFormatInfo format = parseAudioFormat(*packet.format);
        applyMicrophoneAgc(packet, format, *microphoneAgc_);
    }
    const uint32_t volumePercent = packet.track == AudioTrack::System
        ? audioOutputVolumePercent_.load(std::memory_order_relaxed)
        : audioInputVolumePercent_.load(std::memory_order_relaxed);
    // Packet is canonical float32 here; sanitizeSettings owns the 0-200 range
    // so the gain is percent/100 exactly with no second clamp.
    applyCanonicalGain(packet, static_cast<float>(volumePercent) / 100.0f);

    if (recording_ && muxer_.active()) {
        muxer_.writeAudioPacket(packet);
    }
    replay_.pushAudio(std::move(packet));
}

void RecorderController::audioLoop() {
    try {
        audioLoopImpl();
    } catch (const std::exception& error) {
        LogContext::breadcrumb(L"audio loop exited (crash)");
        Logger::instance().error(L"recorder",
            std::wstring(L"Audio worker crashed; audio degraded: ") + utf8ToWide(error.what()));
    } catch (...) {
        LogContext::breadcrumb(L"audio loop exited (crash)");
        Logger::instance().error(L"recorder", L"Audio worker crashed; audio degraded: unknown exception");
    }
}

void RecorderController::audioLoopImpl() {
    setThreadDescriptionSafe(L"Backtrack audio writer");
    const LogContext::ScopedTask taskContext(L"audio");
    LogContext::breadcrumb(L"audio loop started");
    drainAudioQueues(systemAudioQueue_, microphoneAudioQueue_, audioProducersFinished_, audioEvent_,
        [this](AudioPacket&& packet) { writeAudioPacket(std::move(packet)); });
}

void RecorderController::refreshSystemAudioCapture(const AppSettings& settings,
                                                   const std::vector<std::wstring>& mutedKeys) {
    if (!settings.captureSystemAudio) {
        return;
    }

    std::scoped_lock lock(systemAudioCaptureMutex_);

    // Re-assert on every poll, including while capture is already running: a
    // session that restarted or was replaced, or one the app unmuted itself,
    // would otherwise play excluded audio into the recording.
    SessionMuteApplyResult mutes;
    if (!mutedKeys.empty()) {
        mutes = WasapiCapture::applySessionMutesForExecutables(mutedKeys);
    }

    if (!mutedKeys.empty() && !mutes.exclusionEstablished()) {
        // Privacy gate: the user asked to keep these apps out of the recording,
        // so an unverifiable mute means no system audio at all rather than the
        // audio the user excluded. The poll retries and capture resumes as soon
        // as the exclusion holds again.
        // Unconditional: a capture whose worker is still retrying reports
        // running() == false while its thread is alive, and that thread would
        // reconnect into the recording once the mutes are restored. stop() reaps
        // it, and the keys stay recorded so the next poll re-establishes the
        // exclusion before it starts capture again.
        systemAudio_.stop();
        systemAudioMutedExecutableKeys_ = mutedKeys;
        if (!systemAudioExclusionFailed_) {
            systemAudioExclusionFailed_ = true;
            std::wstring reason;
            if (mutes.journalFailed) {
                reason = L"the mute journal could not be updated";
            } else if (mutes.enumerationFailed) {
                reason = L"audio session enumeration failed";
            } else {
                reason = std::to_wstring(mutes.muteFailures) + L" audio session(s) could not be muted";
            }
            Logger::instance().error(L"recorder",
                L"Sound separation could not establish the requested exclusion (" + reason +
                L"); system audio capture is suspended so excluded audio cannot be recorded");
        }
        return;
    }
    if (systemAudioExclusionFailed_) {
        systemAudioExclusionFailed_ = false;
        Logger::instance().info(L"recorder",
            L"Sound separation exclusion established again; system audio capture can resume");
    }

    const bool keysChanged = mutedKeys != systemAudioMutedExecutableKeys_;
    if (systemAudio_.running() && !keysChanged) {
        return;
    }

    systemAudioMutedExecutableKeys_ = mutedKeys;
    systemAudio_.stop();

    if (mutedKeys.empty()) {
        restoreSessionMutes(L"for the next attempt");
    } else {
        Logger::instance().info(L"recorder",
            L"Sound separation muting " + std::to_wstring(mutes.muted) +
            L" audio session(s) across " + std::to_wstring(mutedKeys.size()) + L" executable(s)");
    }

    // Capture full system mix; muted apps are silenced at the session level so
    // multiple apps can be excluded (process-loopback exclude only supports one PID).
    const bool started = systemAudio_.start(
        AudioTrack::System,
        settings.audioOutputDeviceId,
        [this](AudioPacket&& packet) { handleAudioPacket(std::move(packet)); });

    if (!started) {
        Logger::instance().warning(L"recorder", L"System audio capture did not start");
        // start() can fail while its worker keeps retrying to reconnect, so the
        // object is still in use even though running() is false. Reap that thread
        // before the mutes below are restored, or the reconnect records the mix
        // the user excluded. The keys stay recorded so the next poll re-applies
        // the exclusion first.
        systemAudio_.stop();
        restoreSessionMutes(L"for the next launch");
    }
}

std::filesystem::path RecorderController::nextClipPath(const wchar_t* prefix, bool organizeIntoSubfolder) const {
    const auto snapshot = settings();
    const auto gameDirectory = ClipStorage::outputDirectory(
        snapshot.clipDirectory, prefix ? prefix : L"", organizeIntoSubfolder);
    std::filesystem::path directory = gameDirectory;
    if (directory != snapshot.clipDirectory) {
        std::error_code directoryError;
        std::filesystem::create_directories(directory, directoryError);
        if (directoryError || !ClipStorage::isDirectoryWithoutReparsePoint(directory)) {
            Logger::instance().warning(L"recorder", std::wstring(L"Could not use game clip folder, saving to the clip folder instead: ") +
                                       directory.wstring());
            directory = snapshot.clipDirectory;
        }
    }
    const auto output = ClipStorage::reserveClipOutput(directory / makeTimestampedFileName(prefix, L".mp4"));
    if (output.empty()) {
        Logger::instance().warning(L"recorder", L"Could not reserve a clip name in " + directory.wstring());
    }
    return output;
}

void RecorderController::setLastRecordingError(std::wstring error) {
    std::scoped_lock lock(recordingErrorMutex_);
    lastRecordingError_ = std::move(error);
}

} // namespace backtrack
