#pragma once

#include "core/Types.h"
#include "replay/RingFile.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <Windows.h>
namespace backtrack {

class ReplayBuffer {
public:
    ReplayBuffer() = default;
    ~ReplayBuffer();
    ReplayBuffer(const ReplayBuffer&) = delete;
    ReplayBuffer& operator=(const ReplayBuffer&) = delete;

    // storageDirectory/capacityBytes only select the video ring file location
    // and size when settings.diskBackedVideo is set: the directory must be
    // non-empty and the capacity at least kMinRingCapacityBytes (8 MiB).
    // audioCapacityBytes likewise gates settings.diskBackedAudio at
    // kMinAudioRingCapacityBytes (1 MiB). Either capacity is still stored as
    // the RAM byte cap for that track when its backend is memory, so a long
    // time window can never grow RAM without bound. With the flags off
    // buffering for that track stays fully in RAM, bounded by the stored cap.
    void configure(const ReplaySettings& settings,
                   const std::filesystem::path& storageDirectory = {},
                   uint64_t capacityBytes = 0,
                   uint64_t audioCapacityBytes = 0);
    void pushVideo(EncodedPacket packet);
    bool extendLastVideoDuration(int64_t expectedPts100ns, int64_t duration100ns);
    void pushAudio(AudioPacket packet);
    bool saveTo(
        const std::filesystem::path& outputPath,
        const VideoSettings& videoSettings,
        std::wstring* errorDetail = nullptr);
    void clear();
    size_t videoPacketCount() const;
    size_t videoKeyFrameCount() const;
    // Duration (100ns) of muxable video currently buffered, from the first
    // buffered keyframe to the newest packet end. 0 when empty or keyframe-less.
    int64_t videoSpan100ns() const;
    // PTS (100ns) of the newest buffered video packet, or 0 when empty. Lets a
    // caller (saveReplay) detect a buffer whose content stopped advancing from
    // a stalled capture/encode pipeline; PTS-age trim alone cannot (heartbeats
    // keep emitting with advancing timestamps while the pipeline is idle).
    int64_t newestVideoPts100ns() const;
    // Non-empty when disk-backed buffering could not be used and replay video
    // is being buffered in RAM instead. Read on the UI thread, so it is guarded
    // by its own lock rather than mutex_: queueing behind an in-flight ring
    // write on the encode thread would make the status text itself stutter.
    std::wstring storageFallbackReason() const;
    // Startup maintenance: deletes a leftover ring folder from a killed session.
    // Nothing else reclaims it - the recovery sweep only walks the clip
    // library's .backtrack_tmp, and a ring file is pre-sized to the full
    // capacity (up to 16 GiB). Only safe when no ring is open in this process.
    static void removeOrphanedRing(const std::filesystem::path& storageDirectory);

private:
    // Ring folder and files, named once so the startup sweep and the tests
    // cannot drift from what openRingLocked actually creates.
    static constexpr const wchar_t* kRingFolderName = L".backtrack_ring";
    static constexpr const wchar_t* kRingFileName = L"replay_video.bin";
    static constexpr const wchar_t* kAudioRingFileName = L"replay_audio.bin";
    // Metadata view of one buffered video packet; identical for both backends
    // so save-path selection logic never diverges between them.
    struct VideoPacketMeta {
        int64_t pts100ns = 0;
        int64_t dts100ns = 0;
        int64_t duration100ns = 0;
        bool hasDts = false;
        bool keyFrame = false;
    };

    // One buffered video packet's bytes inside the video ring; the payload
    // wraps at most once, so two extents always suffice.
    struct DiskVideoPacket {
        VideoPacketMeta meta;
        RingExtent first;
        RingExtent second;
    };
    // Metadata view of one buffered audio packet on disk. The payload lives in
    // the audio ring file; format/frameCount rebuild an AudioPacket on the
    // save path, and processId survives so the RAM and disk backends publish
    // identical packets.
    struct DiskAudioPacket {
        AudioTrack track = AudioTrack::System;
        uint32_t processId = 0;
        int64_t pts100ns = 0;
        uint32_t frameCount = 0;
        std::shared_ptr<const WaveFormatBlob> format;
        RingExtent first;
        RingExtent second;
    };

