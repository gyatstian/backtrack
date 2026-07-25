#include "app/RecorderController.h"
#include "app/VideoTimelineScheduler.h"

#include "capture/DesktopDuplicationCapture.h"
#include "capture/WgcCaptureSource.h"
#include "capture/game/GameCaptureSource.h"
#include "core/Logger.h"
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

const wchar_t* captureBackendDisplayName(CaptureBackend backend) {
    switch (backend) {
    case CaptureBackend::WindowsGraphicsCapture:
        return L"Windows Graphics Capture";
    case CaptureBackend::DesktopDuplication:
        return L"Desktop Duplication";
    case CaptureBackend::GameCapture:
        return L"Game Capture";
    }
    return L"Unknown";
}

CaptureBackend selectedBackendForSettings(const AppSettings& settings) {
    return settings.preferredCaptureBackend;
}

VideoSettings activeVideoSettingsFor(const AppSettings& settings, uint32_t sourceWidth, uint32_t sourceHeight) {
    VideoSettings active = settings.video;
    if (settings.video.resolutionMode == ResolutionMode::Native && sourceWidth > 0 && sourceHeight > 0) {
        // Keep native source size even when follow-monitor is enabled so the
        // first monitor's resolution is encoded 1:1; later switches letterbox
        // into this canvas via the scaler fit-with-bars path.
        active.width = evenEncodeDimension(sourceWidth);
        active.height = evenEncodeDimension(sourceHeight);
    } else {
        active.width = evenEncodeDimension(active.width);
        active.height = evenEncodeDimension(active.height);
    }
    return active;
}

bool isWaveExtensibleSubFormat(const GUID& guid, uint16_t tag) {
    static constexpr std::array<uint8_t, 8> kWaveSubFormatTail = {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
    return guid.Data1 == tag &&
           guid.Data2 == 0 &&
           guid.Data3 == 0x0010 &&
           std::equal(std::begin(guid.Data4), std::end(guid.Data4), kWaveSubFormatTail.begin());
}

enum class AudioSampleEncoding {
    Pcm,
    Float,
    Unsupported,
};

struct AudioFormatInfo {
    AudioSampleEncoding encoding = AudioSampleEncoding::Unsupported;
    uint16_t bitsPerSample = 0;
    uint16_t channels = 0;
    uint32_t samplesPerSec = 0;
    uint16_t blockAlign = 0;
};

AudioFormatInfo audioFormatInfo(const WaveFormatBlob& blob) {
    if (blob.bytes.size() < sizeof(WAVEFORMATEX)) {
        return {};
    }

    const auto* format = reinterpret_cast<const WAVEFORMATEX*>(blob.bytes.data());
    AudioFormatInfo info;
    info.bitsPerSample = format->wBitsPerSample;
    info.channels = format->nChannels;
    info.samplesPerSec = format->nSamplesPerSec;
    info.blockAlign = format->nBlockAlign;
    if (format->wFormatTag == WAVE_FORMAT_PCM) {
        info.encoding = AudioSampleEncoding::Pcm;
    } else if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        info.encoding = AudioSampleEncoding::Float;
    } else if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && blob.bytes.size() >= sizeof(WAVEFORMATEXTENSIBLE)) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(blob.bytes.data());
        if (isWaveExtensibleSubFormat(extensible->SubFormat, WAVE_FORMAT_PCM)) {
            info.encoding = AudioSampleEncoding::Pcm;
        } else if (isWaveExtensibleSubFormat(extensible->SubFormat, WAVE_FORMAT_IEEE_FLOAT)) {
            info.encoding = AudioSampleEncoding::Float;
        }
    }
    return info;
}

int32_t readSigned24(const uint8_t* sample);
void writeSigned24(uint8_t* sample, int32_t value);

bool audioFormatUsable(const AudioFormatInfo& format) {
    const uint16_t bytesPerSample = format.bitsPerSample / 8;
    return format.encoding != AudioSampleEncoding::Unsupported &&
           format.bitsPerSample > 0 &&
           format.bitsPerSample % 8 == 0 &&
           bytesPerSample > 0 &&
           format.channels > 0 &&
           format.samplesPerSec > 0 &&
           format.blockAlign >= static_cast<uint16_t>(format.channels * bytesPerSample);
}

