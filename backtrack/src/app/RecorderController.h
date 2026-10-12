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

class MicrophoneAgc;
#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
class RnnoiseDenoiser;
#endif

// Capture-session supervisor: the 5-state exclusive-fullscreen recovery
// machine plus the flags that used to live as ~30 captureLoopImpl locals.
// All recovery state lives here; frame data and clocks stay in the loop.
// Transitions are explicit methods so call sites read as state changes, not
// flag soup. Dropped twins (not stored): the derived
// desktopRecoveryProbeActive predicate (computed from recoveryProbe +
// cover), and the ad-hoc `stallHold || state != Capturing` checks, which are
// now stalled(). exclusiveBlind vs loggedHold both stay: blindness is
// recovery input, loggedHold is first-hold output (IDR + log once).
enum class CaptureSessionState {
    Capturing,
    AccessLost,
    WaitStable,
    Rebind,
    Holding,
};

struct CaptureSession {
    CaptureSessionState state = CaptureSessionState::Capturing;
    bool stallHold = false;
    bool exclusiveBlind = false;
    bool recoveryProbe = false;
    bool targetLostLatched = false;
    bool retargetedAlternate = false;
    bool immediateRebind = false;
    // Known source switches may accept the first fresh source frame; the longer
    // sustained-frame gate remains for ambiguous capture-loss recovery.
    bool acceptFirstLive = false;
    bool loggedHold = false;
    int rebindAttempts = 0;
    int exclusiveDxgiAttempts = 0;
    int sustainedLive = 0;
    int softStallRecreates = 0;
    int deadTargetFailures = 0;
    SteadyClock::time_point accessLostAt = SteadyClock::time_point::min();
    SteadyClock::time_point liveSince = SteadyClock::time_point::min();
    SteadyClock::time_point rebindBornAt = SteadyClock::time_point::min();
    SteadyClock::time_point lastRebindAt = SteadyClock::time_point::min();
    SteadyClock::time_point softStallBeganAt = SteadyClock::time_point::min();
    SteadyClock::time_point lastSoftStallAt = SteadyClock::time_point::min();
    SteadyClock::time_point nextTargetCheckAt = SteadyClock::time_point::min();
    HWND exclusiveWindow = nullptr;

    bool stalled() const { return stallHold || state != CaptureSessionState::Capturing; }

    void toAccessLost(SteadyClock::time_point now, HWND coverWindow) {
        if (state != CaptureSessionState::Capturing) {
            stallHold = true;
            return;
        }
        accessLostAt = now;
        exclusiveWindow = coverWindow;
        state = CaptureSessionState::AccessLost;
        stallHold = true;
        liveSince = SteadyClock::time_point::min();
        sustainedLive = 0;
        softStallBeganAt = SteadyClock::time_point::min();
        softStallRecreates = 0;
    }

    // Returns true on the first hold of this cover episode (caller emits the
    // one IDR + log + status line).
    bool toHolding() {
        const bool firstHold = !loggedHold;
        state = CaptureSessionState::Holding;
        stallHold = true;
        exclusiveBlind = true;
        sustainedLive = 0;
        liveSince = SteadyClock::time_point::min();
        softStallBeganAt = SteadyClock::time_point::min();
        softStallRecreates = 0;
        if (firstHold) {
            loggedHold = true;
        }
        return firstHold;
    }

    void toCapturing() {
        state = CaptureSessionState::Capturing;
        stallHold = false;
        exclusiveBlind = false;
        recoveryProbe = false;
        loggedHold = false;
        acceptFirstLive = false;
        rebindAttempts = 0;
        exclusiveDxgiAttempts = 0;
        softStallBeganAt = SteadyClock::time_point::min();
        softStallRecreates = 0;
    }
};

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
    bool isRecording() const noexcept { return recording_.load(std::memory_order_acquire); }
    EncoderCapabilities encoderCapabilities() const;
    AppSettings settings() const;
    std::wstring captureBackendStatus() const;
    std::wstring lastRecordingError() const;
    // Non-empty when disk-backed replay buffering could not be used and replay
    // video is in RAM instead. The setting stays checked, so this is the only
    // place the user can learn the buffer silently changed backend.
    std::wstring replayStorageFallback() const;