    enum class VideoBackend {
        Ram,
        Disk,
    };
    enum class AudioBackend {
        Ram,
        Disk,
    };

    void trimLocked();
    void clearBufferedVideoLocked();
    // Refreshes the lock-free mirrors the UI-thread counters read. Called
    // everywhere the buffered packet or keyframe counts change.
    void publishCountersLocked() const;
    // Caller holds mutex_. Lock order is always mutex_ then storageFallbackMutex_.
    void setVideoFallbackReasonLocked(std::wstring reason);
    void setAudioFallbackReasonLocked(std::wstring reason);
    bool openRingLocked(uint64_t capacityBytes);
    // Caller holds mutex_ and has decided the buffered video may be dropped.
    // Opens a ring of the given capacity and, on failure, degrades to RAM with
    // a user-visible reason.
    void openRingWithFallbackLocked(uint64_t capacityBytes);
    bool openAudioRingLocked(uint64_t capacityBytes);
    // Caller holds mutex_ and has decided the buffered audio may be dropped.
    // Opens an audio ring of the given capacity and, on failure, degrades to
    // RAM with a user-visible reason. Video buffering is untouched.
    void openAudioRingWithFallbackLocked(uint64_t capacityBytes);
    // Defers the physical close while a save holds the pin (the save may
    // still be reading from the handle); executes immediately otherwise.
    void closeRingLocked() noexcept;
    void closeRingNowLocked() noexcept;
    void closeAudioRingLocked() noexcept;
    void closeAudioRingNowLocked() noexcept;
    // Caller must have secured space via the ring's ensureSpace; appends the
    // packet payload to the ring and records its extents. False on any write
    // failure - the caller must then stop trusting the ring contents.
    bool appendRingLocked(const EncodedPacket& packet);
    // Per-track space policy for the video ring: waits on the save pin or
    // evicts via freeRingSpaceLocked (keyframe realignment).
    bool ensureRingSpaceLocked(std::unique_lock<std::mutex>& lock, uint32_t size);
    void freeRingSpaceLocked(uint64_t bytesNeeded);
    void freeAudioRingSpaceLocked(uint64_t bytesNeeded);
    // Per-track space policy for the audio ring: waits on the save pin or
    // evicts via freeAudioRingSpaceLocked (oldest-first).
    bool ensureAudioRingSpaceLocked(std::unique_lock<std::mutex>& lock, uint32_t size);
    static int64_t diskAudioPacketEndPts100ns(const DiskAudioPacket& packet);
    // Caller must have secured space via the audio ring's ensureSpace; appends
    // the packet payload to the audio ring and records its extents. False on
    // any write failure - the caller must then stop trusting the ring contents.
    bool appendAudioRingLocked(const AudioPacket& packet);
    void clearBufferedAudioLocked();
    // Audio ring storage failed: discard the buffered audio window and fall
    // back to memory buffering for the rest of the session. Video buffering
    // is untouched - each backend degrades on its own.
    void failOverAudioToRamLocked(std::wstring reason);
    // Ring storage failed (typically a full disk, or a read that could not be
    // satisfied): discard the whole buffered video window and fall back to
    // memory buffering for the rest of the session. Coherent clips matter more
    // than the storage mode. `reason` is published for the Diagnostics page.
    void failOverToRamLocked(std::wstring reason);
    // Test-only fault injection. Drops the ring handle while leaving the
    // buffered metadata intact, so the next read fails exactly as it would for
    // a vanished volume or a media error. No public path can produce that, and
    // without it the save path's read-failure handling ships untested. Never
    // called by the app.
    void breakRingForTesting() noexcept;
    void breakAudioRingForTesting() noexcept;
    friend struct ReplayBufferTestAccess;