std::wstring normalizedExecutableKey(const std::filesystem::path& path) {
    std::wstring value = path.wstring();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return value;
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
    const AudioFormatInfo format = audioFormatInfo(*packet.format);
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

RecorderController::RecorderController()
    : frameQueue_(8),
      systemAudioQueue_(256),
      microphoneAudioQueue_(256) {
    frameEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    audioEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

RecorderController::~RecorderController() {
    shutdown();
    if (frameEvent_) {
        CloseHandle(frameEvent_);
        frameEvent_ = nullptr;
    }
    if (audioEvent_) {
        CloseHandle(audioEvent_);
        audioEvent_ = nullptr;
    }
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
        replay_.clear();
        replay_.configure(settings_.replay);
    }
    Logger::instance().info(L"recorder", std::wstring(L"Recorder initialized: clips=") + settings_.clipDirectory.wstring() +
                            L", replay=" + (settings_.replay.enabled ? L"enabled" : L"disabled") +
                            L", fps=" + std::to_wstring(settings_.video.fps) +
                            L", bitrateKbps=" + std::to_wstring(settings_.video.bitrateKbps));
    return true;
}

bool RecorderController::updateSettings(AppSettings settings) {
    settings = sanitizeSettings(std::move(settings));
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
        settings_ = std::move(settings);
        activeVideoSettings_ = settings_.video;
        activeGpuSettings_ = settings_.gpu;
        audioOutputVolumePercent_.store(settings_.audioOutputVolumePercent, std::memory_order_relaxed);
        audioInputVolumePercent_.store(settings_.audioInputVolumePercent, std::memory_order_relaxed);
        replay_.configure(settings_.replay);
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
        Logger::instance().error(L"recorder", L"Recording muxer could not start for output: " + output.wstring());
        if (!settings().replay.enabled) {
            stopPipeline();
        }
        return false;
    }
    lastEncodedVideoPts100ns_ = 0;
    videoTimelineEndPts100ns_ = 0;
    waitingForRecordingKeyFrame_ = true;
    recording_ = true;
    forceVideoHeartbeat_ = true;
    {
        std::scoped_lock gpuLock(pipelineGpuMutex_);
        if (encoder_) {
            encoder_->requestKeyFrame();
        }
    }
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
    // Buffer already has keyframes: mux without restarting capture (avoids WGC
    // recreate hangs and is unnecessary work when pipeline is already warm).
    if (replay_.videoKeyFrameCount() == 0) {
        Logger::instance().info(L"recorder", L"Replay save has no buffered keyframe; ensuring pipeline and requesting IDR");
        if (!ensurePipeline()) {
            Logger::instance().error(L"recorder", L"Replay could not be saved because the capture pipeline failed to start");
            return {};
        }
        forceVideoHeartbeat_ = true;
        {
            std::scoped_lock gpuLock(pipelineGpuMutex_);
            if (encoder_) {
                encoder_->requestKeyFrame();
            }
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (replay_.videoKeyFrameCount() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (replay_.videoKeyFrameCount() == 0) {
            Logger::instance().warning(L"recorder", L"Replay save aborted: no keyframe arrived within 3s");
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
    const bool saved = replay_.saveTo(output, videoSettings);
    if (!saved) {
        Logger::instance().warning(L"recorder", std::wstring(L"Replay save failed: ") + output.wstring());
    }
    return saved ? output : std::filesystem::path();
}

void RecorderController::shutdown() {
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

AntiCheatKind RecorderController::consumeAntiCheatBlock() {
    std::scoped_lock lock(captureStatusMutex_);
    if (!captureAntiCheatBlocked_) {
        return AntiCheatKind::None;
    }
    captureAntiCheatBlocked_ = false;
    return captureAntiCheatKind_;
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
                                 : snapshot.preferredCaptureBackend == CaptureBackend::GameCapture
                                     ? L"Game Capture"
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

    replay_.configure(snapshot.replay);
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
    lastEncodedVideoPts100ns_ = 0;
    videoTimelineEndPts100ns_ = 0;

    if (snapshot.captureSystemAudio) {
        refreshSystemAudioCapture(snapshot);
    }
    if (snapshot.captureMicrophone) {
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
    // Do not drain stale GPU work during a settings/backend switch. This lets
    // encodeLoop release frame leases and exit instead of submitting old input.
    discardQueuedFrames_.store(true, std::memory_order_release);
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
    const VideoSettings videoSettings = activeVideoSettingsFor(snapshot, sourceWidth, sourceHeight);
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
    Logger::instance().info(L"recorder",
        L"GPU pipeline ready: capture=" + std::to_wstring(sourceWidth) + L"x" + std::to_wstring(sourceHeight) +
        L", encode=" + std::to_wstring(videoSettings.width) + L"x" + std::to_wstring(videoSettings.height) +
        L", adapter=" + d3d_.adapterName());
    return true;
}

bool RecorderController::createCaptureSource(
    const CaptureTarget& target,
    bool exclusiveRecovery,
    bool allowDesktopDuplication,
    bool allowGameCapture,
    bool* gameCaptureAttempted) {
    if (gameCaptureAttempted) {
        *gameCaptureAttempted = false;
    }
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

    bool gameCapTargetDied = false;
    bool gameCapAntiCheatBlocked = false;
    AntiCheatKind gameCapAntiCheatKind = AntiCheatKind::None;
    auto tryInitialize = [&](std::unique_ptr<ICaptureSource> source, const wchar_t* name,
                             const CaptureTarget& initTarget) -> std::unique_ptr<ICaptureSource> {
        Logger::instance().info(L"recorder", std::wstring(L"Initializing capture source: ") + name);
        if (source->initialize(d3d_, snapshot, initTarget)) {
            Logger::instance().info(L"recorder", std::wstring(L"Capture source initialized: ") + name);
            return std::move(source);
        }
        Logger::instance().warning(L"recorder", std::wstring(L"Capture source initialization failed: ") + name);
        if (source->backend() == CaptureBackend::GameCapture) {
            auto* gc = static_cast<GameCaptureSource*>(source.get());
            if (gc->targetProcessExited()) {
                gameCapTargetDied = true;
            }
            if (gc->antiCheatBlocked()) {
                gameCapAntiCheatBlocked = true;
                gameCapAntiCheatKind = gc->antiCheatKind();
            }
        }
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

    // Auto Game Capture is exclusive-only. Windowed/borderless recovery must
    // return to desktop composition so title bars and taskbar remain recorded.
    const GameCaptureMode gameMode = snapshot.gameCaptureMode;
    const bool tryGameCapture =
        allowGameCapture &&
        gameMode != GameCaptureMode::Off &&
        (exclusiveRecovery ||
         !allowDesktopDuplication ||
         gameMode == GameCaptureMode::On ||
         selectedBackend == CaptureBackend::GameCapture);
    if (tryGameCapture) {
        // Preserve exclusiveTargetWindow validated by the recovery state
        // machine. monitorTarget intentionally has no window for desktop APIs.
        CaptureTarget gameTarget = target;
        // Automatic exclusive recovery must inject only the window validated
        // when the capture loss occurred, never whichever app is foreground.
        if (!gameTarget.window && !exclusiveRecovery) {
            gameTarget.window = GetForegroundWindow();
        }
        if (gameTarget.window) {
            if (gameCaptureAttempted) {
                *gameCaptureAttempted = true;
            }
            initialized = tryInitialize(
                std::make_unique<GameCaptureSource>(), L"Game Capture", gameTarget);
        }
    }

    // GameCap-only exclusive probe: do not fall through to DXGI/WGC.
    if (!initialized && !allowDesktopDuplication) {
        std::scoped_lock lock(captureStatusMutex_);
        captureBackendActive_ = false;
        captureBackendFallbackUsed_ = false;
        captureBackendStatus_ = L"Game capture unavailable; exclusive hold";
        return false;
    }

    // Inject killed the game — never thrash DXGI as fake recovery.
    if (!initialized && gameCapTargetDied && exclusiveRecovery) {
        Logger::instance().warning(
            L"recorder",
            L"Game died on inject — exclusive hold, no DXGI thrash");
        std::scoped_lock lock(captureStatusMutex_);
        captureBackendActive_ = false;
        captureBackendFallbackUsed_ = false;
        captureBackendStatus_ = L"Exclusive: game died on inject (holding last frame)";
        return false;
    }

    // Respect an explicit DXGI choice before considering WGC window capture.
    // Window targeting is an optimization, not permission to override capture
    // method selected by user.
    if (!initialized && selectedBackend == CaptureBackend::DesktopDuplication &&
        !exclusiveRecovery && dxgiTargetOnActiveAdapter) {
        initialized = tryInitialize(
            std::make_unique<DesktopDuplicationCapture>(), L"Desktop Duplication", monitorTarget);
    }

    // Normal WGC path can target window. Exclusive recovery uses monitor DXGI
    // only after validated Game Capture attempt.
    if (!initialized && hasWindowTarget && !exclusiveRecovery &&
        selectedBackend != CaptureBackend::DesktopDuplication) {
        initialized = tryInitialize(
            std::make_unique<WgcCaptureSource>(), L"Windows Graphics Capture (window)", windowTarget);
    }

    if (!initialized && exclusiveRecovery && allowDesktopDuplication &&
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

    if (!initialized && selectedBackend == CaptureBackend::GameCapture &&
        dxgiTargetOnActiveAdapter &&
        !exclusiveRecovery) {
        initialized = tryInitialize(
            std::make_unique<DesktopDuplicationCapture>(), L"Desktop Duplication", monitorTarget);
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

    // Exclusive recovery: if GameCapture and DXGI both
    // failed, leave capture null so the loop deep-holds lastFrame. Do not thrash WGC.

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
        if (gameCapAntiCheatBlocked) {
            // Anti-cheat titles never receive the injected hook. Surface the
            // reason explicitly so the user understands why WGC is active.
            captureBackendStatus_ = std::wstring(L"Game Capture disabled for anti-cheat title (") +
                antiCheatDisplayName(gameCapAntiCheatKind) +
                L"), using " + captureBackendDisplayName(activeBackend);
        } else if (fallbackUsed) {
            captureBackendStatus_ += dxgiSkippedForTarget
                ? L" (DXGI unavailable for target adapter)"
                : selectedBackend == CaptureBackend::DesktopDuplication
                    ? L" (fallback after DXGI failed)"
                    : L" (fallback after WGC failed)";
        }
    }
    {
        // Edge-triggered latch: the observer fires the failure beep once per
        // transition into the anti-cheat-blocked state. Re-arm when a later
        // rebind is no longer blocked so a subsequent block beeps again.
        std::scoped_lock lock(captureStatusMutex_);
        captureAntiCheatBlocked_ = gameCapAntiCheatBlocked;
        captureAntiCheatKind_ = gameCapAntiCheatBlocked ? gameCapAntiCheatKind : AntiCheatKind::None;
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
    // Known fullscreen mode switches may accept the first fresh source frame. The
    // longer sustained-frame gate remains for ambiguous capture-loss recovery.
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
    bool exclusiveCoverActive = false;
    HWND exclusiveTargetWindow = nullptr;
    bool loggedExclusiveHold = false;
    bool exclusiveBlind = false;
    // A monitor-sized borderless game remains an exclusive-cover candidate.
    // Periodically test desktop composition without retrying Game Capture.
    bool desktopRecoveryProbe = false;
    auto lastGameCapFailureAt = SteadyClock::time_point::min();
    auto rebindSourceBornAt = SteadyClock::time_point::min();
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
        std::scoped_lock gpuLock(pipelineGpuMutex_);
        return gpuSettings.allowIdleFrameSkipping &&
               encoder_ && encoder_->capabilities().effective.zeroReorderDelay;
    };
    const auto monitorPollInterval = std::chrono::milliseconds(250);
    const auto soundSeparationPollInterval = std::chrono::seconds(2);
    const auto monitorSwitchStableInterval = std::chrono::milliseconds(1500);
    const auto monitorSwitchCooldown = std::chrono::milliseconds(3000);
    auto lastMonitorSwitch = SteadyClock::time_point::min();

    auto emitFrame = [&](const GpuFrame& source, SteadyClock::time_point emitTime, uint32_t intervalCount) {
        const bool forcedHeartbeat = forceVideoHeartbeat_.exchange(false);
        // Stall/hold: suppress periodic GOP Submit of same texture (slideshow).
        // Forced heartbeat (enter hold / recover) still allowed for seekability.
        const bool periodicHeartbeat =
            !captureStallHold &&
            captureSession == CaptureSessionState::Capturing &&
            lastVideoSubmission != SteadyClock::time_point::min() &&
            emitTime - lastVideoSubmission >= keyFrameHeartbeatInterval;
        const VideoTimelineEmission emission = timelineScheduler.plan(
            source.frameIndex,
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

        GpuFrame frame = source;
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
                                    bool exclusiveRecovery = false,
                                    bool allowDesktopDuplication = true,
                                    bool allowGameCapture = true,
                                    bool* gameCaptureAttempted = nullptr) -> bool {
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
            if (!createCaptureSource(
                    target,
                    exclusiveRecovery,
                    allowDesktopDuplication,
                    allowGameCapture,
                    gameCaptureAttempted) ||
                !capture_) {
                return false;
            }
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
                }
            }
        }

        // Missing-capture recovery is handled below with cooldown so lastFrame
        // heartbeats keep running during exclusive-fullscreen DuplicateOutput waits.

        const auto nowLoop = std::chrono::steady_clock::now();
        exclusiveCoverActive =
            foregroundWindowCoversMonitor(monitorForCaptureTarget(activeCaptureTarget_));

        // Auto Game Capture has game-backbuffer semantics. Once F11 returns to
        // windowed mode, immediately recover desktop composition instead.
        if (captureSession == CaptureSessionState::Capturing &&
            capture_ && capture_->backend() == CaptureBackend::GameCapture &&
            captureSettings.gameCaptureMode == GameCaptureMode::Auto &&
            !exclusiveCoverActive) {
            // F11 back to windowed mode is a known source change, not a failed
            // capture session. Swap directly instead of entering recovery hold.
            CaptureTarget target = captureSettings.multiMonitorSupport && captureSettings.followFocusedMonitor
                ? captureTargetForSettings(captureSettings)
                : activeCaptureTarget_;
            target.window = nullptr;
            if (switchCaptureTarget(target, L"exclusive cover ended", false)) {
                captureSession = CaptureSessionState::Capturing;
                captureStallHold = true;
                acceptFirstLiveFrameAfterTransition = true;
                immediateRebindPending = false;
                lastSourceFrameAt = nowLoop;
                forceVideoHeartbeat_ = true;
                Logger::instance().info(L"recorder",
                    L"Fast fullscreen transition: Game Capture -> desktop capture");
            } else {
                enterAccessLost(L"exclusive cover ended while Game Capture active");
            }
        }

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
        if (acquired) {
            lastSourceFrameAt = std::chrono::steady_clock::now();
            // Exclusive + stall/hold/blind: desktop WGC/DXGI frames are sparse or
            // stale. Do not promote them into lastFrame (slideshow). Healthy
            // continuous desktop capture under borderless cover still encodes.
            // GameCapture always counts as live.
            const bool desktopBackend =
                capture_ &&
                capture_->backend() != CaptureBackend::GameCapture;
            const bool desktopRecoveryProbeActive =
                desktopRecoveryProbe &&
                exclusiveCoverActive &&
                desktopBackend;
            const bool freezeDesktopFrame =
                exclusiveCoverActive &&
                desktopBackend &&
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
                    desktopRecoveryProbeActive ||
                    desktopRecoveryProbe ||
                    (capture_ && capture_->backend() == CaptureBackend::GameCapture);
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

                GpuFrame encodeFrame;
                bool scaled = false;
                {
                    std::scoped_lock gpuLock(pipelineGpuMutex_);
                    const bool forceOwnedTexture =
                        (capture_ &&
                         (capture_->backend() == CaptureBackend::WindowsGraphicsCapture ||
                          capture_->backend() == CaptureBackend::DesktopDuplication ||
                          capture_->backend() == CaptureBackend::GameCapture)) ||
                        (captureSettings.multiMonitorSupport && captureSettings.followFocusedMonitor && gpuSettings.stableMultimonitorFrames);
                    scaled = scaler_.scale(
                        d3d_,
                        frame,
                        videoSettings.width,
                        videoSettings.height,
                        encodeFrame,
                        captureSettings.multiMonitorSupport && captureSettings.followFocusedMonitor,
                        forceOwnedTexture,
                        encoderInputFormat);
                }
                if (!scaled) {
                    ++droppedFrames_;
                    liveFramesSince = SteadyClock::time_point::min();
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
                    forceVideoHeartbeat_ = true;
                    Logger::instance().info(L"recorder", L"Capture session Capturing (live frames resumed)");
                }

                lastFrame = encodeFrame;
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
                    // Use encodeFrame.pts100ns (WGC SystemRelativeTime, QPC epoch == MSVC
                    // steady_clock) NOT the drain wall-clock (lastSourceFrameAt): the
                    // drain instant is scheduling-jittered and only trades the structural
                    // beat for arrival jitter. The present time is the evenly-spaced clock.
                    //
                    // Scope-gated: only healthy windowed desktop capture. Exclusive-
                    // fullscreen and GameCapture paths keep the free-running grid (their
                    // present times are already 1:1 with vsync -- no beat to fix, and we
                    // must not perturb the smooth path or the game-capture pipeline).
                    const bool relockPhase =
                        desktopBackend &&
                        !exclusiveCoverActive &&
                        captureSession == CaptureSessionState::Capturing;
                    // Only snap forward and only past the last emitted instant, so the
                    // emit timeline stays strictly monotonic (advanceVideoCadence emits
                    // at emitTime == nextEmit; a backward snap would double-emit).
                    const auto presentInstant = steadyPointFrom100ns(encodeFrame.pts100ns);
                    if (relockPhase &&
                        presentInstant > nextEmit &&
                        presentInstant > lastEmitTime) {
                        nextEmit = presentInstant;
                    }
                }
            }
        }

        bool captureLost = false;
        bool deviceRemoved = false;
        bool encoderFaulted = false;
        {
            std::scoped_lock gpuLock(pipelineGpuMutex_);
            captureLost = !capture_ || capture_->isDeviceLost();
            deviceRemoved = d3d_.isDeviceRemoved();
            encoderFaulted = encoder_ && !encoder_->stats().encoderAvailable;
        }

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
        } else {
            // ---- Capture session state machine (exclusive-aware) ----
            // Exclusive rebind: GameCap first; DXGI only if GameCap not just failed.
            // GameCap death / cooldown → hold (no DXGI thrash / slideshow).
            constexpr auto kWaitStableMs = std::chrono::milliseconds(1200);
            constexpr auto kRebindCooldownMs = std::chrono::milliseconds(2500);
            constexpr auto kHoldDesktopProbeMs = std::chrono::milliseconds(2000);
            // WGC legitimately remains quiet for unchanged borderless content.
            // Require a sustained stall before treating a full-monitor window
            // as exclusive and switching to Present-hook capture.
            constexpr auto kWgcStallUnderExclusiveMs = std::chrono::milliseconds(2000);
            constexpr auto kMaxFastRebinds = 1;
            constexpr auto kExclusiveNoFrameMs = std::chrono::milliseconds(2000);
            constexpr auto kPostCrashInjectCooldownMs = std::chrono::milliseconds(60000);
            constexpr auto kDxgiInstantDeathMs = std::chrono::milliseconds(400);
            constexpr auto kSoftStallMs = std::chrono::milliseconds(750);

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
                    const bool skipGameCap =
                        lastGameCapFailureAt != SteadyClock::time_point::min() &&
                        nowLoop - lastGameCapFailureAt < kPostCrashInjectCooldownMs;
                    // Exclusive + GameCap just failed/cooldown: never thrash DXGI.
                    // Exclusive + one DXGI death: also skip further DXGI.
                    const bool skipDxgi =
                        validatedExclusiveTarget &&
                        (exclusiveDxgiAttempts >= 1 ||
                         (skipGameCap && !desktopRecoveryProbe));
                    Logger::instance().warning(L"recorder",
                        L"Capture session Rebind (game/DXGI) attempt=" +
                        std::to_wstring(rebindAttempts) +
                        L", exclusiveCover=" + (exclusiveCoverActive ? L"yes" : L"no") +
                        L", skipGameCap=" + (skipGameCap ? L"yes" : L"no") +
                        L", skipDxgi=" + (skipDxgi ? L"yes" : L"no"));

                    bool switched = false;
                    bool gameCaptureAttempted = false;
                    if (skipDxgi && skipGameCap) {
                        switched = false;
                    } else {
                        switched = switchCaptureTarget(
                            target,
                            validatedExclusiveTarget
                                ? L"exclusive capture rebind"
                                : L"desktop capture rebind",
                            validatedExclusiveTarget,
                             !skipDxgi,
                            validatedExclusiveTarget && !skipGameCap && !desktopRecoveryProbe,
                            &gameCaptureAttempted);
                        if (validatedExclusiveTarget && capture_ &&
                            capture_->backend() == CaptureBackend::DesktopDuplication) {
                            ++exclusiveDxgiAttempts;
                        }
                        if (validatedExclusiveTarget && gameCaptureAttempted &&
                            (!switched ||
                             !capture_ ||
                             capture_->backend() != CaptureBackend::GameCapture)) {
                            lastGameCapFailureAt = nowLoop;
                        }
                        if (switched &&
                            capture_ &&
                            capture_->backend() != CaptureBackend::GameCapture) {
                            rebindSourceBornAt = nowLoop;
                        }
                    }

                    captureStallHold = true;
                    liveFramesSince = SteadyClock::time_point::min();
                    sustainedLiveFrameCount = 0;
                    lastSourceFrameAt = nowLoop;
                    if (switched && immediateRebindPending) {
                        acceptFirstLiveFrameAfterTransition = true;
                        Logger::instance().info(L"recorder",
                            L"Fast fullscreen transition: desktop capture -> Game Capture");
                    }
                    immediateRebindPending = false;

                    if (switched && capture_ &&
                        capture_->backend() == CaptureBackend::GameCapture) {
                        lastGameCapFailureAt = SteadyClock::time_point::min();
                        rebindSourceBornAt = SteadyClock::time_point::min();
                        // Stay stall-hold until sustained live frames prove health.
                        captureSession = CaptureSessionState::Capturing;
                    } else if (switched) {
                        // Desktop mode and provisional DXGI recovery both need
                        // sustained live frames before removing stall hold.
                        captureSession = CaptureSessionState::Capturing;
                    } else if (!validatedExclusiveTarget) {
                        // F11 or focus changed during recovery. A stale
                        // exclusive incident must not force Game Capture/hold.
                        captureSession = CaptureSessionState::WaitStable;
                        accessLostAt = nowLoop;
                    } else if (
                        rebindAttempts >= kMaxFastRebinds || skipGameCap || skipDxgi || !switched) {
                        enterExclusiveHold(
                            !switched ? L"GameCap/DXGI unavailable"
                                      : L"desktop APIs blind under exclusive");
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
                    // Keep inject cooldown after GameCap crash; only clear on success path.
                    rebindSourceBornAt = SteadyClock::time_point::min();
                    sustainedLiveFrameCount = 0;
                    CaptureTarget target = captureSettings.multiMonitorSupport && captureSettings.followFocusedMonitor
                        ? captureTargetForSettings(captureSettings)
                        : activeCaptureTarget_;
                    target.window = nullptr;
                    // Desktop only — do not inject GameCapture after alt-tab.
                    if (switchCaptureTarget(target, L"exclusive cover ended", false)) {
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
                    // Probe Desktop Duplication only; Vanguard blocks Game Capture and
                    // repeated injection attempts would not make desktop capture recover.
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
                    if (exclusiveBlind || exclusiveDxgiAttempts >= 1 ||
                        (lastGameCapFailureAt != SteadyClock::time_point::min() &&
                         nowLoop - lastGameCapFailureAt < kPostCrashInjectCooldownMs)) {
                        enterExclusiveHold(L"no live frames under exclusive cover");
                    } else {
                        enterAccessLost(L"no live frames under exclusive cover");
                    }
                } else if (!exclusiveCoverActive && noFrameFor >= kSoftStallMs) {
                    captureStallHold = true;
                    sustainedLiveFrameCount = 0;
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
        }
    }

    disableMmcssForThread(mmcssHandle);
}

void RecorderController::encodeLoop() {
    setThreadDescriptionSafe(L"Backtrack encode");

    constexpr size_t kMaxPendingDurations = 512;
    std::unordered_map<int64_t, int64_t> pendingDurations;

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
        discardCompletedGeneration_.store(
            discardRequestGeneration_.load(std::memory_order_acquire),
            std::memory_order_release);
        frameDiscardComplete_.notify_all();
    };

    auto extendVideoDuration = [&](int64_t pts100ns, int64_t duration100ns) {
        if (pts100ns <= 0 || duration100ns <= 0) {
            return;
        }
        if (pendingDurations.size() >= kMaxPendingDurations &&
            pendingDurations.find(pts100ns) == pendingDurations.end()) {
            pendingDurations.clear();
            Logger::instance().warning(L"recorder", L"Discarded unmatched pending video durations after reaching limit");
        }
        auto [it, inserted] = pendingDurations.try_emplace(pts100ns, duration100ns);
        if (!inserted) {
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
}

void RecorderController::handleAudioPacket(AudioPacket&& packet) {
    if (packet.track == AudioTrack::SoundSeparation) {
        return;
    }

    const AudioTrack track = packet.track;
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
    std::error_code error;
    if (!std::filesystem::exists(initial, error)) {
        return initial;
    }

    const std::wstring stem = initial.stem().wstring();
    for (uint32_t suffix = 2; suffix < 1000; ++suffix) {
        const auto candidate = snapshot.clipDirectory / (stem + L"_" + std::to_wstring(suffix) + L".mp4");
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
