#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace backtrack {

using Microsoft::WRL::ComPtr;
using SteadyClock = std::chrono::steady_clock;

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
};

enum class VideoCodec {
    H264,
    Hevc,
};

enum class CaptureBackend {
    WindowsGraphicsCapture,
    DesktopDuplication,
};

enum class GpuVendor {
    Unknown,
    Nvidia,
    Amd,
    Intel,
};

enum class AudioTrack {
    System,
    Microphone,
    SoundSeparation,
};

enum class ResolutionMode {
    P240,
    P480,
    P720,
    P1080,
    P2K,
    P4K,
    Custom,
};

enum class EncoderPreset {
    P1,
    P2,
    P3,
    P4,
    P5,
    P6,
    P7,
};

enum class EncoderMode {
    HighQuality,
    LowLatency,
    UltraLowLatency,
    Lossless,
    UltraHighQuality,
};

enum class EncoderProfile {
    LowestGpu,
    Balanced,
    Custom,
};

enum class EncoderMultipass {
    Disabled,
    QuarterResolution,
    FullResolution,
};

enum class GpuAdaptiveMode {
    Disabled,
    Conservative,
    Aggressive,
};

struct EncoderEffectiveSettings {
    EncoderProfile profile = EncoderProfile::LowestGpu;
    EncoderPreset preset = EncoderPreset::P1;
    EncoderMode mode = EncoderMode::UltraLowLatency;
    bool lookahead = false;
    uint32_t lookaheadDepth = 0;
    bool spatialAQ = false;
    uint32_t aqStrength = 8;
    bool temporalAQ = false;
    EncoderMultipass multipass = EncoderMultipass::Disabled;
    bool bFrames = false;
    bool adaptiveBFrames = false;
    bool adaptiveIFrames = false;
    bool zeroReorderDelay = true;
    uint32_t referenceFrames = 1;
};

struct VideoSettings {
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 60;
    uint32_t bitrateKbps = 24000;
    VideoCodec codec = VideoCodec::H264;
    uint32_t gopSeconds = 2;
    ResolutionMode resolutionMode = ResolutionMode::P1080;
    EncoderPreset encoderPreset = EncoderPreset::P4;
    EncoderMode encoderMode = EncoderMode::HighQuality;
    EncoderProfile encoderProfile = EncoderProfile::LowestGpu;
    bool encoderLookahead = false;
    uint32_t encoderLookaheadDepth = 0;
    bool encoderSpatialAQ = false;
    uint32_t encoderAQStrength = 8;
    bool encoderTemporalAQ = false;
    EncoderMultipass encoderMultipass = EncoderMultipass::Disabled;
    bool encoderBFrames = false;
    bool encoderAdaptiveBFrames = false;
    bool encoderAdaptiveIFrames = false;
    bool encoderZeroReorderDelay = true;
    uint32_t encoderReferenceFrames = 1;
    bool operator==(const VideoSettings&) const = default;
};

struct ReplaySettings {
    uint32_t seconds = 120;
    bool enabled = true;
    // Opt-in: buffer encoded video packets in a fixed-capacity on-disk ring
    // file instead of process RAM. Audio stays memory-resident. When false
    // the replay buffer behaves exactly as before.
    bool diskBackedVideo = false;
    bool operator==(const ReplaySettings&) const = default;
};

struct GpuOptimizationSettings {
    GpuAdaptiveMode adaptiveMode = GpuAdaptiveMode::Conservative;
    bool wgcZeroCopy = true;
    uint32_t frameQueueLimit = 4;
    bool allowIdleFrameSkipping = true;
    bool stableMultimonitorFrames = true;
    bool operator==(const GpuOptimizationSettings&) const = default;
};

struct HotkeySettings {
    uint32_t startStopModifiers = MOD_CONTROL | MOD_ALT;
    uint32_t startStopVirtualKey = VK_F9;
    uint32_t saveReplayModifiers = MOD_CONTROL | MOD_ALT;
    uint32_t saveReplayVirtualKey = VK_F10;
    bool operator==(const HotkeySettings&) const = default;
};

