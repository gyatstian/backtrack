#pragma once

#include "audio/WasapiCapture.h"
#include "capture/D3D11FrameScaler.h"
#include "capture/D3DDevice.h"
#include "capture/ICaptureSource.h"
#include "core/SpscQueue.h"
#include "core/Types.h"
#include "encoder/EncoderFactory.h"
#include "encoder/IEncoder.h"
#include "mux/Mp4Muxer.h"
#include "replay/ReplayBuffer.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace backtrack {

class RecorderController {
public:
    RecorderController();
    ~RecorderController();

    bool initialize(AppSettings settings);
    bool updateSettings(AppSettings settings);
    bool startRecording();
    std::filesystem::path stopRecording();
    std::filesystem::path recoverFailedRecording();
    std::filesystem::path saveReplay();
    void shutdown();

    RecordingStats stats() const;
    EncoderCapabilities encoderCapabilities() const;
    AppSettings settings() const;
    std::wstring captureBackendStatus() const;
    std::wstring lastRecordingError() const;

private:
    bool ensurePipeline();
    void stopPipeline();
    bool createCaptureSource(const CaptureTarget& target);
    uint32_t activeFrameQueueLimit() const;
    bool shouldDropForGpuProtection(bool duplicateFrame) const;
    void captureLoop();
    void encodeLoop();
    void handleAudioPacket(AudioPacket&& packet);
    void writeAudioPacket(AudioPacket&& packet);
    void refreshSystemAudioCapture(const AppSettings& settings);
    std::filesystem::path nextClipPath(const wchar_t* prefix) const;
    void setLastRecordingError(std::wstring error);

    mutable std::mutex stateMutex_;
    AppSettings settings_;
    VideoSettings activeVideoSettings_;
    GpuOptimizationSettings activeGpuSettings_;
    CaptureTarget activeCaptureTarget_;

    D3DDevice d3d_;
    std::unique_ptr<ICaptureSource> capture_;
    mutable std::mutex captureStatusMutex_;
    CaptureBackend selectedCaptureBackend_ = CaptureBackend::WindowsGraphicsCapture;
    CaptureBackend activeCaptureBackend_ = CaptureBackend::WindowsGraphicsCapture;
    bool captureBackendActive_ = false;
    bool captureBackendFallbackUsed_ = false;
    std::wstring captureBackendStatus_ = L"Capture backend has not started";
    D3D11FrameScaler scaler_;
    std::unique_ptr<IEncoder> encoder_;
    Mp4Muxer muxer_;
    ReplayBuffer replay_;
    WasapiCapture systemAudio_;
    WasapiCapture microphoneAudio_;

    mutable std::mutex systemAudioCaptureMutex_;
    uint32_t systemAudioExcludedProcessId_ = UINT32_MAX;
    // PID whose loopback exclusion was attempted and failed; suppresses retrying
    // (and re-logging) the same doomed exclusion every poll cycle.
    uint32_t failedSoundSeparationProcessId_ = 0;
    mutable std::mutex recordingErrorMutex_;
    std::wstring lastRecordingError_;

    SpscQueue<GpuFrame> frameQueue_;
    mutable std::mutex durationUpdateMutex_;
    int64_t pendingDurationPts100ns_ = 0;
    int64_t pendingDuration100ns_ = 0;
    std::thread captureThread_;
    std::thread encodeThread_;
    HANDLE frameEvent_ = nullptr;

    std::atomic<bool> pipelineRunning_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> recording_{false};
    std::atomic<bool> waitingForRecordingKeyFrame_{false};
    std::atomic<bool> discardQueuedFrames_{false};
    std::atomic<bool> durationUpdatePending_{false};
    std::atomic<bool> forceVideoHeartbeat_{false};
    std::atomic<uint64_t> capturedFrames_{0};
    std::atomic<uint64_t> sourceFrames_{0};
    std::atomic<uint64_t> cadenceDuplicateFrames_{0};
    std::atomic<uint64_t> catchUpDuplicateFrames_{0};
    std::atomic<uint64_t> coalescedIdleIntervals_{0};
    std::atomic<uint64_t> droppedFrames_{0};
    std::atomic<uint64_t> gpuProtectionDrops_{0};
    std::atomic<uint64_t> idleFrameSkips_{0};
    std::atomic<int64_t> lastEncodedVideoPts100ns_{0};
    std::atomic<int64_t> videoTimelineEndPts100ns_{0};
    std::atomic<uint32_t> captureWidth_{0};
    std::atomic<uint32_t> captureHeight_{0};
    std::atomic<uint32_t> encodeWidth_{0};
    std::atomic<uint32_t> encodeHeight_{0};
};

} // namespace backtrack