private:
    bool ensurePipeline();
    void stopPipeline();
    // Runs RecoveryStore::sweepOrphanedRecoveryData on a worker thread so the
    // main window appears immediately even when a crashed session left a large
    // recording to remux. It also clears a leftover replay ring folder, which
    // the recovery sweep does not cover. startRecording/saveReplay/
    // recoverFailedRecording/shutdown wait for it first: the sweep reads and
    // deletes recovery data, so it must never overlap code that stages new
    // recovery state or muxes from it.
    void beginStartupSweep();
    void waitForStartupSweep();
    bool recreateGpuPipeline(const CaptureTarget& target, const wchar_t* reason, uint32_t adapterIndex);
    // exclusiveRecovery: DXGI monitor capture only, skipping the WGC window
    // path while a validated foreground window remains in exclusive fullscreen.
    bool createCaptureSource(
        const CaptureTarget& target,
        bool exclusiveRecovery = false);
    uint32_t pipelineAdapterForTarget(const CaptureTarget& target) const;
    uint32_t activeFrameQueueLimit(const GpuOptimizationSettings& gpuSettings) const;
    bool shouldDropForGpuProtection(bool duplicateFrame, const GpuOptimizationSettings& gpuSettings) const;
    // Thread procs. Each wraps its *Impl body in a catch-all so a stray
    // exception (bad_alloc, filesystem errors) degrades that worker instead of
    // terminating the process mid-save/mid-recording.
    void captureLoop();
    void captureLoopImpl();
    void encodeLoop();
    void encodeLoopImpl();
    void audioLoop();
    void audioLoopImpl();
    void handleAudioPacket(AudioPacket&& packet);
    void writeAudioPacket(AudioPacket&& packet);
    void refreshSystemAudioCapture(const AppSettings& settings);
    // Publishes the lock-free mirrors for hot UI-timer reads (replay enable,
    // GOP seconds, cursor preference). Called wherever settings_ is assigned
    // so the mirrors never drift from the sanitized source of truth.
    void publishLiveMirrors(const AppSettings& settings);
    // Diagnostics-only watchdog: warns (once per stall episode) when the
    // capture/encode pipeline stops producing encoded video while it should be
    // running and replay is enabled. Reads lastEncodedVideoPts100ns_ against
    // the steady clock; no recovery logic. Safe to call from stats().
    void detectVideoPipelineStall() const;
    std::filesystem::path nextClipPath(const wchar_t* prefix, bool organizeIntoSubfolder) const;
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
    // Published under pipelineGpuMutex_, read through atomic<shared_ptr>
    // without it: the UI thread and the controller worker thread both call
    // stats()/encoderCapabilities(), so the contended read must never touch a
    // member another caller can be assigning. EncoderCapabilities owns three
    // std::wstrings, so an unlocked member copy tears a buffer rather than
    // merely going stale. Seeded with an empty snapshot so the first read,
    // before any publish, is never null.
    mutable std::atomic<std::shared_ptr<const EncoderCapabilities>> lastEncoderCapabilities_{
        std::make_shared<const EncoderCapabilities>()};
    mutable std::atomic<std::shared_ptr<const EncoderStats>> lastEncoderStats_{
        std::make_shared<const EncoderStats>()};
    Mp4Muxer muxer_;
    ReplayBuffer replay_;
    WasapiCapture systemAudio_;
    WasapiCapture microphoneAudio_;

    mutable std::mutex systemAudioCaptureMutex_;
    std::vector<std::wstring> systemAudioMutedExecutableKeys_;
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
    std::thread startupSweepThread_;
    // Serializes spawn/join of startupSweepThread_: concurrent waiters must
    // not call join() on the same thread object.
    std::mutex startupSweepMutex_;
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
    // Mirrors the active encoder's caps.effective.zeroReorderDelay (and its
    // mere existence): written once per pipeline build/teardown, read per
    // emitted frame by idleCoalescingAllowed without touching pipelineGpuMutex_
    // or copying EncoderCapabilities.
    std::atomic<bool> encoderZeroReorderDelay_{false};
    std::atomic<uint32_t> audioOutputVolumePercent_{100};
    std::atomic<uint32_t> audioInputVolumePercent_{100};
    std::atomic<bool> audioInputAutoGain_{false};
    // Lock-free mirrors for hot UI-timer/diagnostics reads. settings() copies
    // the whole struct (paths, vectors) per call; these load without a lock
    // or allocation. Written only where settings_ is assigned, always from
    // the sanitized value.
    std::atomic<bool> replayEnabledMirror_{true};
    std::atomic<uint32_t> videoGopSecondsMirror_{2};
    std::atomic<bool> captureCursorMirror_{true};
    // Set when the microphone stream restarts or AGC is toggled; the audio
    // writer resets the AGC envelope on its own thread before the next packet.
    std::atomic<bool> microphoneAgcResetPending_{true};
    std::unique_ptr<MicrophoneAgc> microphoneAgc_;