struct GameIntegrationSettings {
    enum class VoiceCommandMode {
        Disabled,
        ClipThat,
        RecordVideo,
    };

    enum class DiscordRichPresenceMode {
        Always,
        FullscreenOnly,
        Off,
    };

    bool leagueOfLegendsKillReminder = false;
    VoiceCommandMode voiceCommand = VoiceCommandMode::Disabled;
    DiscordRichPresenceMode discordRichPresence = DiscordRichPresenceMode::Off;
    bool operator==(const GameIntegrationSettings&) const = default;
};

struct AppSettings {
    VideoSettings video;
    ReplaySettings replay;
    GpuOptimizationSettings gpu;
    HotkeySettings hotkeys;
    GameIntegrationSettings gameIntegrations;
    std::filesystem::path clipDirectory;
    CaptureBackend preferredCaptureBackend = CaptureBackend::WindowsGraphicsCapture;
    bool captureMicrophone = true;
    bool captureSystemAudio = true;
    std::wstring audioInputDeviceId;
    std::wstring audioOutputDeviceId;
    uint32_t audioInputVolumePercent = 100;
    uint32_t audioOutputVolumePercent = 100;
    // Voice-oriented automatic gain control: normalizes quiet microphone streams
    // (like Discord's own AGC) instead of relying on a fixed manual gain.
    bool audioInputAutoGainEnabled = false;
    bool soundSeparationEnabled = false;
    struct SoundSeparationApp {
        std::wstring name;
        std::filesystem::path executablePath;
        bool muted = true;
        bool operator==(const SoundSeparationApp&) const = default;
    };
    std::vector<SoundSeparationApp> soundSeparationApps;
    // Auto record: starts continuous recording when one of the selected
    // executables has a visible window and stops it when the last one exits.
    bool autoRecordEnabled = false;
    struct AutoRecordApp {
        std::wstring name;
        std::filesystem::path executablePath;
        bool operator==(const AutoRecordApp&) const = default;
    };
    std::vector<AutoRecordApp> autoRecordApps;
    // Optional override for where the disk-backed replay ring file lives.
    // Empty = next to the clip library (clipDirectory). Useful to keep the
    // continuous ring writes off a game HDD, e.g. on an SSD.
    std::filesystem::path replayRingDirectory;
    bool startWithWindowsMinimized = false;
    bool pruneStaleMicrophoneConsentEntries = false;
    bool exitToTray = false;
    uint32_t notificationSoundVolumePercent = 100;
    // Empty = built-in success tone. Used when a replay saves successfully.
    std::filesystem::path customNotificationSoundPath;
    bool libraryGalleryView = false;
    uint32_t monitorIndex = 0;
    bool multiMonitorSupport = false;
    bool followFocusedMonitor = false;
    bool followMouseMonitor = false;
    bool captureCursor = true;
    LogLevel logLevel = LogLevel::Debug;
    bool operator==(const AppSettings&) const = default;
};

struct CaptureTarget {
    HMONITOR monitor = nullptr;
    uint32_t monitorIndex = 0;
    // Optional WGC CreateForWindow target (exclusive-fullscreen recovery).
    HWND window = nullptr;
};

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring name;
};

struct AudioSessionAppInfo {
    std::wstring name;
    std::filesystem::path executablePath;
    uint32_t processId = 0;
};

struct GpuFrame {
    ComPtr<ID3D11Texture2D> texture;
    std::shared_ptr<void> lease;
    uint64_t frameIndex = 0;
    int64_t pts100ns = 0;
    int64_t duration100ns = 0;
    int64_t previousFramePts100ns = 0;
    int64_t previousFrameDuration100ns = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    // True when the source surface carries PQ (ST.2084) encoded HDR content
    // (advanced color active on the output). The scaler tone-maps such frames
    // to SDR before encoding; 8-bit SDR sources leave it false.
    bool hdr = false;
};

