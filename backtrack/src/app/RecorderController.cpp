#include "app/RecorderController.h"
#include "app/VideoTimelineScheduler.h"

#include "audio/AudioConvert.h"
#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
#include "audio/RnnoiseDenoiser.h"
#endif
#include "capture/DesktopDuplicationCapture.h"
#include "capture/WgcCaptureSource.h"
#include "core/Logger.h"
#include "mux/RecoveryStore.h"
#include "platform/Win32Util.h"
#include "settings/SettingsStore.h"
#include "ui/UiHelpers.h"

#include <mmreg.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <cstring>
#include <deque>
#include <limits>
#include <thread>
#include <unordered_set>
#include <utility>

namespace backtrack {

namespace {

// How long a pipeline worker may take to reach a checkpoint that the caller can
// observe, before the caller gives up on it and carries on. Applies to waits that
// have an abort path (recreateGpuPipeline's encode-loop discard). The stop path's
// thread joins deliberately do NOT use it: std::thread::join has no timed form and
// abandoning a wedged vendor call would destroy the user's clip.
constexpr auto kGpuPipelineStopTimeout = std::chrono::seconds(30);

int64_t steadyNow100ns() {
    return std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int64_t steadyTimePoint100ns(SteadyClock::time_point time) {
    return std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>>(
               time.time_since_epoch())
        .count();
}

// Inverse of steadyTimePoint100ns. WGC SystemRelativeTime and MSVC steady_clock
// are both QPC-derived (same epoch), so a source present time in 100ns units maps
// back to a steady_clock instant for emit-grid phase locking.
SteadyClock::time_point steadyPointFrom100ns(int64_t value100ns) {
    return SteadyClock::time_point(
        std::chrono::duration_cast<SteadyClock::duration>(
            std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>(value100ns)));
}

uint32_t captureTexturePoolSize(const GpuOptimizationSettings& settings) {
    // +1 for lastFrame pin in captureLoop (keeps one scaler/capture slot in use).
    return std::clamp<uint32_t>(settings.frameQueueLimit + 4, 5, 16);
}

CaptureTarget captureTargetForSettings(const AppSettings& settings) {
    CaptureTarget target;
    target.monitorIndex = settings.monitorIndex;
    if (settings.multiMonitorSupport && settings.followFocusedMonitor) {
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

CaptureBackend selectedBackendForSettings(const AppSettings& settings) {
    return settings.preferredCaptureBackend;
}

VideoSettings activeVideoSettingsFor(const AppSettings& settings) {
    // sanitizeSettings already clamps width/height even 16-7680/4320; no second
    // clamp here so the sanitizer stays the sole authority.
    return settings.video;
}

enum class OutputNameReservation {
    Reserved,
    AlreadyExists,
    Failed,
};

// Atomically claims a clip output name by creating it as an empty hidden file
// with fail-if-exists semantics and closing the handle again. The placeholder
// holds the name from selection until the finished MP4 is published, so no
// concurrent saver or external process can take the same name in between;
// publishing consumes only such empty reservations and never overwrites real
// content (see Mp4Muxer's staged publish).
OutputNameReservation reserveClipOutputName(const std::filesystem::path& path) {
    const HANDLE handle = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_HIDDEN,
        nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        return OutputNameReservation::Reserved;
    }
    const DWORD error = GetLastError();
    return (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
        ? OutputNameReservation::AlreadyExists
        : OutputNameReservation::Failed;
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

bool hasMutedSoundSeparationApps(const AppSettings& settings) {
    if (!settings.soundSeparationEnabled) {
        return false;
    }
    return std::any_of(
        settings.soundSeparationApps.begin(),
        settings.soundSeparationApps.end(),
        [](const AppSettings::SoundSeparationApp& app) {
            return app.muted && !app.executablePath.empty();
        });
}

} // namespace

// Canonical float gain helper for the mic AGC path: packets are canonical by
// the time AGC/volume run, so integer PCM scaling is dead. AudioConvert owns
// the pure mute/gain primitive; this thin wrapper reuses it.
void applyCanonicalGain(AudioPacket& packet, float gain) {
    if (gain == 1.0f || packet.bytes.empty()) {
        return;
    }
    auto* floats = reinterpret_cast<float*>(packet.bytes.data());
    applyMuteGain(
        std::span<float>(floats, packet.bytes.size() / sizeof(float)), gain, false);
}

// Voice-oriented automatic gain control. Keeps a slow level envelope and drives
// a smoothed gain so average speech lands near a target RMS, mirroring the AGC
// Discord applies to quiet microphone streams. Slow attack/release prevents
// pumping on individual words; the gain itself is time-smoothed as well.
// Forward-declared in RecorderController.h; defined here at backtrack scope so
// the class is visible where the unique_ptr member is constructed/destroyed.
constexpr float kAgcFloorDb = -90.0f;

class MicrophoneAgc {
public:
    void reset() {
        envelopeDb_ = -90.0f;
        gainDb_ = 0.0f;
    }

    // Advances the envelopes with this block's RMS (dBFS) and duration and
    // returns the current smoothed linear gain to apply to the block.
    float gainFor(float rmsDb, float blockSeconds) {
        const float levelDb = std::max(rmsDb, -90.0f);
        if (blockSeconds <= 0.0f) {
            return linearGain();
        }

        // Fast attack on loud content, slow release when the mic goes quiet.
        constexpr float kAttackTauSeconds = 0.03f;
        constexpr float kReleaseTauSeconds = 0.40f;
        const float envelopeTau = levelDb > envelopeDb_ ? kAttackTauSeconds : kReleaseTauSeconds;
        const float envelopeCoeff = 1.0f - std::exp(-blockSeconds / envelopeTau);
        envelopeDb_ += (levelDb - envelopeDb_) * envelopeCoeff;

        // Bring the envelope to the target speech RMS. Boost up to +30 dB (the
        // same range as a hardware "Microphone Boost"); only cut very loud mics.
        constexpr float kTargetRmsDb = -22.0f;
        constexpr float kMaxBoostDb = 30.0f;
        constexpr float kMaxCutDb = 6.0f;
        const float desiredGainDb = std::clamp(kTargetRmsDb - envelopeDb_, -kMaxCutDb, kMaxBoostDb);

        // Smooth the gain itself so level changes do not pump on single words.
        constexpr float kGainTauSeconds = 0.15f;
        const float gainCoeff = 1.0f - std::exp(-blockSeconds / kGainTauSeconds);
        gainDb_ += (desiredGainDb - gainDb_) * gainCoeff;
        return linearGain();
    }

private:
    float linearGain() const {
        return std::pow(10.0f, gainDb_ / 20.0f);
    }

    float envelopeDb_ = -90.0f;
    float gainDb_ = 0.0f;
};

struct BlockLevels {
    float rmsDb = -90.0f;
    float peakDb = -90.0f;
};

BlockLevels measureBlockLevels(const AudioPacket& packet, const AudioFormatInfo& format) {
    BlockLevels levels;
    const uint16_t bytesPerSample = format.bitsPerSample / 8;
    if (bytesPerSample == 0 || packet.bytes.empty()) {
        return levels;
    }

    // Intentional RMS pool in double: float accumulation loses quiet-mic
    // resolution over long blocks; the sum itself never allocates.
    uint64_t sampleCount = 0;
    double sumOfSquares = 0.0;
    float peak = 0.0f;
    const uint8_t* data = packet.bytes.data();
    const size_t byteCount = packet.bytes.size();
    for (size_t offset = 0; offset + bytesPerSample <= byteCount; offset += bytesPerSample) {
        // Single decode path (8/16/24/32 + float32) owned by AudioConvert;
        // no hand-rolled integer magic here.
        const float value = decodeInterleavedSample(data + offset, format);
        const float magnitude = std::fabs(value);
        peak = std::max(peak, magnitude);
        sumOfSquares += static_cast<double>(value) * value;
        ++sampleCount;
    }
    if (sampleCount == 0) {
        return levels;
    }
    const double rms = std::sqrt(sumOfSquares / static_cast<double>(sampleCount));
    levels.rmsDb = rms > 0.0 ? static_cast<float>(20.0 * std::log10(rms)) : kAgcFloorDb;
    levels.peakDb = peak > 0.0f ? 20.0f * std::log10(peak) : kAgcFloorDb;
    return levels;
}

void applyMicrophoneAgc(AudioPacket& packet, const AudioFormatInfo& format, MicrophoneAgc& agc) {
    const uint16_t bytesPerSample = format.bitsPerSample / 8;
    const float blockSeconds =
        bytesPerSample > 0 && format.channels > 0 && format.samplesPerSec > 0
            ? static_cast<float>(packet.bytes.size()) /
                  (static_cast<float>(bytesPerSample) * static_cast<float>(format.channels) *
                   static_cast<float>(format.samplesPerSec))
            : 0.010f;
    const BlockLevels levels = measureBlockLevels(packet, format);
    const float gain = agc.gainFor(levels.rmsDb, blockSeconds);
    // Packets are canonical float32 by the time AGC runs, so the old integer
    // PCM scaling is dead; AudioConvert owns the single gain primitive.
    applyCanonicalGain(packet, gain);
}

#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
// Noise suppression runs after canonicalization, so the packet is guaranteed
// stereo float32 at 48 kHz; the guard keeps an unexpected format from being
// misinterpreted. The stereo mic signal is folded to mono (channels are
// near-identical for voice capture), denoised, and written back to both
// channels. RNNoise consumes 480-sample frames, so a call may emit up to one
// buffered frame more or fewer samples than it consumed: the helper resizes
// the packet payload to match what was actually produced, and held-back
// samples arrive in a later packet, keeping cumulative stream length exact.
// Returns the produced mono-sample count.
size_t applyMicrophoneNoiseSuppression(
    AudioPacket& packet,
    const AudioFormatInfo& format,
    RnnoiseDenoiser& denoiser,
    std::vector<float>& scratchMonoIn,
    std::vector<float>& scratchMonoOut) {
    const size_t frameCount = packet.bytes.size() / (2 * sizeof(float));
    if (!isCanonicalAudioFormat(format) || !denoiser.valid() || frameCount == 0) {
        static bool loggedUnsupportedOnce = false;
        if (!loggedUnsupportedOnce && !isCanonicalAudioFormat(format)) {
            loggedUnsupportedOnce = true;
            Logger::instance().warning(L"recorder", L"Microphone noise suppression skipped non-canonical audio packet");
        }
        return frameCount; // Bypass: everything "produced" unprocessed.
    }

    scratchMonoIn.resize(frameCount);
    const float* interleaved = reinterpret_cast<const float*>(packet.bytes.data());
    for (size_t frame = 0; frame < frameCount; ++frame) {
        // Average the channels: mono sources duplicate to L/R in canonical
        // conversion, so averaging avoids a spurious +6 dB level shift.
        scratchMonoIn[frame] = 0.5f * (interleaved[2 * frame] + interleaved[2 * frame + 1]);
    }

    // +480 lets the denoiser flush any frame buffered by an earlier odd-sized
    // packet instead of accumulating backlog.
    scratchMonoOut.resize(frameCount + 480);
    const size_t produced =
        denoiser.process(scratchMonoIn.data(), frameCount, scratchMonoOut.data(), scratchMonoOut.size());

    packet.bytes.resize(produced * 2 * sizeof(float));
    float* out = reinterpret_cast<float*>(packet.bytes.data());
    for (size_t frame = 0; frame < produced; ++frame) {
        out[2 * frame] = scratchMonoOut[frame];
        out[2 * frame + 1] = scratchMonoOut[frame];
    }
    return produced;
}
#endif // BACKTRACK_HAS_NOISE_SUPPRESSION

RecorderController::RecorderController()
    : frameQueue_(8),
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
    WasapiCapture::clearSessionMutes();
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
    // Also captured for the same reason. Only swept when disk backing is off:
    // an enabled ring is truncated and reused by openRingLocked on the next
    // configure, so there is nothing to reclaim and nothing is open to race.
    const std::filesystem::path ringDirectory = replayRingDirectoryFor(snapshot);
    const bool sweepOrphanedRing = !snapshot.replay.diskBackedVideo && !snapshot.replay.diskBackedAudio;
    Logger::instance().info(L"app", L"Startup recovery sweep started in background");
    startupSweepThread_ = std::thread([clipDirectory, ringDirectory, sweepOrphanedRing]() {
        try {
            if (sweepOrphanedRing) {
                ReplayBuffer::removeOrphanedRing(ringDirectory);
            }
            RecoveryStore::sweepOrphanedRecoveryData(clipDirectory);
        } catch (const std::exception& error) {
            Logger::instance().warning(L"app", std::wstring(L"Startup recovery sweep failed: ") + utf8ToWide(error.what()));
        } catch (...) {
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
    // Sweep crash-recovery leftovers from previous sessions in the background:
    // recover the newest valid manifest into a clip, then delete older
    // orphaned temporary data. The single-instance mutex guarantees no other
    // concurrent session; recording/recovery actions below wait for the sweep,
    // so no new recovery state can be created while it runs.
    beginStartupSweep();
    return true;
}

bool RecorderController::updateSettings(AppSettings settings) {
    settings = sanitizeSettings(std::move(settings));
    bool startReplayPipeline = false;
    {
        std::scoped_lock lock(stateMutex_);
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
        AppSettings currentNoReplay = currentWithoutVoice;
        currentNoReplay.replay = {};
        currentNoReplay.replayRingDirectory.clear();
        AppSettings incomingNoReplay = settings;
        incomingNoReplay.replay = {};
        incomingNoReplay.replayRingDirectory.clear();
        if (currentNoReplay == incomingNoReplay &&
            currentWithoutVoice.replay.enabled == settings.replay.enabled &&
            pipelineRunning_.load()) {
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
        AppSettings currentBase = currentWithoutVoice;
        currentBase.audioOutputVolumePercent = 0;
        currentBase.audioInputVolumePercent = 0;
        currentBase.audioInputAutoGainEnabled = false;
#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
        currentBase.audioInputNoiseSuppressionEnabled = false;
#endif
        currentBase.captureCursor = false;
        AppSettings incomingBase = settings;
        incomingBase.audioOutputVolumePercent = 0;
        incomingBase.audioInputVolumePercent = 0;
        incomingBase.audioInputAutoGainEnabled = false;
#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
        incomingBase.audioInputNoiseSuppressionEnabled = false;
#endif
        incomingBase.captureCursor = false;
        // startReplayPipeline means nothing actually changed (voice branch fell
        // through): let it reach ensurePipeline below instead of returning here.
        if (!startReplayPipeline && currentBase == incomingBase) {
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
    videoTimelineEndPts100ns_ = 0;
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
    const auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (muxer_.videoPacketCount() > 0 &&
           videoTimelineEndPts100ns_.load(std::memory_order_acquire) < stopPts100ns &&
           std::chrono::steady_clock::now() < drainDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Mirror, not settings(): this path needs one bool, not a whole struct
    // copy with paths and vectors.
    if (!replayEnabledMirror_.load(std::memory_order_relaxed)) {
        stopPipeline();
    }

    recording_ = false;
    waitingForRecordingKeyFrame_ = false;
    auto output = muxer_.finalize();
    setLastRecordingError(output.empty() ? muxer_.lastError() : std::wstring());
    Logger::instance().info(L"recorder", output.empty()
                                ? std::wstring(L"Recording stopped without a finalized clip")
                                : std::wstring(L"Recording stopped and saved: ") + output.wstring());
    return output;
}

std::filesystem::path RecorderController::recoverFailedRecording() {
    Logger::instance().info(L"recorder", L"Recover failed recording requested");
    // Serialize with the background startup sweep: both pick and delete the
    // newest recovery manifest, so overlapping runs could recover it twice.
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
        while (replay_.videoKeyFrameCount() == 0 ||
               replay_.videoSpan100ns() < minReplayVideo100ns ||
               bufferedVideoStale()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
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
    VideoSettings videoSettings;
    {
        std::scoped_lock lock(stateMutex_);
        videoSettings = activeVideoSettings_;
    }
    Logger::instance().info(L"recorder", L"Replay saveTo begin: " + output.wstring());
    std::wstring saveDetail;
    const bool saved = replay_.saveTo(output, videoSettings, &saveDetail);
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
        if (gpuLock.owns_lock() && encoder_) {
            lastEncoderStats_.store(std::make_shared<const EncoderStats>(encoder_->stats()));
        }
        // The scaler is only touched under pipelineGpuMutex_ (both scale()
        // sites and every reset()), so this won-lock read of its sticky HDR
        // rejection is race-free and still never blocks the UI thread.
        if (gpuLock.owns_lock()) {
            scalerHdrRejected = scaler_.hdrInputRejected();
        }
        stats.encoder = *lastEncoderStats_.load();
    }
    // A driver that refused tone-mapping still sinks HDR frames, so the scaler
    // latch counts only alongside live HDR input (gating also keeps a stale
    // latch from painting SDR content as rejected); the monitor probe covers
    // the WGC/SDR-interpretation case instead.
    stats.hdrRejected = (stats.hdrActive && scalerHdrRejected) || (hdrMonitor && !stats.hdrActive);
    stats.encoder.queueDepth = static_cast<uint32_t>(frameQueue_.size());
    detectVideoPipelineStall();
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
                            L", followFocusedMonitor=" + (snapshot.followFocusedMonitor ? L"yes" : L"no") +
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
    discardQueuedFrames_ = false;
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
    // Audio-liveness clocks start at pipeline start so a never-delivering
    // input shows a growing silence age instead of 0.
    systemAudioLastPacketAt100ns_.store(steadyNow100ns(), std::memory_order_relaxed);
    microphoneAudioLastPacketAt100ns_.store(steadyNow100ns(), std::memory_order_relaxed);
    lastEncodedVideoPts100ns_ = 0;
    videoTimelineEndPts100ns_ = 0;

    if (snapshot.captureSystemAudio) {
        refreshSystemAudioCapture(snapshot);
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
    captureThread_ = std::thread(&RecorderController::captureLoop, this);
    encodeThread_ = std::thread(&RecorderController::encodeLoop, this);
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
        Logger::instance().info(L"recorder", L"Waiting for encode worker to stop");
        encodeThread_.join();
        Logger::instance().info(L"recorder", L"Encode worker stopped");
    }
    Logger::instance().debug(L"recorder", L"Capture/encode threads joined");

    systemAudio_.stop();
    microphoneAudio_.stop();
    Logger::instance().info(L"recorder", L"Capture/encode pipeline audio stopped");
    if (audioEvent_) {
        SetEvent(audioEvent_);
    }
    if (audioThread_.joinable()) {
        Logger::instance().info(L"recorder", L"Waiting for audio writer to stop");
        audioThread_.join();
        Logger::instance().info(L"recorder", L"Audio writer stopped");
    }
    WasapiCapture::clearSessionMutes();
    {
        std::scoped_lock lock(systemAudioCaptureMutex_);
        systemAudioMutedExecutableKeys_.clear();
    }
    // Drop any remaining GpuFrame leases before capture shutdown so WGC
    // Direct3D11CaptureFrame refs are released first.
    frameQueue_.clear();
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
        std::to_wstring(finalStats.encoder.encodedBytes));

    {
        std::scoped_lock gpuLock(pipelineGpuMutex_);
        encoderZeroReorderDelay_.store(false, std::memory_order_relaxed);
        if (encoder_) {
            encoder_->shutdown();
            encoder_.reset();
        }
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

bool RecorderController::recreateGpuPipeline(
    const CaptureTarget& target,
    const wchar_t* reason,
    uint32_t adapterIndex) {
    Logger::instance().info(L"recorder", std::wstring(L"Recreating GPU pipeline: ") + (reason ? reason : L"unspecified"));

    const uint64_t discardGeneration =
        discardRequestGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
    // No encoder exists from here until the new one initializes successfully.
    encoderZeroReorderDelay_.store(false, std::memory_order_relaxed);
    discardQueuedFrames_.store(true, std::memory_order_release);
    if (frameEvent_) {
        SetEvent(frameEvent_);
    }

    // Only encodeLoop consumes frameQueue_. Drain old-device frames before D3D teardown.
    if (encodeThread_.joinable()) {
        Logger::instance().debug(L"recorder", L"GPU recreate waiting for encode-loop frame discard");
        std::unique_lock discardLock(frameDiscardMutex_);
        // The encode loop reaches its loop top within one 16 ms wait, so this
        // deadline only trips when it is wedged inside a vendor call or a
        // synchronous ring write. Aborting the recreate keeps the capture loop
        // responsive and leaves the thread untouched for the later join.
        if (!frameDiscardComplete_.wait_for(
                discardLock,
                kGpuPipelineStopTimeout,
                [this, discardGeneration] {
                    return discardCompletedGeneration_.load(std::memory_order_acquire) >= discardGeneration;
                })) {
            Logger::instance().error(
                L"recorder",
                L"GPU recreate timed out waiting for the encode loop to discard queued frames");
            return false;
        }
        Logger::instance().debug(L"recorder", L"GPU recreate encode-loop discard complete");
    } else {
        // Settings/stop path already joined encodeThread_; drop any leftover frames.
        frameQueue_.clear();
        Logger::instance().debug(L"recorder", L"GPU recreate skipped discard wait (encode thread not running)");
    }

    std::scoped_lock gpuLock(pipelineGpuMutex_);

    if (encoder_) {
        encoder_->shutdown();
        encoder_.reset();
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
    const VideoSettings videoSettings = activeVideoSettingsFor(snapshot);
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
    Logger::instance().info(L"recorder",
        L"GPU pipeline ready: capture=" + std::to_wstring(sourceWidth) + L"x" + std::to_wstring(sourceHeight) +
        L", encode=" + std::to_wstring(videoSettings.width) + L"x" + std::to_wstring(videoSettings.height) +
        L", adapter=" + d3d_.adapterName());
    return true;
}

bool RecorderController::createCaptureSource(
    const CaptureTarget& target,
    bool exclusiveRecovery) {
    AppSettings snapshot = settings();
    const CaptureBackend selectedBackend = selectedBackendForSettings(snapshot);

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
    bool dxgiSkippedForTarget = false;
    // Cache the advanced-color probe: stats() derives hdrRejected from it
    // without re-walking DXGI on the diagnostics path. An OS HDR toggle
    // mid-session on WGC stays invisible until the next rebind (WGC frames
    // carry no hdr flag); DD toggles still surface via the per-frame flag.
    const bool monitorHdr = monitor && monitorHasAdvancedColor(monitor);
    hdrMonitorActive_.store(monitorHdr, std::memory_order_relaxed);

    CaptureTarget monitorTarget = target;
    monitorTarget.window = nullptr;
    CaptureTarget windowTarget = target;
    const bool hasWindowTarget = windowTarget.window && IsWindow(windowTarget.window);
    if (!hasWindowTarget) {
        windowTarget.window = nullptr;
    }

    std::unique_ptr<ICaptureSource> initialized;

    // Respect an explicit DXGI choice before considering WGC window capture.
    // Window targeting is an optimization, not permission to override capture
    // method selected by user.
    if (!initialized && selectedBackend == CaptureBackend::DesktopDuplication &&
        !exclusiveRecovery && dxgiTargetOnActiveAdapter) {
        initialized = tryInitialize(
            std::make_unique<DesktopDuplicationCapture>(), L"Desktop Duplication", monitorTarget);
    }

    // Normal WGC path can target a window. Exclusive recovery stays on the
    // monitor: CreateForWindow against an exclusive-fullscreen surface yields
    // zero frames.
    if (!initialized && hasWindowTarget && !exclusiveRecovery &&
        selectedBackend != CaptureBackend::DesktopDuplication) {
        initialized = tryInitialize(
            std::make_unique<WgcCaptureSource>(), L"Windows Graphics Capture (window)", windowTarget);
    }

    if (!initialized && exclusiveRecovery &&
        dxgiTargetOnActiveAdapter) {
        Logger::instance().info(L"recorder", L"Attempting Desktop Duplication recovery");
        initialized = tryInitialize(
            std::make_unique<DesktopDuplicationCapture>(), L"Desktop Duplication", monitorTarget);
    }

    if (!initialized && selectedBackend == CaptureBackend::WindowsGraphicsCapture &&
        !exclusiveRecovery) {
        // HDR monitor on a WGC default stays on a fixed B8G8R8A8 pool and
        // leans on DWM pre-tonemap, which washes out. Prefer Desktop
        // Duplication so the scaler tone-maps; WGC below remains the fallback.
        if (monitorHdr && dxgiTargetOnActiveAdapter) {
            Logger::instance().info(L"app",
                L"HDR monitor detected; preferring Desktop Duplication for tone-mapping (WGC fallback if it fails)");
            initialized = tryInitialize(
                std::make_unique<DesktopDuplicationCapture>(), L"Desktop Duplication", monitorTarget);
        }
        if (!initialized) {
            initialized = tryInitialize(
                std::make_unique<WgcCaptureSource>(), L"Windows Graphics Capture", monitorTarget);
        }
    }

    if (!initialized && selectedBackend == CaptureBackend::DesktopDuplication) {
        if (!dxgiTargetOnActiveAdapter) {
            const wchar_t* detail = !location.valid()
                ? L"monitor has no DXGI output"
                : !dxgiTargetEncodable
                    ? L"monitor adapter has no supported hardware encoder"
                    : L"target adapter differs from active D3D adapter";
            Logger::instance().warning(L"recorder",
                std::wstring(L"Desktop Duplication unavailable for target; using WGC: ") + detail);
            dxgiSkippedForTarget = true;
        } else {
            Logger::instance().warning(L"recorder",
                L"Desktop Duplication failed; attempting Windows Graphics Capture fallback");
        }
        if (hasWindowTarget) {
            initialized = tryInitialize(
                std::make_unique<WgcCaptureSource>(), L"Windows Graphics Capture (window)", windowTarget);
        }
        if (!initialized) {
            initialized = tryInitialize(
                std::make_unique<WgcCaptureSource>(), L"Windows Graphics Capture", monitorTarget);
        }
    }

    // WGC fallback may use Desktop Duplication only when output and active D3D
    // device share an encoder-capable adapter.
    if (!initialized && selectedBackend == CaptureBackend::WindowsGraphicsCapture &&
        !exclusiveRecovery && dxgiTargetOnActiveAdapter) {
        Logger::instance().info(L"recorder", L"WGC failed; attempting Desktop Duplication fallback");
        initialized = tryInitialize(
            std::make_unique<DesktopDuplicationCapture>(), L"Desktop Duplication", monitorTarget);
    }

    // Exclusive recovery: if DXGI failed, leave capture null so the loop
    // deep-holds lastFrame. Do not thrash WGC.

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
        Logger::instance().error(L"recorder",
            std::wstring(L"Capture worker crashed; capture degraded: ") + utf8ToWide(error.what()));
    } catch (...) {
        Logger::instance().error(L"recorder", L"Capture worker crashed; capture degraded: unknown exception");
    }
    if (!stopRequested_.load(std::memory_order_acquire)) {
        // Crashed, not stopped. Surface the degradation instead of leaving the
        // UI reporting a healthy backend that no longer delivers frames.
        std::scoped_lock lock(captureStatusMutex_);
        captureBackendActive_ = false;
        captureBackendStatus_ += L"; capture worker stopped after an internal error";
    }
}

void RecorderController::captureLoopImpl() {
    setThreadDescriptionSafe(L"Backtrack capture");
    const HANDLE mmcssHandle = enableMmcssForCaptureThread();
    // Session-scoped: raises the global system timer resolution for the whole
    // capture/encode session, then restores it on loop exit. Without this,
    // coarse cadence waits quantize to the 15.6ms default and 60fps aliases to
    // ~30fps in windowed mode (exclusive fullscreen raises the period for us).
    const HighResolutionTimerScope timerScope;

    AppSettings captureSettings = settings();
    VideoSettings videoSettings;
    GpuOptimizationSettings gpuSettings;
    {
        std::scoped_lock stateLock(stateMutex_);
        videoSettings = activeVideoSettings_;
        gpuSettings = activeGpuSettings_;
    }
    GpuFrame lastFrame;
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
    bool requestKeyFrameAfterAcceptedFrame = false;
    // Recovery state lives in the header-only CaptureSession supervisor; frame
    // data and clocks stay here.
    // Capture session under exclusive fullscreen:
    // Capturing → AccessLost → WaitStable → Rebind (DXGI only) → Capturing | Holding.
    // Never thrash WGC CreateForWindow for exclusive cover (zero frames).
    // Transitions are session.toX(); partial resets write session fields directly.
    CaptureSession session;
    // Tracks capture_->cursorOnlyFrames() from the last delivered frame. A DXGI
    // frame whose counter did not advance carries a real desktop present (proof of
    // composited/capturable content); a cursor-only stale frame advances it.
    uint64_t prevCursorOnlyFrames = 0;
    // Tracks capture_->resolutionChangeCount(); an advance means the source
    // resized in place and the next accepted frame must open a new GOP.
    uint64_t prevResolutionChanges = 0;
    // Tracks the duplication surface HDR identity (GpuFrame::hdr + format). A
    // mid-session HDR toggle flips them in place without a rebind; the next
    // accepted frame must open a new GOP so the tone-map switch never lands
    // mid-GOP. Seeded from the first acquired frame after each rebind.
    bool prevHdrSeen = false;
    bool prevHdr = false;
    DXGI_FORMAT prevHdrFormat = DXGI_FORMAT_UNKNOWN;
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
    {
        std::scoped_lock gpuLock(pipelineGpuMutex_);
        encoderInputFormat = encoder_
            ? encoder_->preferredInputFormat()
            : DXGI_FORMAT_B8G8R8A8_UNORM;
    }
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
            Logger::instance().warning(L"recorder",
                std::wstring(L"Capture session AccessLost: ") + (reason ? reason : L"unspecified"));
        }
    };
    auto enterExclusiveHold = [&](const wchar_t* reason) {
        // toHolding reports the first hold of this cover episode; only then
        // emit the one IDR + log + status line.
        if (session.toHolding()) {
            forceVideoHeartbeat_ = true; // one IDR of freeze; no periodic slideshow
            Logger::instance().warning(L"recorder",
                std::wstring(L"Exclusive fullscreen hold: ") +
                    (reason ? reason : L"desktop APIs blind") +
                    L". Holding last frame (no slideshow).");
            {
                std::scoped_lock lock(captureStatusMutex_);
                captureBackendStatus_ =
                    L"Exclusive: frozen (last frame) — game capture needed for live";
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
            // The zero-copy path hands the WGC frame itself onward; its lease
            // stays pinned by lastFrame until this call replaces it, which is
            // within the pool budget (see maxLeases in WgcCaptureSource).
            const bool wgcZeroCopyEligible =
                capture_ &&
                capture_->backend() == CaptureBackend::WindowsGraphicsCapture &&
                gpuSettings.wgcZeroCopy;
            const bool forceOwnedTexture =
                !wgcZeroCopyEligible ||
                (captureSettings.multiMonitorSupport && captureSettings.followFocusedMonitor && gpuSettings.stableMultimonitorFrames);
            const uint64_t poolGeneration = scaler_.poolGeneration();
            const bool scaled = scaler_.scale(
                    d3d_,
                    rawSource,
                    videoSettings.width,
                    videoSettings.height,
                    encodeFrame,
                    captureSettings.multiMonitorSupport && captureSettings.followFocusedMonitor,
                    forceOwnedTexture,
                    encoderInputFormat);
            if (scaler_.poolGeneration() != poolGeneration && encoder_) {
                // The scaler tore down its output pool. The encoder's
                // registered-input table still points at those textures and the
                // driver reuses the freed addresses, so every registration is
                // stale from here on.
                encoder_->resetInputResources();
            }
            if (!scaled) {
                ++droppedFrames_;
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

    // Copies lastFrame's pixels into a scaler-owned texture before the current
    // capture source is shut down (source rebind). A raw WGC zero-copy frame
    // would otherwise pin its Direct3D11CaptureFrame across the old source's
    // shutdown, whose lease-drain wait can stall teardown; a DDup pool slot
    // stays valid but is recreated with the new source anyway. One copy per
    // rebind keeps the freeze-frame continuity that exclusive-fullscreen
    // recovery depends on. Cheap: source switches are rare events.
    auto privatizeLastFrame = [&]() {
        if (!lastFrame.texture) {
            return;
        }
        GpuFrame copy;
        bool scaled = false;
        {
            std::scoped_lock gpuLock(pipelineGpuMutex_);
            const DXGI_FORMAT format =
                lastFrame.format != DXGI_FORMAT_UNKNOWN ? lastFrame.format : DXGI_FORMAT_B8G8R8A8_UNORM;
            const uint64_t poolGeneration = scaler_.poolGeneration();
            scaled = scaler_.scale(
                    d3d_,
                    lastFrame,
                    lastFrame.width,
                    lastFrame.height,
                    copy,
                    false,
                    true,
                    format);
            if (scaler_.poolGeneration() != poolGeneration && encoder_) {
                // Same dangling-registration hazard as the capture loop: this
                // copy path can reconfigure the pool at the frame's own size.
                encoder_->resetInputResources();
            }
        }
        if (!scaled) {
            // No scaler-owned copy means lastFrame still holds the raw WGC
            // zero-copy lease, and switchCaptureTarget shuts the source down
            // next - a shutdown that drains leases waits on exactly this one,
            // and only a later successful capture could release it. Drop the
            // frame here instead: one lost freeze frame beats a stalled rebind.
            lastFrame = {};
            return;
        }
        lastFrame = std::move(copy);
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
                scaler_.setPoolSize(captureTexturePoolSize(gpuSettings));
            }
        }
        lastFrame = {};
        emitClockStarted = false;
        timelineScheduler.reset();
        lastVideoSubmission = SteadyClock::time_point::min();
        requestKeyFrameAfterAcceptedFrame = true;
        prevHdrSeen = false;
        // A DD(hdr=true) mirror must not survive the handoff gap: no HDR frame
        // flows again until the new source delivers one.
        hdrActive_.store(false, std::memory_order_relaxed);
    };

    auto switchCaptureTarget = [&](const CaptureTarget& target,
                                    const wchar_t* reason,
                                    bool exclusiveRecovery = false) -> bool {
        // Detach lastFrame from the outgoing source before its shutdown: the
        // raw frame now carries the capture-side lease (WGC Direct3D11Capture-
        // Frame), and holding it across teardown would stall the lease drain.
        privatizeLastFrame();
        const uint32_t targetAdapter = pipelineAdapterForTarget(target);
        if (targetAdapter != d3d_.adapterIndex()) {
            // Full device recreate invalidates lastFrame GPU resources.
            if (!recreateGpuPipeline(target, reason, targetAdapter)) {
                return false;
            }
            resetVideoHandoff();
            {
                std::scoped_lock stateLock(stateMutex_);
                videoSettings = activeVideoSettings_;
                gpuSettings = activeGpuSettings_;
            }
            {
                std::scoped_lock gpuLock(pipelineGpuMutex_);
                encoderInputFormat = encoder_
                    ? encoder_->preferredInputFormat()
                    : DXGI_FORMAT_B8G8R8A8_UNORM;
            }
            return true;
        }
        {
            std::scoped_lock gpuLock(pipelineGpuMutex_);
            if (!createCaptureSource(target, exclusiveRecovery) || !capture_) {
                return false;
            }
            prevResolutionChanges = capture_->resolutionChangeCount();
            prevHdrSeen = false;
            // Stale DD(hdr=true) must not survive the gap: the privatized freeze
            // frame is already SDR after its scaler copy, and WGC emits SDR
            // only, so re-flagging it HDR would tone-map twice.
            if (capture_->backend() == CaptureBackend::WindowsGraphicsCapture) {
                lastFrame.hdr = false;
            }
            hdrActive_.store(false, std::memory_order_relaxed);
            if (encoder_) {
                encoder_->resetInputResources();
            }
            // Scaler pool slots held by lastFrame stay alive via shared_ptr leases.
            scaler_.reset();
            scaler_.setPoolSize(captureTexturePoolSize(gpuSettings));
        }
        // Drop in-flight queue frames from the previous source, but keep lastFrame
        // and the emit clock so exclusive-fullscreen ACCESS_LOST / WGC stalls do
        // not punch a hole in the replay timeline (freeze until live frames return).
        discardQueuedFrames_.store(true, std::memory_order_release);
        requestKeyFrameAfterAcceptedFrame = true;
        if (frameEvent_) {
            SetEvent(frameEvent_);
        }
        refreshCaptureDimensions();
        Logger::instance().info(L"recorder", std::wstring(L"Capture source switched: ") + reason);
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
        const auto loopNow = std::chrono::steady_clock::now();
        if (captureSettings.captureSystemAudio &&
            loopNow >= nextSoundSeparationPoll) {
            nextSoundSeparationPoll = loopNow + soundSeparationPollInterval;
            refreshSystemAudioCapture(captureSettings);
        }

        if (captureSettings.multiMonitorSupport && captureSettings.followFocusedMonitor && loopNow >= nextMonitorPoll) {
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

        // Missing-capture recovery is handled below with cooldown so lastFrame
        // heartbeats keep running during exclusive-fullscreen DuplicateOutput waits.

        const auto nowLoop = std::chrono::steady_clock::now();
        exclusiveCoverActive =
            foregroundWindowCoversMonitor(monitorForCaptureTarget(activeCaptureTarget_));

        GpuFrame frame;
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
                Logger::instance().info(L"recorder",
                    L"Capture source resized in place; keyframe requested at " +
                    std::to_wstring(capture_->width()) + L"x" + std::to_wstring(capture_->height()));
            }
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
            // escalation budget re-arms.
            session.softStallBeganAt = SteadyClock::time_point::min();
            session.softStallRecreates = 0;
            session.deadTargetFailures = 0;
            // Mid-session HDR toggle flips the duplication surface in place
            // (no rebind, no isDeviceLost). A silent flip would stretch
            // whitened frames into the running GOP, so open a new one.
            hdrActive_.store(frame.hdr, std::memory_order_relaxed);
            if (!prevHdrSeen) {
                prevHdr = frame.hdr;
                prevHdrFormat = frame.format;
                prevHdrSeen = true;
            } else if (frame.hdr != prevHdr || frame.format != prevHdrFormat) {
                prevHdr = frame.hdr;
                prevHdrFormat = frame.format;
                requestKeyFrameAfterAcceptedFrame = true;
                Logger::instance().info(L"app",
                    std::wstring(L"HDR state changed mid-session (hdr=") +
                    (frame.hdr ? L"on" : L"off") + L"); keyframe requested");
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

        // Health probes run lock-free. Every pointer read here is only ever
        // mutated on this thread while the pipeline runs (GPU recreation and
        // source rebinds execute on the capture thread; stopPipeline joins it
        // before teardown), so no lock is needed for a stable view.
        // encoderAvailable() is an atomic mirror; the previous
        // pipelineGpuMutex_ + stats() version serialized every iteration of
        // this loop behind in-flight encodes just to read one bool.
        bool captureLost = !capture_ || capture_->isDeviceLost();
        bool deviceRemoved = d3d_.isDeviceRemoved();
        bool encoderFaulted = encoder_ && !encoder_->encoderAvailable();

        if (deviceRemoved || encoderFaulted) {
            CaptureTarget target = captureSettings.multiMonitorSupport && captureSettings.followFocusedMonitor
                ? captureTargetForSettings(captureSettings)
                : activeCaptureTarget_;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            Logger::instance().warning(L"recorder",
                deviceRemoved
                    ? L"D3D device removed; recreating full GPU pipeline"
                    : L"Hardware encoder faulted; recreating full GPU pipeline");
            if (recreateGpuPipeline(
                    target,
                    deviceRemoved ? L"device removed" : L"encoder fault",
                    pipelineAdapterForTarget(target))) {
                {
                    std::scoped_lock stateLock(stateMutex_);
                    videoSettings = activeVideoSettings_;
                    gpuSettings = activeGpuSettings_;
                }
                resetVideoHandoff();
                {
                    std::scoped_lock gpuLock(pipelineGpuMutex_);
                    encoderInputFormat = encoder_
                        ? encoder_->preferredInputFormat()
                        : DXGI_FORMAT_B8G8R8A8_UNORM;
                }
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
            constexpr auto kHoldDesktopProbeMs = std::chrono::milliseconds(2000);
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
                    CaptureTarget target = captureSettings.multiMonitorSupport && captureSettings.followFocusedMonitor
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
                                    L"Capture display is unavailable; recording holds its "
                                    L"last frame. Reconnect the display or change the "
                                    L"capture monitor in settings.";
                                setLastRecordingError(detail);
                                Logger::instance().error(L"recorder", detail);
                                std::scoped_lock lock(captureStatusMutex_);
                                captureBackendActive_ = false;
                                captureBackendStatus_ =
                                    L"Capture display unavailable; holding last frame";
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
                    CaptureTarget target = captureSettings.multiMonitorSupport && captureSettings.followFocusedMonitor
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
                    // Full-monitor borderless still satisfies exclusive-cover geometry.
                    // Probe Desktop Duplication once more; repeated identical
                    // attempts would not make desktop capture recover.
                    session.exclusiveBlind = false;
                    session.exclusiveDxgiAttempts = 0;
                    session.rebindAttempts = 0;
                    session.recoveryProbe = true;
                    session.state = CaptureSessionState::Rebind;
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
                        session.softStallRecreates < kMaxSoftStallRecreates &&
                        recreateCooldownElapsed &&
                        nowLoop - session.softStallBeganAt >= kSoftStallRecreateAfterMs) {
                        ++session.softStallRecreates;
                        session.lastSoftStallAt = nowLoop;
                        session.softStallBeganAt = nowLoop;
                        CaptureTarget target = captureSettings.multiMonitorSupport && captureSettings.followFocusedMonitor
                            ? captureTargetForSettings(captureSettings)
                            : activeCaptureTarget_;
                        target.window = nullptr;
                        Logger::instance().warning(L"recorder",
                            L"No frames for " +
                            std::to_wstring(
                                std::chrono::duration_cast<std::chrono::milliseconds>(nowLoop - lastSourceFrameAt).count()) +
                            L"ms without device loss; recreating capture source (attempt " +
                            std::to_wstring(session.softStallRecreates) + L"/" +
                            std::to_wstring(kMaxSoftStallRecreates) + L")");
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

        // Emit cadence even when capture is lost/null: lastFrame heartbeats keep
        // the replay timeline continuous across exclusive-fullscreen recovery.
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
        Logger::instance().error(L"recorder",
            std::wstring(L"Encode worker crashed; encoding degraded: ") + utf8ToWide(error.what()));
    } catch (...) {
        Logger::instance().error(L"recorder", L"Encode worker crashed; encoding degraded: unknown exception");
    }
    // Harmless on the normal path (the drain already published this value) and
    // essential on the crash path: recreateGpuPipeline blocks until this
    // generation advances, so GPU teardown must never wait on a dead worker.
    // Publishing under frameDiscardMutex_ pairs with the waiter's predicate
    // check; without the lock a store+notify landing between the check and
    // the sleep would be lost and recreateGpuPipeline could hang forever on
    // this dead worker.
    {
        std::scoped_lock lock(frameDiscardMutex_);
        discardCompletedGeneration_.store(
            discardRequestGeneration_.load(std::memory_order_acquire),
            std::memory_order_release);
        frameDiscardComplete_.notify_all();
    }
}

void RecorderController::encodeLoopImpl() {
    setThreadDescriptionSafe(L"Backtrack encode");
    // MMCSS "Games" keeps the encode loop above background system load. The
    // frame queue drops the newest frames under pressure, so a starved encoder
    // would silently lose the most recent replay/recorded frames exactly when
    // the machine is busy - the frames that matter most for a backtrack clip.
    const HANDLE mmcssHandle = enableMmcssForEncodeThread();

    constexpr size_t kMaxPendingDurations = 512;
    std::unordered_map<int64_t, int64_t> pendingDurations;
    // Insertion order of live pendingDurations keys. The encoder normally
    // consumes each entry within a frame or two; only a pathological encoder
    // stall lets the map reach the cap, and then the OLDEST entries are
    // evicted one by one. Wholesale-clearing instead would drop every still
    // unmatched duration extension at once, making video stts jump while
    // audio is silence-filled and skewing the streams apart.
    std::deque<int64_t> pendingDurationOrder;
    uint64_t pendingDurationEvictions = 0;

    auto discardQueuedFrames = [&]() {
        GpuFrame discarded;
        while (frameQueue_.tryPop(discarded)) {
        }
        {
            std::scoped_lock lock(durationUpdateMutex_);
            pendingDurationPts100ns_ = 0;
            pendingDuration100ns_ = 0;
            durationUpdatePending_.store(false, std::memory_order_release);
        }
        pendingDurations.clear();
        pendingDurationOrder.clear();
        // Same mutex pairing as the crash-path publish above: the store must
        // happen under frameDiscardMutex_ or a waiter between its predicate
        // check and its sleep can miss the notification forever.
        {
            std::scoped_lock lock(frameDiscardMutex_);
            discardCompletedGeneration_.store(
                discardRequestGeneration_.load(std::memory_order_acquire),
                std::memory_order_release);
            frameDiscardComplete_.notify_all();
        }
    };

    auto extendVideoDuration = [&](int64_t pts100ns, int64_t duration100ns) {
        if (pts100ns <= 0 || duration100ns <= 0) {
            return;
        }
        auto [it, inserted] = pendingDurations.try_emplace(pts100ns, duration100ns);
        if (inserted) {
            pendingDurationOrder.push_back(pts100ns);
            // Drop already-consumed keys from the front so the order index
            // stays proportional to the live map instead of total inserts.
            while (!pendingDurationOrder.empty() &&
                   pendingDurations.find(pendingDurationOrder.front()) == pendingDurations.end()) {
                pendingDurationOrder.pop_front();
            }
            if (pendingDurations.size() > kMaxPendingDurations) {
                // Evict oldest unconsumed entries; skip keys already consumed.
                while (!pendingDurationOrder.empty() &&
                       pendingDurations.size() > kMaxPendingDurations) {
                    const int64_t oldest = pendingDurationOrder.front();
                    pendingDurationOrder.pop_front();
                    if (pendingDurations.erase(oldest) > 0) {
                        ++pendingDurationEvictions;
                        if (pendingDurationEvictions == 1 ||
                            (pendingDurationEvictions % 512) == 0) {
                            Logger::instance().warning(L"recorder",
                                L"Encoder output is falling behind; evicted old unmatched video durations count=" +
                                std::to_wstring(pendingDurationEvictions));
                        }
                    }
                }
            }
        } else {
            it->second = std::max(it->second, duration100ns);
        }
        const bool recordingExtended =
            recording_ && muxer_.active() &&
            muxer_.extendLastVideoDuration(pts100ns, duration100ns);
        const bool replayExtended = replay_.extendLastVideoDuration(pts100ns, duration100ns);
        if (recordingExtended || replayExtended) {
            pendingDurations.erase(pts100ns);
        }
        videoTimelineEndPts100ns_.store(pts100ns + duration100ns, std::memory_order_release);
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
        if (auto it = pendingDurations.find(packet.pts100ns); it != pendingDurations.end()) {
            packet.duration100ns = std::max(packet.duration100ns, it->second);
            pendingDurations.erase(it);
        }
        lastEncodedVideoPts100ns_.store(packet.pts100ns, std::memory_order_release);
        videoTimelineEndPts100ns_.store(
            packet.pts100ns + std::max<int64_t>(packet.duration100ns, 0),
            std::memory_order_release);

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
    };

    while (!stopRequested_ || frameQueue_.size() > 0 ||
           durationUpdatePending_.load(std::memory_order_acquire)) {
        if (discardQueuedFrames_.exchange(false)) {
            discardQueuedFrames();
            continue;
        }

        consumeDurationUpdate();

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
            discardQueuedFrames();
            continue;
        }

        extendVideoDuration(frame.previousFramePts100ns, frame.previousFrameDuration100ns);

        EncodedPacket packet;
        bool encoded = false;
        {
            std::scoped_lock gpuLock(pipelineGpuMutex_);
            if (encoder_ && encoder_->encodeFrame(frame, packet) && !packet.bytes.empty()) {
                encoded = true;
            }
        }
        if (encoded) {
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

void RecorderController::handleAudioPacket(AudioPacket&& packet) {
    if (packet.track == AudioTrack::SoundSeparation) {
        return;
    }

    const AudioTrack track = packet.track;
    // Liveness: record arrival before the packet can be dropped by a full
    // queue - a starved queue must not read as a dead input.
    (track == AudioTrack::System
        ? systemAudioLastPacketAt100ns_
        : microphoneAudioLastPacketAt100ns_)
        .store(steadyNow100ns(), std::memory_order_relaxed);
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
    if (!convertAudioPacketToCanonical(packet)) {
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
        Logger::instance().error(L"recorder",
            std::wstring(L"Audio worker crashed; audio degraded: ") + utf8ToWide(error.what()));
    } catch (...) {
        Logger::instance().error(L"recorder", L"Audio worker crashed; audio degraded: unknown exception");
    }
}

void RecorderController::audioLoopImpl() {
    setThreadDescriptionSafe(L"Backtrack audio writer");

    while (!stopRequested_ || systemAudioQueue_.size() > 0 || microphoneAudioQueue_.size() > 0) {
        AudioPacket packet;
        bool wrotePacket = false;
        while (systemAudioQueue_.tryPop(packet)) {
            writeAudioPacket(std::move(packet));
            wrotePacket = true;
        }
        while (microphoneAudioQueue_.tryPop(packet)) {
            writeAudioPacket(std::move(packet));
            wrotePacket = true;
        }
        if (!wrotePacket && audioEvent_) {
            WaitForSingleObject(audioEvent_, 16);
        }
    }
}

void RecorderController::refreshSystemAudioCapture(const AppSettings& settings) {
    if (!settings.captureSystemAudio) {
        return;
    }

    const std::vector<std::wstring> mutedKeys = mutedSoundSeparationExecutableKeys(settings);
    std::scoped_lock lock(systemAudioCaptureMutex_);

    const bool keysChanged = mutedKeys != systemAudioMutedExecutableKeys_;
    if (systemAudio_.running() && !keysChanged) {
        // Re-apply session mutes so newly started muted apps get silenced.
        if (!mutedKeys.empty()) {
            WasapiCapture::applySessionMutesForExecutables(mutedKeys);
        }
        return;
    }

    systemAudioMutedExecutableKeys_ = mutedKeys;
    systemAudio_.stop();

    if (mutedKeys.empty()) {
        WasapiCapture::clearSessionMutes();
    } else {
        const uint32_t mutedCount = WasapiCapture::applySessionMutesForExecutables(mutedKeys);
        Logger::instance().info(L"recorder",
            L"Sound separation muting " + std::to_wstring(mutedCount) +
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
        WasapiCapture::clearSessionMutes();
        systemAudioMutedExecutableKeys_.clear();
    }
}

std::filesystem::path RecorderController::nextClipPath(const wchar_t* prefix, bool organizeIntoSubfolder) const {
    const auto snapshot = settings();
    // Per-game folders reuse the already filename-sanitized clip prefix, so no
    // further scrubbing is needed. Created here because the reservation below
    // (CREATE_NEW) fails when the parent does not exist yet.
    std::filesystem::path directory = snapshot.clipDirectory;
    if (organizeIntoSubfolder && prefix != nullptr && *prefix != L'\0') {
        std::error_code directoryError;
        std::filesystem::create_directories(snapshot.clipDirectory / prefix, directoryError);
        if (directoryError) {
            Logger::instance().warning(L"recorder", std::wstring(L"Could not create game clip folder, saving to the clip folder instead: ") +
                                       (snapshot.clipDirectory / prefix).wstring());
        } else {
            directory /= prefix;
        }
    }
    const std::filesystem::path initial = directory / makeTimestampedFileName(prefix, L".mp4");
    if (reserveClipOutputName(initial) == OutputNameReservation::Reserved) {
        return initial;
    }

    // The name could not be claimed exclusively; keep the previous best-effort
    // existence probing so path selection still works on exotic failures.
    std::error_code error;
    if (!std::filesystem::exists(initial, error)) {
        return initial;
    }

    const std::wstring stem = initial.stem().wstring();
    for (uint32_t suffix = 2; suffix < 1000; ++suffix) {
        const auto candidate = directory / (stem + L"_" + std::to_wstring(suffix) + L".mp4");
        if (reserveClipOutputName(candidate) == OutputNameReservation::Reserved) {
            return candidate;
        }
        error = {};
        if (!std::filesystem::exists(candidate, error)) {
            return candidate;
        }
    }

    return directory /
           (stem + L"_" + std::to_wstring(GetTickCount64()) + L".mp4");
}

void RecorderController::setLastRecordingError(std::wstring error) {
    std::scoped_lock lock(recordingErrorMutex_);
    lastRecordingError_ = std::move(error);
}

} // namespace backtrack
