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
    Native,
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
    ResolutionMode resolutionMode = ResolutionMode::Native;
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
};

struct ReplaySettings {
    uint32_t seconds = 120;
    bool enabled = true;
};

struct GpuOptimizationSettings {
    GpuAdaptiveMode adaptiveMode = GpuAdaptiveMode::Conservative;
    bool wgcZeroCopy = true;
    uint32_t frameQueueLimit = 4;
    bool allowIdleFrameSkipping = true;
    bool stableMultimonitorFrames = true;
};

struct HotkeySettings {
    uint32_t startStopModifiers = MOD_CONTROL | MOD_ALT;
    uint32_t startStopVirtualKey = VK_F9;
    uint32_t saveReplayModifiers = MOD_CONTROL | MOD_ALT;
    uint32_t saveReplayVirtualKey = VK_F10;
};

struct GameIntegrationSettings {
    bool leagueOfLegendsKillReminder = false;
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
    bool soundSeparationEnabled = false;
    struct SoundSeparationApp {
        std::wstring name;
        std::filesystem::path executablePath;
        bool muted = true;
    };
    std::vector<SoundSeparationApp> soundSeparationApps;
    bool startWithWindowsMinimized = false;
    bool pruneStaleMicrophoneConsentEntries = false;
    bool exitToTray = false;
    uint32_t notificationSoundVolumePercent = 100;
    bool libraryGalleryView = false;
    uint32_t monitorIndex = 0;
    bool followFocusedMonitor = false;
    bool followMouseMonitor = false;
    bool captureCursor = true;
    LogLevel logLevel = LogLevel::Debug;
};

struct CaptureTarget {
    HMONITOR monitor = nullptr;
    uint32_t monitorIndex = 0;
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
};

struct EncodedPacket {
    std::vector<uint8_t> bytes;
    int64_t pts100ns = 0;
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
    uint64_t cadenceDuplicateFrames = 0;
    uint64_t catchUpDuplicateFrames = 0;
    uint64_t coalescedIdleIntervals = 0;
    uint64_t droppedFrames = 0;
    uint64_t gpuProtectionDrops = 0;
    uint64_t idleFrameSkips = 0;
    uint64_t systemAudioQueueDrops = 0;
    uint64_t microphoneAudioQueueDrops = 0;
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