#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
    // Noise suppression mirrors the AGC pattern: an enable flag snapshot plus
    // a reset flag consumed on the audio writer thread. The scratch buffers
    // avoid per-packet allocations for the stereo fold-down.
    std::atomic<bool> audioInputNoiseSuppression_{false};
    std::atomic<bool> microphoneDenoiserResetPending_{true};
    std::unique_ptr<RnnoiseDenoiser> microphoneDenoiser_;
    std::vector<float> microphoneDenoiseScratch_;
    std::vector<float> microphoneDenoiseOutputScratch_;
#endif
    std::atomic<uint64_t> capturedFrames_{0};
    std::atomic<uint64_t> sourceFrames_{0};
    std::atomic<uint64_t> sourceFramesPerSecond_{0};
    std::atomic<uint64_t> timelineIntervalsPerSecond_{0};
    std::atomic<uint64_t> cadenceDuplicateFrames_{0};
    std::atomic<uint64_t> catchUpDuplicateFrames_{0};
    std::atomic<uint64_t> coalescedIdleIntervals_{0};
    std::atomic<uint64_t> droppedFrames_{0};
    std::atomic<uint64_t> gpuProtectionDrops_{0};
    std::atomic<uint64_t> frameQueueDrops_{0};
    std::atomic<uint64_t> idleFrameSkips_{0};
    std::atomic<uint64_t> cursorOnlyFrames_{0};
    std::atomic<uint64_t> systemAudioQueueDrops_{0};
    std::atomic<uint64_t> microphoneAudioQueueDrops_{0};
    // Steady-clock arrival time (100ns) of the most recent packet per audio
    // input. Seeded at pipeline start so silence ages count from startup;
    // updated on the WASAPI capture threads. Drives the audio-liveness
    // telemetry in RecordingStats.
    std::atomic<int64_t> systemAudioLastPacketAt100ns_{0};
    std::atomic<int64_t> microphoneAudioLastPacketAt100ns_{0};
    std::atomic<int64_t> lastEncodedVideoPts100ns_{0};
    std::atomic<int64_t> videoTimelineEndPts100ns_{0};
    // Pipeline-stall watchdog (diagnostics only). Tracks the newest encoded
    // video PTS so stats() can warn when a capture/encode pipeline that should
    // be running stops advancing (frozen GPU/NVENC). mutable/inline so the
    // const stats() path can observe and update it.
    mutable std::atomic<int64_t> videoStallSeenPts100ns_{0};
    mutable std::atomic<bool> videoStallWarningActive_{false};
    std::atomic<uint32_t> captureWidth_{0};
    std::atomic<uint32_t> captureHeight_{0};
    std::atomic<uint32_t> encodeWidth_{0};
    std::atomic<uint32_t> encodeHeight_{0};
    // HDR telemetry mirrors (see RecordingStats::hdrActive/hdrRejected).
    // hdrMonitorActive_ is written on pipeline (re)build; hdrActive_ follows
    // the last acquired frame. Both read lock-free from stats().
    std::atomic<bool> hdrActive_{false};
    std::atomic<bool> hdrMonitorActive_{false};
};

} // namespace backtrack
