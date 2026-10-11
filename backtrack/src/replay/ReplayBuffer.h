#pragma once

#include "core/Types.h"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

#include <Windows.h>
namespace backtrack {

class ReplayBuffer {
public:
    ReplayBuffer() = default;
    ~ReplayBuffer();
    ReplayBuffer(const ReplayBuffer&) = delete;
    ReplayBuffer& operator=(const ReplayBuffer&) = delete;

    // storageDirectory/capacityBytes only apply when settings.diskBackedVideo
    // is set: they select where the ring file lives and how large it may grow.
    // With the flag off both are ignored and buffering stays fully in RAM.
    void configure(const ReplaySettings& settings,
                   const std::filesystem::path& storageDirectory = {},
                   uint64_t capacityBytes = 0);
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

private:
    // Metadata view of one buffered video packet; identical for both backends
    // so save-path selection logic never diverges between them.
    struct VideoPacketMeta {
        int64_t pts100ns = 0;
        int64_t dts100ns = 0;
        int64_t duration100ns = 0;
        bool hasDts = false;
        bool keyFrame = false;
    };

    // One contiguous or wrap-split extent of a packet's bytes inside the ring
    // file. secondByteCount == 0 means the payload did not wrap.
    struct RingExtent {
        uint64_t offset = 0;
        uint32_t byteCount = 0;
    };
    struct DiskVideoPacket {
        VideoPacketMeta meta;
        RingExtent first;
        RingExtent second;
    };

    enum class VideoBackend {
        Ram,
        Disk,
    };

    void trimLocked();
    void clearBufferedVideoLocked();
    bool openRingLocked();
    // Defers the physical close while a save holds the pin (the save may
    // still be reading from the handle); executes immediately otherwise.
    void closeRingLocked() noexcept;
    void closeRingNowLocked() noexcept;
    // Caller must have secured space via ensureRingSpaceLocked; appends the
    // packet payload to the ring (wrap split handled here) and records its
    // extents. False on any write failure - the caller must then stop
    // trusting the ring contents.
    bool appendRingLocked(const EncodedPacket& packet);
    // Waits (while a save pins refs) or evicts oldest packets until size
    // bytes of ring space are free. False when the request can never fit.
    bool ensureRingSpaceLocked(std::unique_lock<std::mutex>& lock, uint32_t size);
    // Explicit-offset overlapped read; safe to call with or without mutex_
    // held because no per-handle file position exists.
    bool readRingLocked(uint64_t offset, uint8_t* destination, uint32_t size) const;
    void freeRingSpaceLocked(uint64_t bytesNeeded);
    // Ring writes failed (typically disk full): discard the whole buffered
    // video window and fall back to memory buffering for the rest of the
    // session. Coherent clips matter more than the storage mode.
    void failOverToRamLocked();

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
    size_t videoKeyFrames_ = 0;
    int64_t lastSaveEndPts100ns_ = 0;
    // Codec parameter sets (SPS/PPS or VPS/SPS/PPS) captured from the first
    // buffered packet that carried them. Prepended to a replay cut when its
    // start packet lacks headers, so a clip never begins with an IDR that no
    // decoder can bootstrap (drivers can ignore header-insertion settings).
    std::vector<uint8_t> codecHeaderBytes_;

    // --- disk-backed video backend ---
    VideoBackend videoBackend_ = VideoBackend::Ram;
    std::filesystem::path ringDirectory_;
    std::filesystem::path ringPath_;
    HANDLE ringFile_ = nullptr;
    uint64_t ringCapacityBytes_ = 0;
    // Next write position inside [0, capacity). Payloads crossing the end of
    // the file wrap around to offset 0 and produce a two-extent packet.
    uint64_t ringWriteOffset_ = 0;
    // Total bytes of all live refs; capacity minus this is the free gap the
    // write cursor may consume before it would collide with the oldest ref.
    uint64_t ringLiveBytes_ = 0;
    std::deque<DiskVideoPacket> diskVideo_;
    size_t diskVideoKeyFrames_ = 0;
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
};

} // namespace backtrack
