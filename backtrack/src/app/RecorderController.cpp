#include "app/RecorderController.h"
#include "app/VideoTimelineScheduler.h"

#include "capture/DesktopDuplicationCapture.h"
#include "capture/WgcCaptureSource.h"
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
    if (settings.followFocusedMonitor) {
        target.monitor = settings.followMouseMonitor
            ? cursorMonitorOrFallback(settings.monitorIndex)
            : focusedMonitorOrFallback(settings.monitorIndex);
    } else {
        target.monitor = monitorFromIndex(settings.monitorIndex);
    }
    return target;
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
    }
    return L"Unknown";
}

CaptureBackend selectedBackendForSettings(const AppSettings& settings) {
    return settings.followFocusedMonitor
        ? CaptureBackend::WindowsGraphicsCapture
        : settings.preferredCaptureBackend;
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
    Logger::instance().info(std::wstring(L"Recorder initialized: clips=") + settings_.clipDirectory.wstring() +
                            L", replay=" + (settings_.replay.enabled ? L"enabled" : L"disabled") +
                            L", fps=" + std::to_wstring(settings_.video.fps) +
                            L", bitrateKbps=" + std::to_wstring(settings_.video.bitrateKbps));
    return true;
}

bool RecorderController::updateSettings(AppSettings settings) {
    settings = sanitizeSettings(std::move(settings));
    const bool wasRunning = pipelineRunning_.load();
    const bool wasRecording = recording_.load();
    Logger::instance().info(std::wstring(L"Applying recorder settings: pipelineRunning=") + (wasRunning ? L"yes" : L"no") +
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
            Logger::instance().error(L"Could not restart capture pipeline after applying settings");
        }
        return started;
    }
    Logger::instance().info(wasRecording
                                ? L"Recorder settings applied; recording was stopped and replay is disabled, so the pipeline remains stopped"
                                : L"Recorder settings applied without starting pipeline because replay is disabled and no recording is active");
    return true;
}

