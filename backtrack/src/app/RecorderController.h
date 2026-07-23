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
#include <condition_variable>
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
    bool recreateGpuPipeline(const CaptureTarget& target, const wchar_t* reason);
    bool createCaptureSource(const CaptureTarget& target);
    uint32_t activeFrameQueueLimit(const GpuOptimizationSettings& gpuSettings) const;
    bool shouldDropForGpuProtection(bool duplicateFrame, const GpuOptimizationSettings& gpuSettings) const;
    void captureLoop();
    void encodeLoop();
    void audioLoop();
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
    // Serializes GPU resource teardown/recreation. Per-operation D3D access is
    // synchronized by D3DDevice's immediate-context mutex.
    mutable std::mutex pipelineGpuMutex_;
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
    std::vector<std::wstring> systemAudioMutedExecutableKeys_;
    bool systemAudioSessionMutesActive_ = false;
    mutable std::mutex recordingErrorMutex_;
    std::wstring lastRecordingError_;

    SpscQueue<GpuFrame> frameQueue_;
    SpscQueue<AudioPacket> systemAudioQueue_;
    SpscQueue<AudioPacket> microphoneAudioQueue_;
    mutable std::mutex durationUpdateMutex_;
    std::mutex frameDiscardMutex_;
    std::condition_variable frameDiscardComplete_;
    int64_t pendingDurationPts100ns_ = 0;
    int64_t pendingDuration100ns_ = 0;
    std::thread captureThread_;
    std::thread encodeThread_;
    std::thread audioThread_;
    HANDLE frameEvent_ = nullptr;
    HANDLE audioEvent_ = nullptr;

    std::atomic<bool> pipelineRunning_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> recording_{false};
    std::atomic<bool> waitingForRecordingKeyFrame_{false};
    std::atomic<bool> discardQueuedFrames_{false};
    std::atomic<uint64_t> discardRequestGeneration_{0};
    std::atomic<uint64_t> discardCompletedGeneration_{0};
    std::atomic<bool> durationUpdatePending_{false};
    std::atomic<bool> forceVideoHeartbeat_{false};
    std::atomic<uint32_t> audioOutputVolumePercent_{100};
    std::atomic<uint32_t> audioInputVolumePercent_{100};
    std::atomic<uint64_t> capturedFrames_{0};
    std::atomic<uint64_t> sourceFrames_{0};
    std::atomic<uint64_t> cadenceDuplicateFrames_{0};
    std::atomic<uint64_t> catchUpDuplicateFrames_{0};
    std::atomic<uint64_t> coalescedIdleIntervals_{0};
    std::atomic<uint64_t> droppedFrames_{0};
    std::atomic<uint64_t> gpuProtectionDrops_{0};
    std::atomic<uint64_t> idleFrameSkips_{0};
    std::atomic<uint64_t> systemAudioQueueDrops_{0};
    std::atomic<uint64_t> microphoneAudioQueueDrops_{0};
    std::atomic<int64_t> lastEncodedVideoPts100ns_{0};
    std::atomic<int64_t> videoTimelineEndPts100ns_{0};
    std::atomic<uint32_t> captureWidth_{0};
    std::atomic<uint32_t> captureHeight_{0};
    std::atomic<uint32_t> encodeWidth_{0};
    std::atomic<uint32_t> encodeHeight_{0};
};

} // namespace backtrack