    // Reasons the ring could not be used, published for the Diagnostics page.
    // Kept per track so a video failure cannot hide an audio one and a later
    // success on one backend cannot clear the other's report. Lock order is
    // always mutex_ then storageFallbackMutex_, never the reverse: writers
    // hold mutex_, readers hold nothing.
    mutable std::mutex storageFallbackMutex_;
    std::wstring videoFallbackReason_;
    std::wstring audioFallbackReason_;

    mutable std::mutex mutex_;
    ReplaySettings settings_;
    // Packets are immutable after pushVideo except for the live-tail duration
    // extension, which mutates the newest packet in place under mutex_ (a
    // full payload copy per emitted interval would spike allocations on
    // static screens). saveTo snapshots meta under mutex_, so no reader ever
    // observes the mutation unsynchronized.
    std::deque<std::shared_ptr<EncodedPacket>> video_;
    std::deque<std::shared_ptr<const AudioPacket>> systemAudio_;
    std::deque<std::shared_ptr<const AudioPacket>> microphoneAudio_;
    // RAM byte caps, stored from configure's capacity arguments even when the
    // backend is memory (0 falls back to the controller's derivation). Live
    // byte totals for the RAM queues; disk backends account in RingFile.
    uint64_t ramVideoCapacityBytes_ = 0;
    uint64_t ramAudioCapacityBytes_ = 0;
    uint64_t ramVideoBytes_ = 0;
    uint64_t ramAudioBytes_ = 0;
    size_t videoKeyFrames_ = 0;
    int64_t lastSaveEndPts100ns_ = 0;
    // GOP most recently seen by saveTo (VideoSettings is save-scoped), so
    // videoSpan100ns can mirror the save window cut. Default matches VideoSettings.
    uint32_t lastGopSeconds_ = 2;
    // Codec parameter sets (SPS/PPS or VPS/SPS/PPS) captured from the first
    // buffered packet that carried them. Prepended to a replay cut when its
    // start packet lacks headers, so a clip never begins with an IDR that no
    // decoder can bootstrap (drivers can ignore header-insertion settings).
    std::vector<uint8_t> codecHeaderBytes_;

    // --- disk-backed video backend ---
    VideoBackend videoBackend_ = VideoBackend::Ram;
    std::filesystem::path ringDirectory_;
    // Byte store for the video ring; eviction policy is freeRingSpaceLocked
    // (realigns to the next keyframe so the buffer stays decodable).
    RingFile videoRing_;
    std::deque<DiskVideoPacket> diskVideo_;
    size_t diskVideoKeyFrames_ = 0;
    // Lock-free mirrors of the two counts RecorderController::stats reads.
    // stats() runs on the UI thread and must not queue behind pushVideo's
    // synchronous ring write, which can block on the device for as long as the
    // write takes. Slightly stale by one push is fine for a status readout.
    mutable std::atomic<size_t> mirroredVideoPackets_{0};
    mutable std::atomic<size_t> mirroredKeyFrames_{0};
    // Save-in-progress pin: while >0, pushes that need ring space wait on
    // ringSpace_ instead of byte-trimming, because trimmed refs could be ones
    // an in-flight save has not read yet. Saves are rare and short, so this
    // only ever backpressures after roughly a full window of save duration.
    // Counted rather than boolean so a future second concurrent save cannot
    // silently break the protocol; today all saves serialize on the caller
    // side and the count never exceeds 1.
    unsigned savePinsRefs_ = 0;
    std::condition_variable ringSpace_;
    // Set by closeRingLocked while the pin defers the physical close; the
    // pin release path performs it.
    bool ringPendingClose_ = false;

    // --- disk-backed audio backend (independent of the video backend) ---
    AudioBackend audioBackend_ = AudioBackend::Ram;
    // Byte store for the audio ring; eviction is plain oldest-first across
    // both tracks (freeAudioRingSpaceLocked), since audio has no keyframes.
    RingFile audioRing_;
    std::deque<DiskAudioPacket> diskSystemAudio_;
    std::deque<DiskAudioPacket> diskMicrophoneAudio_;
    bool audioRingPendingClose_ = false;
};

} // namespace backtrack
