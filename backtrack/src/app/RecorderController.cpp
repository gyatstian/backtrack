#include "app/RecorderController.h"
#include "app/VideoTimelineScheduler.h"

#include "audio/AudioConvert.h"
#include "capture/DesktopDuplicationCapture.h"
#include "capture/WgcCaptureSource.h"
#include "core/Logger.h"
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
#include <deque>
#include <limits>
#include <thread>
#include <unordered_set>
#include <utility>

namespace backtrack {

namespace {

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

uint32_t evenEncodeDimension(uint32_t value) {
    value = std::max<uint32_t>(16, value);
    return value % 2 == 0 ? value : value - 1;
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

// Ring storage location: explicit override when set, otherwise next to the
// clip library. Keeping this resolution here means ReplayBuffer stays agnostic
// of where the user wants the file.
std::filesystem::path replayRingDirectoryFor(const AppSettings& settings) {
    return settings.replayRingDirectory.empty() ? settings.clipDirectory : settings.replayRingDirectory;
}

const wchar_t* captureBackendDisplayName(CaptureBackend backend) {
    switch (backend) {
    case CaptureBackend::WindowsGraphicsCapture:
        return L"Windows Graphics Capture";
    case CaptureBackend::DesktopDuplication:
        return L"Desktop Duplication";
    }
    return L"Unknown";
}

CaptureBackend selectedBackendForSettings(const AppSettings& settings) {
    return settings.preferredCaptureBackend;
}

VideoSettings activeVideoSettingsFor(const AppSettings& settings) {
    VideoSettings active = settings.video;
    active.width = evenEncodeDimension(active.width);
    active.height = evenEncodeDimension(active.height);
    return active;
}

std::wstring normalizedExecutableKey(const std::filesystem::path& path) {
    std::wstring value = path.wstring();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return value;
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
        const std::wstring key = normalizedExecutableKey(app.executablePath);
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

int32_t readSigned24(const uint8_t* sample) {
    int32_t value = static_cast<int32_t>(sample[0]) |
                    (static_cast<int32_t>(sample[1]) << 8) |
                    (static_cast<int32_t>(sample[2]) << 16);
    if ((value & 0x00800000) != 0) {
        value |= static_cast<int32_t>(0xff000000);
    }
    return value;
}

void writeSigned24(uint8_t* sample, int32_t value) {
    sample[0] = static_cast<uint8_t>(value & 0xff);
    sample[1] = static_cast<uint8_t>((value >> 8) & 0xff);
    sample[2] = static_cast<uint8_t>((value >> 16) & 0xff);
}

template <typename Sample>
Sample scaledIntegerSample(Sample value, float gain) {
    const auto scaled = static_cast<long long>(std::llround(static_cast<double>(value) * gain));
    return static_cast<Sample>(std::clamp<long long>(
        scaled,
        static_cast<long long>(std::numeric_limits<Sample>::min()),
        static_cast<long long>(std::numeric_limits<Sample>::max())));
}

void applyPcmVolume(AudioPacket& packet, uint16_t bitsPerSample, float gain) {
    const uint16_t bytesPerSample = bitsPerSample / 8;
    if (bytesPerSample == 0 || bitsPerSample % 8 != 0) {
        return;
    }

    for (size_t offset = 0; offset + bytesPerSample <= packet.bytes.size(); offset += bytesPerSample) {
        uint8_t* sample = packet.bytes.data() + offset;
        switch (bitsPerSample) {
        case 8: {
            const int value = static_cast<int>(*sample) - 128;
            const int scaled = std::clamp(static_cast<int>(std::lround(value * gain)), -128, 127);
            *sample = static_cast<uint8_t>(scaled + 128);
            break;
        }
        case 16: {
            int16_t value = 0;
            std::memcpy(&value, sample, sizeof(value));
            value = scaledIntegerSample(value, gain);
            std::memcpy(sample, &value, sizeof(value));
            break;
        }
        case 24: {
            const int32_t value = readSigned24(sample);
            const int32_t scaled = static_cast<int32_t>(std::clamp<long long>(
                static_cast<long long>(std::llround(static_cast<double>(value) * gain)),
                -8388608,
                8388607));
            writeSigned24(sample, scaled);
            break;
        }
        case 32: {
            int32_t value = 0;
            std::memcpy(&value, sample, sizeof(value));
            value = scaledIntegerSample(value, gain);
            std::memcpy(sample, &value, sizeof(value));
            break;
        }
        default:
            return;
        }
    }
}

void applyFloatVolume(AudioPacket& packet, uint16_t bitsPerSample, float gain) {
    if (bitsPerSample != 32) {
        return;
    }

    for (size_t offset = 0; offset + sizeof(float) <= packet.bytes.size(); offset += sizeof(float)) {
        float value = 0.0f;
        std::memcpy(&value, packet.bytes.data() + offset, sizeof(value));
        value = std::clamp(value * gain, -1.0f, 1.0f);
        std::memcpy(packet.bytes.data() + offset, &value, sizeof(value));
    }
}

void applyAudioVolume(AudioPacket& packet, uint32_t volumePercent) {
    if (volumePercent == 100 || packet.bytes.empty()) {
        return;
    }

    if (!packet.format) {
        return;
    }
    const AudioFormatInfo format = parseAudioFormat(*packet.format);
    const float gain = static_cast<float>(std::clamp<uint32_t>(volumePercent, 0, 200)) / 100.0f;
    switch (format.encoding) {
    case AudioSampleEncoding::Pcm:
        applyPcmVolume(packet, format.bitsPerSample, gain);
        break;
    case AudioSampleEncoding::Float:
        applyFloatVolume(packet, format.bitsPerSample, gain);
        break;
    case AudioSampleEncoding::Unsupported:
        break;
    }
}

} // namespace

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

    uint64_t sampleCount = 0;
    double sumOfSquares = 0.0;
    float peak = 0.0f;
    const uint8_t* data = packet.bytes.data();
    const size_t byteCount = packet.bytes.size();
    for (size_t offset = 0; offset + bytesPerSample <= byteCount; offset += bytesPerSample) {
        float value = 0.0f;
        if (format.encoding == AudioSampleEncoding::Float && format.bitsPerSample == 32) {
            std::memcpy(&value, data + offset, sizeof(value));
            value = std::clamp(value, -1.0f, 1.0f);
        } else {
            switch (format.bitsPerSample) {
            case 8:
                value = (static_cast<int>(data[offset]) - 128) / 128.0f;
                break;
            case 16: {
                int16_t sample = 0;
                std::memcpy(&sample, data + offset, sizeof(sample));
                value = sample / 32768.0f;
                break;
            }
            case 24:
                value = readSigned24(data + offset) / 8388608.0f;
                break;
            case 32: {
                int32_t sample = 0;
                std::memcpy(&sample, data + offset, sizeof(sample));
                value = sample / 2147483648.0f;
                break;
            }
            default:
                return levels;
            }
        }
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
    switch (format.encoding) {
    case AudioSampleEncoding::Pcm:
        applyPcmVolume(packet, format.bitsPerSample, gain);
        break;
    case AudioSampleEncoding::Float:
        applyFloatVolume(packet, format.bitsPerSample, gain);
        break;
    case AudioSampleEncoding::Unsupported:
        break;
    }
}

RecorderController::RecorderController()
    : frameQueue_(8),
      systemAudioQueue_(256),
      microphoneAudioQueue_(256),
      microphoneAgc_(std::make_unique<MicrophoneAgc>()) {
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

void RecorderController::beginStartupSweep() {
    std::scoped_lock lock(startupSweepMutex_);
    if (startupSweepThread_.joinable()) {
        return; // A sweep is already in flight.
    }
    // Capture the path: a settings change must not redirect the running sweep.
    const std::filesystem::path clipDirectory = settings().clipDirectory;
    Logger::instance().info(L"app", L"Startup recovery sweep started in background");
    startupSweepThread_ = std::thread([clipDirectory]() {
        try {
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
    {
        std::scoped_lock lock(stateMutex_);
        settings_ = std::move(settings);
        activeVideoSettings_ = settings_.video;
        activeGpuSettings_ = settings_.gpu;
        audioOutputVolumePercent_.store(settings_.audioOutputVolumePercent, std::memory_order_relaxed);
        audioInputVolumePercent_.store(settings_.audioInputVolumePercent, std::memory_order_relaxed);
        audioInputAutoGain_.store(settings_.audioInputAutoGainEnabled, std::memory_order_relaxed);
        microphoneAgcResetPending_ = true;
        replay_.clear();
        replay_.configure(settings_.replay, replayRingDirectoryFor(settings_), replayRingCapacityBytesFor(settings_));
    }
    Logger::instance().info(L"recorder", std::wstring(L"Recorder initialized: clips=") + settings_.clipDirectory.wstring() +
                            L", replay=" + (settings_.replay.enabled ? L"enabled" : L"disabled") +
                            L", fps=" + std::to_wstring(settings_.video.fps) +
                            L", bitrateKbps=" + std::to_wstring(settings_.video.bitrateKbps));
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

    {
        std::scoped_lock lock(stateMutex_);
        const VideoSettings oldVideo = activeVideoSettings_;
        settings_ = std::move(settings);
        activeVideoSettings_ = settings_.video;
        activeGpuSettings_ = settings_.gpu;
        audioOutputVolumePercent_.store(settings_.audioOutputVolumePercent, std::memory_order_relaxed);
        audioInputVolumePercent_.store(settings_.audioInputVolumePercent, std::memory_order_relaxed);
        audioInputAutoGain_.store(settings_.audioInputAutoGainEnabled, std::memory_order_relaxed);
        microphoneAgcResetPending_ = true;
        replay_.configure(settings_.replay, replayRingDirectoryFor(settings_), replayRingCapacityBytesFor(settings_));
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

    if (settings_.replay.enabled) {
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

    const auto output = nextClipPath(L"recording");
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
        if (!settings().replay.enabled) {
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

    AppSettings snapshot = settings();
    if (!snapshot.replay.enabled) {
        stopPipeline();
    }

    recording_ = false;
    waitingForRecordingKeyFrame_ = false;
    auto output = muxer_.finalize();
    setLastRecordingError(output.empty() ? muxer_.lastError() : std::wstring());
    Logger::instance().info(L"recorder", output.empty()
                                ? std::wstring(L"Recording stopped without a finalized clip")
                                : std::wstring(L"Recording stopped and saved: ") + output.wstring());

    if (!snapshot.replay.enabled && pipelineRunning_) {
        stopPipeline();
    }
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
    if (!settings().replay.enabled) {
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
    const int64_t replayWindow100ns =
        secondsTo100ns(std::max<uint32_t>(1, settings().replay.seconds));
    const int64_t minReplayVideo100ns = std::min<int64_t>(
        2 * kHundredNanosecondsPerSecond,
        std::max<int64_t>(kHundredNanosecondsPerSecond / 2, (replayWindow100ns * 2) / 3));
    // Buffer already has enough video: mux without restarting capture (avoids
    // WGC recreate hangs and is unnecessary work when pipeline is already warm).
    if (replay_.videoKeyFrameCount() == 0 || replay_.videoSpan100ns() < minReplayVideo100ns) {
        Logger::instance().info(L"recorder",
            L"Replay save has no buffered keyframe or too little video (span=" +
            std::to_wstring(replay_.videoSpan100ns() / kHundredNanosecondsPerSecond) +
            L"s); ensuring pipeline and requesting IDR");
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
               replay_.videoSpan100ns() < minReplayVideo100ns) {
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
    } else {
        Logger::instance().debug(L"recorder", L"Replay save using existing buffer; pipeline ensure skipped");
    }
    const std::wstring gameName = foregroundApplicationName();
    const auto output = nextClipPath(gameName.empty() ? L"replay" : gameName.c_str());
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
    stats.replayEnabled = settings().replay.enabled;
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
    {
        // Diagnostics refreshes on the UI thread. GPU recovery can hold this lock
        // while WGC waits for outstanding frame leases, so never block UI for it.
        std::unique_lock gpuLock(pipelineGpuMutex_, std::try_to_lock);
        if (gpuLock.owns_lock() && encoder_) {
            lastEncoderStats_ = encoder_->stats();
        }
        stats.encoder = lastEncoderStats_;
    }
    stats.encoder.queueDepth = static_cast<uint32_t>(frameQueue_.size());
    return stats;
}

EncoderCapabilities RecorderController::encoderCapabilities() const {
    // See stats(): reporting must not wait for GPU teardown or recreation.
    std::unique_lock gpuLock(pipelineGpuMutex_, std::try_to_lock);
    if (gpuLock.owns_lock() && encoder_) {
        lastEncoderCapabilities_ = encoder_->capabilities();
    }
    return lastEncoderCapabilities_;
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

    replay_.configure(snapshot.replay, replayRingDirectoryFor(snapshot), replayRingCapacityBytesFor(snapshot));
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
    systemAudio_.stop();
    microphoneAudio_.stop();
    Logger::instance().info(L"recorder", L"Capture/encode pipeline audio stopped; joining video workers");
    if (frameEvent_) {
        SetEvent(frameEvent_);
    }
    if (captureThread_.joinable()) {
        Logger::instance().info(L"recorder", L"Waiting for capture worker to stop");
        captureThread_.join();
        Logger::instance().info(L"recorder", L"Capture worker stopped");
    }
    if (encodeThread_.joinable()) {
        Logger::instance().info(L"recorder", L"Waiting for encode worker to stop");
        encodeThread_.join();
        Logger::instance().info(L"recorder", L"Encode worker stopped");
    }
    Logger::instance().debug(L"recorder", L"Capture/encode threads joined");

    if (audioEvent_) {
        SetEvent(audioEvent_);
    }
    if (audioThread_.joinable()) {
        audioThread_.join();
    }
    WasapiCapture::clearSessionMutes();
    {
        std::scoped_lock lock(systemAudioCaptureMutex_);
        systemAudioMutedExecutableKeys_.clear();
        systemAudioSessionMutesActive_ = false;
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
        frameDiscardComplete_.wait(discardLock, [this, discardGeneration] {
            return discardCompletedGeneration_.load(std::memory_order_acquire) >= discardGeneration;
        });
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
        initialized = tryInitialize(
            std::make_unique<WgcCaptureSource>(), L"Windows Graphics Capture", monitorTarget);
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
    auto lastCaptureRebindAt = SteadyClock::time_point::min();
    auto accessLostAt = SteadyClock::time_point::min();
    HMONITOR pendingMonitor = nullptr;
    bool emitClockStarted = false;
    auto lastEmitTime = SteadyClock::time_point::min();
    bool requestKeyFrameAfterAcceptedFrame = false;
    // Known source switches may accept the first fresh source frame. The longer
    // sustained-frame gate remains for ambiguous capture-loss recovery.
    bool acceptFirstLiveFrameAfterTransition = false;
    bool immediateRebindPending = false;
    // Capture session under exclusive fullscreen:
    // Capturing → AccessLost → WaitStable → Rebind (DXGI only) → Capturing | Holding.
    // Never thrash WGC CreateForWindow for exclusive cover (zero frames).
    enum class CaptureSessionState {
        Capturing,
        AccessLost,
        WaitStable,
        Rebind,
        Holding,
    };
    CaptureSessionState captureSession = CaptureSessionState::Capturing;
    auto liveFramesSince = SteadyClock::time_point::min();
    int rebindAttempts = 0;
    int exclusiveDxgiAttempts = 0;
    int sustainedLiveFrameCount = 0;
    auto nextTargetLostCheckAt = SteadyClock::time_point::min();
    // Tracks capture_->cursorOnlyFrames() from the last delivered frame. A DXGI
    // frame whose counter did not advance carries a real desktop present (proof of
    // composited/capturable content); a cursor-only stale frame advances it.
    uint64_t prevCursorOnlyFrames = 0;
    // Tracks capture_->resolutionChangeCount(); an advance means the source
    // resized in place and the next accepted frame must open a new GOP.
    uint64_t prevResolutionChanges = 0;
    bool exclusiveCoverActive = false;
    HWND exclusiveTargetWindow = nullptr;
    bool loggedExclusiveHold = false;
    bool exclusiveBlind = false;
    // A monitor-sized borderless game remains an exclusive-cover candidate.
    // Periodically probe desktop capture while holding so composition resume
    // is detected without waiting for the cover geometry to change.
    bool desktopRecoveryProbe = false;
    auto rebindSourceBornAt = SteadyClock::time_point::min();
    // Non-exclusive soft-stall episode tracking (see kSoftStallRecreateAfterMs).
    // min() begin time means no stall is in progress; a delivered frame or any
    // recovery transition ends the episode and re-arms the escalation budget.
    auto softStallBeganAt = SteadyClock::time_point::min();
    int softStallRecreates = 0;
    auto lastSoftStallRecreateAt = SteadyClock::time_point::min();
    // Capture-target-loss escalation (finding 18): an unplugged or dead target
    // monitor would otherwise be retried forever while the recording holds one
    // frozen frame. After enough failed desktop rebinds, either retarget once
    // to another live display (when one exists) or latch a hard error that
    // surfaces in diagnostics instead of silently freezing.
    int deadTargetRebindFailures = 0;
    bool captureTargetLostLatched = false;
    bool retargetedToAlternateMonitor = false;
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
    // Exclusive-blind: coalesce only (no periodic GOP Submit of frozen texture).
    // One IDR on enter hold + on first real live frames after recover.
    bool captureStallHold = false;
    auto enterAccessLost = [&](const wchar_t* reason) {
        if (captureSession == CaptureSessionState::AccessLost ||
            captureSession == CaptureSessionState::WaitStable ||
            captureSession == CaptureSessionState::Rebind ||
            captureSession == CaptureSessionState::Holding) {
            captureStallHold = true;
            return;
        }
        accessLostAt = std::chrono::steady_clock::now();
        exclusiveTargetWindow = exclusiveCoverActive ? GetForegroundWindow() : nullptr;
        captureSession = CaptureSessionState::AccessLost;
        captureStallHold = true;
        liveFramesSince = SteadyClock::time_point::min();
        sustainedLiveFrameCount = 0;
        softStallBeganAt = SteadyClock::time_point::min();
        softStallRecreates = 0;
        Logger::instance().warning(L"recorder",
            std::wstring(L"Capture session AccessLost: ") + (reason ? reason : L"unspecified"));
    };
    auto enterExclusiveHold = [&](const wchar_t* reason) {
        const bool firstHold = !loggedExclusiveHold;
        captureSession = CaptureSessionState::Holding;
        captureStallHold = true;
        exclusiveBlind = true;
        sustainedLiveFrameCount = 0;
        liveFramesSince = SteadyClock::time_point::min();
        softStallBeganAt = SteadyClock::time_point::min();
        softStallRecreates = 0;
        if (firstHold) {
            forceVideoHeartbeat_ = true; // one IDR of freeze; no periodic slideshow
            loggedExclusiveHold = true;
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
        if (captureStallHold || captureSession != CaptureSessionState::Capturing) {
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
            !captureStallHold &&
            captureSession == CaptureSessionState::Capturing &&
            lastVideoSubmission != SteadyClock::time_point::min() &&
            emitTime - lastVideoSubmission >= keyFrameHeartbeatInterval;
        const VideoTimelineEmission emission = timelineScheduler.plan(
            rawSource.frameIndex,
            steadyTimePoint100ns(emitTime),
            intervalCount,
            idleCoalescingAllowed(),
            forcedHeartbeat,
            periodicHeartbeat);

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
            if (!scaler_.scale(
                    d3d_,
                    rawSource,
                    videoSettings.width,
                    videoSettings.height,
                    encodeFrame,
                    captureSettings.multiMonitorSupport && captureSettings.followFocusedMonitor,
                    forceOwnedTexture,
                    encoderInputFormat)) {
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
        if (shouldDropForGpuProtection(emission.duplicateFrame, gpuSettings) ||
            !frameQueue_.tryPush(std::move(frame))) {
            if (forcedHeartbeat) {
                forceVideoHeartbeat_ = true;
            }
            ++droppedFrames_;
            ++gpuProtectionDrops_;
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
        {
            std::scoped_lock gpuLock(pipelineGpuMutex_);
            const DXGI_FORMAT format =
                lastFrame.format != DXGI_FORMAT_UNKNOWN ? lastFrame.format : DXGI_FORMAT_B8G8R8A8_UNORM;
            if (!scaler_.scale(
                    d3d_,
                    lastFrame,
                    lastFrame.width,
                    lastFrame.height,
                    copy,
                    false,
                    true,
                    format)) {
                return;
            }
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
                    captureSession = CaptureSessionState::Capturing;
                    rebindAttempts = 0;
                    loggedExclusiveHold = false;
                    lastCaptureRebindAt = loopNow;
                    lastSourceFrameAt = loopNow;
                    captureStallHold = true;
                    liveFramesSince = SteadyClock::time_point::min();
                    // Target selection was already debounced. Do not hold a valid
                    // first frame behind the recovery gate intended for lost capture.
                    acceptFirstLiveFrameAfterTransition = true;
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
            (captureSession != CaptureSessionState::Capturing &&
             captureSession != CaptureSessionState::Holding) ||
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
            softStallBeganAt = SteadyClock::time_point::min();
            softStallRecreates = 0;
            deadTargetRebindFailures = 0;
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
            const bool desktopRecoveryProbeActive =
                desktopRecoveryProbe &&
                exclusiveCoverActive &&
                desktopBackend;
            const bool freezeDesktopFrame =
                exclusiveCoverActive &&
                desktopBackend &&
                !wgcBackend &&
                !dxgiPresentProof &&
                !desktopRecoveryProbeActive &&
                (captureStallHold ||
                 exclusiveBlind ||
                 exclusiveDxgiAttempts >= 1 ||
                 captureSession != CaptureSessionState::Capturing);
            if (freezeDesktopFrame) {
                // Drain acquire only; keep frozen lastFrame + coalesce.
            } else {
                if (liveFramesSince == SteadyClock::time_point::min()) {
                    liveFramesSince = lastSourceFrameAt;
                }
                ++sustainedLiveFrameCount;
                // Healthy only after sustained live frames (not one transient DXGI blip).
                // During a hold, sustained desktop frames prove a monitor-sized
                // borderless window has left exclusive mode.
                constexpr auto kSustainedLiveMs = std::chrono::milliseconds(500);
                constexpr int kSustainedLiveFrames = 10;
                const bool backendOkForExclusive =
                    !exclusiveCoverActive ||
                    wgcBackend ||
                    dxgiPresentProof ||
                    desktopRecoveryProbeActive ||
                    desktopRecoveryProbe;
                const bool sustained =
                    backendOkForExclusive &&
                    sustainedLiveFrameCount >= kSustainedLiveFrames &&
                    lastSourceFrameAt - liveFramesSince >= kSustainedLiveMs &&
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

                if ((captureStallHold || captureSession != CaptureSessionState::Capturing) &&
                    (acceptFirstLiveFrameAfterTransition || sustained)) {
                    captureStallHold = false;
                    exclusiveBlind = false;
                    captureSession = CaptureSessionState::Capturing;
                    rebindAttempts = 0;
                    exclusiveDxgiAttempts = 0;
                    desktopRecoveryProbe = false;
                    loggedExclusiveHold = false;
                    acceptFirstLiveFrameAfterTransition = false;
                    softStallBeganAt = SteadyClock::time_point::min();
                    softStallRecreates = 0;
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
                        captureSession == CaptureSessionState::Capturing;
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
                captureSession = CaptureSessionState::Capturing;
                rebindAttempts = 0;
                captureStallHold = true;
                lastSourceFrameAt = std::chrono::steady_clock::now();
            }
        } else if (captureTargetLostLatched) {
            // Hard-error hold (finding 18): the capture target is gone and
            // could not be recovered or replaced. Stop rebind churn (which
            // would otherwise retry a dead monitor forever) and hold the last
            // frame while the cadence keeps the timeline continuous. Poll the
            // configured target once per second so replugging recovers.
            captureStallHold = true;
            if (nextTargetLostCheckAt == SteadyClock::time_point::min() ||
                nowLoop >= nextTargetLostCheckAt) {
                nextTargetLostCheckAt = nowLoop + std::chrono::seconds(1);
                const CaptureTarget desired = captureTargetForSettings(captureSettings);
                const bool targetBackOnline =
                    (desired.monitor && monitorAlive(desired.monitor)) ||
                    (desired.window && IsWindow(desired.window));
                if (targetBackOnline) {
                    Logger::instance().info(L"recorder",
                        L"Capture display is back online; recovering capture source");
                    captureTargetLostLatched = false;
                    retargetedToAlternateMonitor = false;
                    deadTargetRebindFailures = 0;
                    rebindAttempts = 0;
                    lastCaptureRebindAt = SteadyClock::time_point::min();
                    captureSession = CaptureSessionState::WaitStable;
                    accessLostAt = nowLoop;
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
                captureSession == CaptureSessionState::Capturing &&
                captureLost &&
                rebindSourceBornAt != SteadyClock::time_point::min() &&
                nowLoop - rebindSourceBornAt < kDxgiInstantDeathMs) {
                enterExclusiveHold(L"DXGI died immediately under exclusive");
            } else if (captureLost && captureSession == CaptureSessionState::Capturing) {
                // Desktop Duplication reports ACCESS_LOST as exclusive mode takes
                // ownership. Rebind straight to the Present hook; one bad first
                // frame is preferable to freezing the replay for seconds.
                immediateRebindPending = exclusiveCoverActive && capture_ &&
                    capture_->backend() == CaptureBackend::DesktopDuplication;
                enterAccessLost(L"ACCESS_LOST / invalid duplication");
            }

            // WGC under exclusive: one recovery path (no CreateForWindow thrash).
            if (captureSession == CaptureSessionState::Capturing &&
                capture_ &&
                capture_->backend() == CaptureBackend::WindowsGraphicsCapture &&
                exclusiveCoverActive &&
                !acquired &&
                nowLoop - lastSourceFrameAt >= kWgcStallUnderExclusiveMs) {
                enterAccessLost(L"WGC stall under exclusive cover");
            }

            if (captureSession == CaptureSessionState::AccessLost) {
                // Already proven blind this cover session → skip rebind thrash.
                if (exclusiveCoverActive && exclusiveBlind) {
                    enterExclusiveHold(L"still exclusive-blind");
                } else {
                    captureSession = CaptureSessionState::WaitStable;
                    accessLostAt = nowLoop;
                }
            }

            if (captureSession == CaptureSessionState::WaitStable) {
                captureStallHold = true;
                if (immediateRebindPending || nowLoop - accessLostAt >= kWaitStableMs) {
                    captureSession = CaptureSessionState::Rebind;
                }
            }

            if (captureSession == CaptureSessionState::Rebind) {
                captureStallHold = true;
                if (lastCaptureRebindAt != SteadyClock::time_point::min() &&
                    nowLoop - lastCaptureRebindAt < kRebindCooldownMs &&
                    !immediateRebindPending) {
                    // Holding lastFrame between rebinds.
                } else {
                    const bool validatedExclusiveTarget =
                        exclusiveCoverActive &&
                        exclusiveTargetWindow &&
                        GetForegroundWindow() == exclusiveTargetWindow;
                    CaptureTarget target = captureSettings.multiMonitorSupport && captureSettings.followFocusedMonitor
                        ? captureTargetForSettings(captureSettings)
                        : activeCaptureTarget_;
                    target.window = validatedExclusiveTarget ? exclusiveTargetWindow : nullptr;
                    ++rebindAttempts;
                    lastCaptureRebindAt = nowLoop;
                    // Exclusive + one DXGI death already observed: never thrash DXGI.
                    const bool skipDxgi =
                        validatedExclusiveTarget && exclusiveDxgiAttempts >= 1;
                    Logger::instance().warning(L"recorder",
                        L"Capture session Rebind (DXGI) attempt=" +
                        std::to_wstring(rebindAttempts) +
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
                        ++exclusiveDxgiAttempts;
                    }
                    if (switched) {
                        rebindSourceBornAt = nowLoop;
                    }

                    captureStallHold = true;
                    liveFramesSince = SteadyClock::time_point::min();
                    sustainedLiveFrameCount = 0;
                    lastSourceFrameAt = nowLoop;
                    const bool fastRebind = immediateRebindPending;
                    immediateRebindPending = false;
                    if (switched) {
                        if (fastRebind) {
                            acceptFirstLiveFrameAfterTransition = true;
                            Logger::instance().info(L"recorder",
                                L"Fast fullscreen transition: immediate Desktop Duplication rebind");
                        }
                        // Desktop mode and provisional DXGI recovery both need
                        // sustained live frames before removing stall hold.
                        captureSession = CaptureSessionState::Capturing;
                    } else if (!validatedExclusiveTarget) {
                        // F11 or focus changed during recovery. A stale
                        // exclusive incident must not force a hold.
                        captureSession = CaptureSessionState::WaitStable;
                        accessLostAt = nowLoop;
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
                        ++deadTargetRebindFailures;
                        constexpr int kMaxDeadTargetRebinds = 4;
                        if (deadTargetRebindFailures >= kMaxDeadTargetRebinds &&
                            !captureTargetLostLatched) {
                            deadTargetRebindFailures = 0;
                            const HMONITOR targetMonitor =
                                activeCaptureTarget_.monitor
                                    ? activeCaptureTarget_.monitor
                                    : monitorFromIndex(activeCaptureTarget_.monitorIndex);
                            bool recovered = false;
                            if (targetMonitor && !monitorAlive(targetMonitor) &&
                                !retargetedToAlternateMonitor) {
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
                                        retargetedToAlternateMonitor = true;
                                        captureSession = CaptureSessionState::Capturing;
                                        captureStallHold = true;
                                        acceptFirstLiveFrameAfterTransition = true;
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
                                captureTargetLostLatched = true;
                                nextTargetLostCheckAt = SteadyClock::time_point::min();
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
                        deadTargetRebindFailures = 0;
                    }
                }
            }

            if (captureSession == CaptureSessionState::Holding) {
                captureStallHold = true;
                if (!exclusiveCoverActive) {
                    Logger::instance().info(L"recorder",
                        L"Exclusive cover ended; rebinding desktop capture");
                    rebindAttempts = 0;
                    exclusiveDxgiAttempts = 0;
                    exclusiveBlind = false;
                    exclusiveTargetWindow = nullptr;
                    loggedExclusiveHold = false;
                    lastCaptureRebindAt = SteadyClock::time_point::min();
                    rebindSourceBornAt = SteadyClock::time_point::min();
                    sustainedLiveFrameCount = 0;
                    CaptureTarget target = captureSettings.multiMonitorSupport && captureSettings.followFocusedMonitor
                        ? captureTargetForSettings(captureSettings)
                        : activeCaptureTarget_;
                    target.window = nullptr;
                    if (switchCaptureTarget(target, L"exclusive cover ended")) {
                        captureSession = CaptureSessionState::Capturing;
                        captureStallHold = true;
                        liveFramesSince = SteadyClock::time_point::min();
                        lastSourceFrameAt = nowLoop;
                        forceVideoHeartbeat_ = true;
                    } else {
                        captureSession = CaptureSessionState::Rebind;
                    }
                } else if (nowLoop - lastCaptureRebindAt >= kHoldDesktopProbeMs) {
                    // Full-monitor borderless still satisfies exclusive-cover geometry.
                    // Probe Desktop Duplication once more; repeated identical
                    // attempts would not make desktop capture recover.
                    exclusiveBlind = false;
                    exclusiveDxgiAttempts = 0;
                    rebindAttempts = 0;
                    desktopRecoveryProbe = true;
                    captureSession = CaptureSessionState::Rebind;
                }
            }

            // Soft stall: coalesce only. Do not thrash from Holding.
            if (captureSession == CaptureSessionState::Capturing &&
                !acquired && !captureLost) {
                const auto noFrameFor = nowLoop - lastSourceFrameAt;
                if (exclusiveCoverActive && noFrameFor >= kExclusiveNoFrameMs) {
                    if (exclusiveBlind || exclusiveDxgiAttempts >= 1) {
                        enterExclusiveHold(L"no live frames under exclusive cover");
                    } else {
                        enterAccessLost(L"no live frames under exclusive cover");
                    }
                } else if (!exclusiveCoverActive && noFrameFor >= kSoftStallMs) {
                    captureStallHold = true;
                    sustainedLiveFrameCount = 0;
                    // Escalate a sustained non-exclusive stall: coalescing
                    // alone keeps encoding a freeze-frame forever when the
                    // source wedges without reporting device loss. Recreate
                    // the desktop capture source after the stall persists,
                    // bounded per episode and cooldown-limited.
                    const bool desktopSourceRecoverable = capture_ != nullptr;
                    if (softStallBeganAt == SteadyClock::time_point::min()) {
                        softStallBeganAt = nowLoop - noFrameFor;
                    }
                    const bool recreateCooldownElapsed =
                        lastSoftStallRecreateAt == SteadyClock::time_point::min() ||
                        nowLoop - lastSoftStallRecreateAt >= kSoftStallRecreateCooldownMs;
                    if (desktopSourceRecoverable &&
                        softStallRecreates < kMaxSoftStallRecreates &&
                        recreateCooldownElapsed &&
                        nowLoop - softStallBeganAt >= kSoftStallRecreateAfterMs) {
                        ++softStallRecreates;
                        lastSoftStallRecreateAt = nowLoop;
                        softStallBeganAt = nowLoop;
                        CaptureTarget target = captureSettings.multiMonitorSupport && captureSettings.followFocusedMonitor
                            ? captureTargetForSettings(captureSettings)
                            : activeCaptureTarget_;
                        target.window = nullptr;
                        Logger::instance().warning(L"recorder",
                            L"No frames for " +
                            std::to_wstring(
                                std::chrono::duration_cast<std::chrono::milliseconds>(nowLoop - lastSourceFrameAt).count()) +
                            L"ms without device loss; recreating capture source (attempt " +
                            std::to_wstring(softStallRecreates) + L"/" +
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
            const bool cursorPreference = settings().captureCursor;
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
    applyAudioVolume(packet, volumePercent);

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
            const uint32_t mutedCount = WasapiCapture::applySessionMutesForExecutables(mutedKeys);
            systemAudioSessionMutesActive_ = mutedCount > 0;
        }
        return;
    }

    systemAudioMutedExecutableKeys_ = mutedKeys;
    systemAudio_.stop();

    if (mutedKeys.empty()) {
        WasapiCapture::clearSessionMutes();
        systemAudioSessionMutesActive_ = false;
    } else {
        const uint32_t mutedCount = WasapiCapture::applySessionMutesForExecutables(mutedKeys);
        systemAudioSessionMutesActive_ = mutedCount > 0;
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
        systemAudioSessionMutesActive_ = false;
        systemAudioMutedExecutableKeys_.clear();
    }
}

std::filesystem::path RecorderController::nextClipPath(const wchar_t* prefix) const {
    const auto snapshot = settings();
    const std::filesystem::path initial = snapshot.clipDirectory / makeTimestampedFileName(prefix, L".mp4");
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
        const auto candidate = snapshot.clipDirectory / (stem + L"_" + std::to_wstring(suffix) + L".mp4");
        if (reserveClipOutputName(candidate) == OutputNameReservation::Reserved) {
            return candidate;
        }
        error = {};
        if (!std::filesystem::exists(candidate, error)) {
            return candidate;
        }
    }

    return snapshot.clipDirectory /
           (stem + L"_" + std::to_wstring(GetTickCount64()) + L".mp4");
}

void RecorderController::setLastRecordingError(std::wstring error) {
    std::scoped_lock lock(recordingErrorMutex_);
    lastRecordingError_ = std::move(error);
}

} // namespace backtrack