bool RecorderController::startRecording() {
    if (recording_) {
        Logger::instance().info(L"Start recording requested while already recording");
        return true;
    }
    Logger::instance().info(L"Start recording requested");
    setLastRecordingError({});
    if (!ensurePipeline()) {
        Logger::instance().error(L"Recording could not start because the capture pipeline failed to start");
        return false;
    }

    const auto output = nextClipPath(L"recording");
    VideoSettings videoSettings;
    {
        std::scoped_lock lock(stateMutex_);
        videoSettings = activeVideoSettings_;
    }
    if (!muxer_.startRecording(output, videoSettings)) {
        Logger::instance().error(L"Recording muxer could not start for output: " + output.wstring());
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
    Logger::instance().info(L"Recording started");
    return true;
}

std::filesystem::path RecorderController::stopRecording() {
    if (!recording_) {
        Logger::instance().info(L"Stop recording requested while no recording is active");
        return {};
    }
    Logger::instance().info(L"Stop recording requested");

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
    Logger::instance().info(output.empty()
                                ? std::wstring(L"Recording stopped without a finalized clip")
                                : std::wstring(L"Recording stopped and saved: ") + output.wstring());

    if (!snapshot.replay.enabled && pipelineRunning_) {
        stopPipeline();
    }
    return output;
}

std::filesystem::path RecorderController::recoverFailedRecording() {
    Logger::instance().info(L"Recover failed recording requested");
    if (recording_) {
        const std::wstring detail = L"Stop the active recording before recovering a failed recording";
        setLastRecordingError(detail);
        Logger::instance().warning(detail);
        return {};
    }

    std::wstring detail;
    const auto output = Mp4Muxer::recoverLatestFailedRecording(settings().clipDirectory, detail);
    setLastRecordingError(detail);
    if (output.empty()) {
        Logger::instance().warning(detail.empty() ? L"Failed recording recovery did not produce a clip" : detail);
    }
    return output;
}

std::filesystem::path RecorderController::saveReplay() {
    Logger::instance().info(L"Save replay requested");
    if (!ensurePipeline()) {
        Logger::instance().error(L"Replay could not be saved because the capture pipeline failed to start");
        return {};
    }
    if (replay_.videoKeyFrameCount() == 0) {
        Logger::instance().info(L"Replay save requested before a buffered keyframe; requesting an IDR frame");
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
    }
    const std::wstring gameName = foregroundApplicationName();
    const auto output = nextClipPath(gameName.empty() ? L"replay" : gameName.c_str());
    VideoSettings videoSettings;
    {
        std::scoped_lock lock(stateMutex_);
        videoSettings = activeVideoSettings_;
    }
    const bool saved = replay_.saveTo(output, videoSettings);
    if (!saved) {
        Logger::instance().warning(std::wstring(L"Replay save failed: ") + output.wstring());
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
    stats.cadenceDuplicateFrames = cadenceDuplicateFrames_.load();
    stats.catchUpDuplicateFrames = catchUpDuplicateFrames_.load();
    stats.coalescedIdleIntervals = coalescedIdleIntervals_.load();
    stats.droppedFrames = droppedFrames_.load();
    stats.gpuProtectionDrops = gpuProtectionDrops_.load();
    stats.idleFrameSkips = idleFrameSkips_.load();
    stats.systemAudioQueueDrops = systemAudioQueueDrops_.load();
    stats.microphoneAudioQueueDrops = microphoneAudioQueueDrops_.load();
    stats.replayVideoPackets = replay_.videoPacketCount();
    stats.replayKeyFrames = replay_.videoKeyFrameCount();
    stats.captureWidth = captureWidth_.load();
    stats.captureHeight = captureHeight_.load();
    stats.encodeWidth = encodeWidth_.load();
    stats.encodeHeight = encodeHeight_.load();
    {
        std::scoped_lock gpuLock(pipelineGpuMutex_);
        if (encoder_) {
            stats.encoder = encoder_->stats();
        }
    }
    stats.encoder.queueDepth = static_cast<uint32_t>(frameQueue_.size());
    return stats;
}

EncoderCapabilities RecorderController::encoderCapabilities() const {
    std::scoped_lock gpuLock(pipelineGpuMutex_);
    if (encoder_) {
        return encoder_->capabilities();
    }
    return {};
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
    Logger::instance().info(std::wstring(L"Starting capture pipeline: clips=") + snapshot.clipDirectory.wstring() +
                            L", captureBackend=" +
                            (snapshot.preferredCaptureBackend == CaptureBackend::WindowsGraphicsCapture ? L"WGC" : L"Desktop Duplication") +
                            L", followFocusedMonitor=" + (snapshot.followFocusedMonitor ? L"yes" : L"no") +
                            L", followMouseMonitor=" + (snapshot.followMouseMonitor ? L"yes" : L"no") +
                            L", systemAudio=" + (snapshot.captureSystemAudio ? L"yes" : L"no") +
                            L", microphone=" + (snapshot.captureMicrophone ? L"yes" : L"no"));
    std::error_code directoryError;
    std::filesystem::create_directories(snapshot.clipDirectory, directoryError);
    if (directoryError) {
        Logger::instance().error(std::wstring(L"Could not create clip directory: ") + snapshot.clipDirectory.wstring() +
                                 L" (" + utf8ToWide(directoryError.message()) + L")");
        std::scoped_lock lock(captureStatusMutex_);
        captureBackendActive_ = false;
        captureBackendStatus_ = L"Capture pipeline failed before capture backend start: clip folder could not be created";
        return false;
    }

    frameQueue_.resetCapacity(frameQueueCapacityFor(snapshot.gpu));

    const CaptureTarget target = captureTargetForSettings(snapshot);
    if (!recreateGpuPipeline(target, L"pipeline start")) {
        Logger::instance().error(L"Capture pipeline start failed during GPU/capture initialization");
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
    cadenceDuplicateFrames_ = 0;
    catchUpDuplicateFrames_ = 0;
    coalescedIdleIntervals_ = 0;
    droppedFrames_ = 0;
    gpuProtectionDrops_ = 0;
    idleFrameSkips_ = 0;
    systemAudioQueueDrops_ = 0;
    microphoneAudioQueueDrops_ = 0;
    lastEncodedVideoPts100ns_ = 0;
    videoTimelineEndPts100ns_ = 0;

    if (snapshot.captureSystemAudio) {
        refreshSystemAudioCapture(snapshot);
    }
    if (snapshot.captureMicrophone) {
        if (!microphoneAudio_.start(AudioTrack::Microphone, snapshot.audioInputDeviceId, [this](AudioPacket&& packet) { handleAudioPacket(std::move(packet)); })) {
            Logger::instance().warning(L"Microphone capture did not start");
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
    Logger::instance().info(std::wstring(L"Capture/encode pipeline started: capture=") + std::to_wstring(sourceWidth) + L"x" + std::to_wstring(sourceHeight) +
                            L", encode=" + std::to_wstring(videoSettings.width) + L"x" + std::to_wstring(videoSettings.height));
    return true;
}

void RecorderController::stopPipeline() {
    if (!pipelineRunning_) {
        return;
    }

    stopRequested_ = true;
    systemAudio_.stop();
    microphoneAudio_.stop();
    if (frameEvent_) {
        SetEvent(frameEvent_);
    }
    if (captureThread_.joinable()) {
        captureThread_.join();
    }
    if (encodeThread_.joinable()) {
        encodeThread_.join();
    }

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
    frameQueue_.clear();
    systemAudioQueue_.clear();
    microphoneAudioQueue_.clear();

    const RecordingStats finalStats = stats();
    Logger::instance().info(
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
    if (capture_) {
        capture_->shutdown();
        capture_.reset();
    }
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
    Logger::instance().info(L"Capture/encode pipeline stopped");
}

bool RecorderController::recreateGpuPipeline(const CaptureTarget& target, const wchar_t* reason) {
    Logger::instance().info(std::wstring(L"Recreating GPU pipeline: ") + (reason ? reason : L"unspecified"));

    const uint64_t discardGeneration =
        discardRequestGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
    discardQueuedFrames_.store(true, std::memory_order_release);
    if (frameEvent_) {
        SetEvent(frameEvent_);
    }

    // Only encodeLoop consumes frameQueue_. Drain old-device frames before D3D teardown.
    if (encodeThread_.joinable()) {
        std::unique_lock discardLock(frameDiscardMutex_);
        frameDiscardComplete_.wait(discardLock, [this, discardGeneration] {
            return discardCompletedGeneration_.load(std::memory_order_acquire) >= discardGeneration;
        });
    }

    std::scoped_lock gpuLock(pipelineGpuMutex_);

    if (encoder_) {
        encoder_->shutdown();
        encoder_.reset();
    }
    scaler_.reset();
    if (capture_) {
        capture_->shutdown();
        capture_.reset();
    }

    if (!d3d_.initialize(d3d_.adapterIndex())) {
        Logger::instance().error(L"GPU pipeline recreate failed during D3D initialization");
        std::scoped_lock lock(captureStatusMutex_);
        captureBackendActive_ = false;
        captureBackendStatus_ = L"Capture pipeline failed: Direct3D initialization failed";
        return false;
    }

    if (!createCaptureSource(target)) {
        Logger::instance().error(L"GPU pipeline recreate failed because no capture source could initialize");
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
        Logger::instance().error(L"GPU pipeline recreate failed during encoder initialization");
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
    Logger::instance().info(
        L"GPU pipeline ready: capture=" + std::to_wstring(sourceWidth) + L"x" + std::to_wstring(sourceHeight) +
        L", encode=" + std::to_wstring(videoSettings.width) + L"x" + std::to_wstring(videoSettings.height) +
        L", adapter=" + d3d_.adapterName());
    return true;
}

bool RecorderController::createCaptureSource(const CaptureTarget& target) {
    AppSettings snapshot = settings();
    const CaptureBackend selectedBackend = selectedBackendForSettings(snapshot);
    bool wgcAttempted = false;
    bool wgcFailed = false;

    {
        std::scoped_lock lock(captureStatusMutex_);
        selectedCaptureBackend_ = selectedBackend;
        activeCaptureBackend_ = selectedBackend;
        captureBackendActive_ = false;
        captureBackendFallbackUsed_ = false;
        captureBackendStatus_ = std::wstring(L"Selected backend: ") + captureBackendDisplayName(selectedBackend) + L"; initializing";
    }

    auto tryInitialize = [&](std::unique_ptr<ICaptureSource> source, const wchar_t* name) -> std::unique_ptr<ICaptureSource> {
        Logger::instance().debug(std::wstring(L"Initializing capture source: ") + name);
        if (source->initialize(d3d_, snapshot, target)) {
            Logger::instance().debug(std::wstring(L"Capture source initialized: ") + name);
            return std::move(source);
        }
        Logger::instance().warning(std::wstring(L"Capture source initialization failed: ") + name);
        source->shutdown();
        return {};
    };

    std::unique_ptr<ICaptureSource> initialized;
    if (snapshot.followFocusedMonitor || snapshot.preferredCaptureBackend == CaptureBackend::WindowsGraphicsCapture) {
        wgcAttempted = true;
        initialized = tryInitialize(std::make_unique<WgcCaptureSource>(), L"Windows Graphics Capture");
        wgcFailed = !initialized;
    }

    // Always allow Desktop Duplication fallback, including follow-monitor mode
    // (monitor is resolved via HMONITOR → DXGI output mapping).
    if (!initialized) {
        initialized = tryInitialize(std::make_unique<DesktopDuplicationCapture>(), L"Desktop Duplication");
    }

    if (!initialized) {
        std::scoped_lock lock(captureStatusMutex_);
        captureBackendActive_ = false;
        captureBackendFallbackUsed_ = false;
        captureBackendStatus_ = wgcAttempted && wgcFailed
            ? L"Selected backend: Windows Graphics Capture; WGC failed and Desktop Duplication fallback also failed"
            : std::wstring(L"Selected backend: ") + captureBackendDisplayName(selectedBackend) + L"; initialization failed";
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
            captureBackendStatus_ += L" (fallback after WGC failed)";
        }
    }

    if (capture_) {
        capture_->shutdown();
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
    auto nextMonitorPoll = std::chrono::steady_clock::now();
    auto nextSoundSeparationPoll = std::chrono::steady_clock::now();
    auto pendingMonitorSince = std::chrono::steady_clock::now();
    auto lastVideoSubmission = SteadyClock::time_point::min();
    HMONITOR pendingMonitor = nullptr;
    bool emitClockStarted = false;
    bool requestKeyFrameAfterAcceptedFrame = false;
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
    auto idleCoalescingAllowed = [&]() {
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
        const bool periodicHeartbeat =
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

    auto switchCaptureTarget = [&](const CaptureTarget& target, const wchar_t* reason) -> bool {
        {
            std::scoped_lock gpuLock(pipelineGpuMutex_);
            if (!createCaptureSource(target) || !capture_) {
                return false;
            }
            if (encoder_) {
                encoder_->resetInputResources();
            }
            scaler_.reset();
            scaler_.setPoolSize(captureTexturePoolSize(gpuSettings));
        }
        discardQueuedFrames_.store(true, std::memory_order_release);
        durationUpdatePending_.store(false, std::memory_order_release);
        {
            std::scoped_lock lock(durationUpdateMutex_);
            pendingDurationPts100ns_ = 0;
            pendingDuration100ns_ = 0;
        }
        lastFrame = {};
        emitClockStarted = false;
        timelineScheduler.reset();
        lastVideoSubmission = SteadyClock::time_point::min();
        requestKeyFrameAfterAcceptedFrame = true;
        if (frameEvent_) {
            SetEvent(frameEvent_);
        }
        refreshCaptureDimensions();
        Logger::instance().info(std::wstring(L"Capture source switched: ") + reason);
        return true;
    };

    while (!stopRequested_) {
        const auto loopNow = std::chrono::steady_clock::now();
        if (captureSettings.captureSystemAudio &&
            loopNow >= nextSoundSeparationPoll) {
            nextSoundSeparationPoll = loopNow + soundSeparationPollInterval;
            refreshSystemAudioCapture(captureSettings);
        }

        if (captureSettings.followFocusedMonitor && loopNow >= nextMonitorPoll) {
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
                }
            }
        }

        if (!capture_) {
            CaptureTarget target = captureSettings.followFocusedMonitor
                ? captureTargetForSettings(captureSettings)
                : activeCaptureTarget_;
            if (!switchCaptureTarget(target, L"capture source recreated")) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
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

        const bool acquired = capture_ && capture_->acquireNextFrame(frame, timeoutMs);
        if (acquired) {
            ++sourceFrames_;
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
                scaled = scaler_.scale(
                    d3d_,
                    frame,
                    videoSettings.width,
                    videoSettings.height,
                    encodeFrame,
                    captureSettings.followFocusedMonitor,
                    captureSettings.followFocusedMonitor && gpuSettings.stableMultimonitorFrames,
                    encoderInputFormat);
            }
            if (!scaled) {
                ++droppedFrames_;
                continue;
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
            }
        }

        bool captureLost = false;
        bool deviceRemoved = false;
        bool encoderFaulted = false;
        {
            std::scoped_lock gpuLock(pipelineGpuMutex_);
            captureLost = capture_ && capture_->isDeviceLost();
            deviceRemoved = d3d_.isDeviceRemoved();
            encoderFaulted = encoder_ && !encoder_->stats().encoderAvailable;
        }
        if (captureLost || deviceRemoved || encoderFaulted) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            CaptureTarget target = captureSettings.followFocusedMonitor
                ? captureTargetForSettings(captureSettings)
                : activeCaptureTarget_;
            if (deviceRemoved || encoderFaulted) {
                Logger::instance().warning(
                    deviceRemoved
                        ? L"D3D device removed; recreating full GPU pipeline"
                        : L"Hardware encoder faulted; recreating full GPU pipeline");
                if (recreateGpuPipeline(
                        target, deviceRemoved ? L"device removed" : L"encoder fault")) {
                    // Full recreate waits for encode-loop discard before refreshing both settings.
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
                }
            } else {
                Logger::instance().warning(L"Capture access lost; recreating capture source");
                if (switchCaptureTarget(target, L"device/access loss")) {
                    // capture-only path
                }
            }
        }

        if (lastFrame.texture && emitClockStarted) {
            const auto now = std::chrono::steady_clock::now();
            if (const auto advance = advanceVideoCadence(
                    now,
                    nextEmit,
                    frameInterval,
                    stopRequested_.load(std::memory_order_acquire))) {
                nextEmit = advance->nextEmit;
                emitFrame(lastFrame, advance->emitTime, advance->intervalCount);
            }
            if (now - nextEmit > std::chrono::seconds(1)) {
                nextEmit = now;
            }
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
            Logger::instance().warning(L"Discarded unmatched pending video durations after reaching limit");
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
            Logger::instance().warning(
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
        Logger::instance().info(
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
        Logger::instance().warning(L"System audio capture did not start");
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