struct EncodedPacket {
    std::vector<uint8_t> bytes;
    int64_t pts100ns = 0;
    // Decode timestamp for B-frame reordering: packets are produced in decode
    // order while pts100ns stays in presentation order, so PTS regresses
    // within each GOP. Valid only when hasDts is set; without it the decode
    // time equals the presentation time.
    int64_t dts100ns = 0;
    // Explicit validity flag for dts100ns: timestamps can legitimately be
    // zero or negative (early-GOP extrapolation), so no value works as a
    // "no decode timestamp" sentinel.
    bool hasDts = false;
    int64_t duration100ns = 0;
    bool keyFrame = false;
    VideoCodec codec = VideoCodec::H264;
};

struct WaveFormatBlob {
    std::vector<uint8_t> bytes;
};

struct AudioPacket {
    AudioTrack track = AudioTrack::System;
    uint32_t processId = 0;
    std::vector<uint8_t> bytes;
    uint32_t frameCount = 0;
    int64_t pts100ns = 0;
    std::shared_ptr<const WaveFormatBlob> format;
};

struct EncoderCapabilities {
    bool available = false;
    bool h264 = false;
    bool hevc = false;
    bool bFrames = false;
    bool lookahead = false;
    bool multipleReferenceFrames = false;
    bool tenBitHevc = false;
    uint32_t maxWidth = 0;
    uint32_t maxHeight = 0;
    std::wstring adapterName;
    std::wstring backendName;
    std::wstring detail;
    EncoderEffectiveSettings effective;
};

struct EncoderStats {
    uint64_t submittedFrames = 0;
    uint64_t encodedFrames = 0;
    uint64_t keyFrames = 0;
    uint64_t droppedFrames = 0;
    uint64_t encodedBytes = 0;
    double averageEncodeMs = 0.0;
    uint32_t queueDepth = 0;
    bool encoderAvailable = false;
};

struct RecordingStats {
    bool recording = false;
    bool replayEnabled = false;
    CaptureBackend selectedCaptureBackend = CaptureBackend::WindowsGraphicsCapture;
    CaptureBackend activeCaptureBackend = CaptureBackend::WindowsGraphicsCapture;
    bool captureBackendActive = false;
    bool captureBackendFallbackUsed = false;
    std::wstring captureBackendStatus;
    uint64_t capturedFrames = 0;
    uint64_t sourceFrames = 0;
    uint64_t sourceFramesPerSecond = 0;
    uint64_t timelineIntervalsPerSecond = 0;
    uint64_t cadenceDuplicateFrames = 0;
    uint64_t catchUpDuplicateFrames = 0;
    uint64_t coalescedIdleIntervals = 0;
    uint64_t droppedFrames = 0;
    uint64_t gpuProtectionDrops = 0;
    uint64_t idleFrameSkips = 0;
    uint64_t cursorOnlyFrames = 0;
    uint64_t systemAudioQueueDrops = 0;
    uint64_t microphoneAudioQueueDrops = 0;
    // Audio-input liveness. `active` mirrors the WASAPI capture state; the
    // silence ages are wall-time since the last packet arrived from that
    // input (sampled on the stats poll). An input that never delivered a
    // packet reports the full session age. This separates a dead or
    // permanently reconnecting input (age grows, possibly inactive) from a
    // healthy one that merely carries silence.
    bool systemAudioActive = false;
    bool microphoneAudioActive = false;
    uint64_t systemAudioSilenceMs = 0;
    uint64_t microphoneSilenceMs = 0;
    uint64_t replayVideoPackets = 0;
    uint64_t replayKeyFrames = 0;
    uint32_t captureWidth = 0;
    uint32_t captureHeight = 0;
    uint32_t encodeWidth = 0;
    uint32_t encodeHeight = 0;
    EncoderStats encoder;
};

inline constexpr int64_t kHundredNanosecondsPerSecond = 10'000'000;

inline int64_t secondsTo100ns(uint32_t seconds) {
    return static_cast<int64_t>(seconds) * kHundredNanosecondsPerSecond;
}

} // namespace backtrack
