#pragma once

#include "core/Types.h"
#include "mux/AudioTimeline.h"
#include "mux/MuxedInputs.h"
#include "mux/WavWriter.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <vector>

namespace backtrack {

class Mp4Muxer {
public:
    bool startRecording(
        const std::filesystem::path& outputPath,
        const VideoSettings& settings,
        const std::filesystem::path& clipRoot = {});
    void writeVideoPacket(const EncodedPacket& packet);
    // Recording cut: once set, a packet at or below `pts100ns` is written and a
    // packet above it latches a hold, as does everything after it. finalize()
    // writes the held prefix up to the last held packet at or below the cut and
    // drops the rest. A compressed stream may only be cut at a decode-order
    // prefix: a picture dropped from the middle would leave later B pictures
    // referencing it undecodable.
    void setCutPts100ns(int64_t pts100ns);
    bool extendLastVideoDuration(int64_t expectedPts100ns, int64_t duration100ns);
    void writeAudioPacket(const AudioPacket& packet);
    std::filesystem::path finalize();
    void abort();
    bool active() const;
    uint64_t videoPacketCount() const;
    std::wstring lastError() const;
    const std::filesystem::path& outputPath() const { return outputPath_; }

    // stagingVariant selects the preferred per-attempt staging directory.
    // win32Error: optional receiver for the raw Win32 code of
    // the failing file operation (ERROR_SHARING_VIOLATION etc.);
    // ERROR_SUCCESS when the failure was not from a Win32 call.
    static bool muxToMp4(
        const MuxedInputs& inputs,
        const std::filesystem::path& outputPath,
        std::wstring* errorDetail = nullptr,
        unsigned stagingVariant = 0,
        DWORD* win32Error = nullptr);
    static std::filesystem::path recoverLatestFailedRecording(
        const std::filesystem::path& clipDirectory,
        std::wstring& detail);

private:
    friend struct Mp4MuxerTestAccess;

    // Bounded hold: a stop's post-cut tail is at most a reorder depth of frames,
    // so the caps only ever fire on a runaway producer, and then a warning plus
    // a dropped tail beats unbounded memory during a save.
    static constexpr size_t kMaxHeldVideoPackets = 64;
    static constexpr uint64_t kMaxHeldVideoBytes = 8ull * 1024 * 1024;

    struct AudioTimelineState {
        WavWriter writer;
        int64_t nextPts100ns = 0;
        // Gap-fill accounting for the finalize summary (mux/AudioTimeline.h).
        AudioGapStats gapStats;
        // Set once when a packet arrives whose format differs from the WAV the
        // writer was opened with; such packets are dropped instead of copied
        // raw (which would append wrong-rate samples and skew the timeline).
        bool warnedFormatMismatch = false;
    };

    void writeTimelineAudioPacket(AudioTimelineState& state, const AudioPacket& packet, const wchar_t* fileName);
    // writeVideoPacket's body without the cut gate, so the finalize flush can
    // write held packets through the same manifest/sample-table path.
    // Called with mutex_ held.
    void writeVideoPacketLocked(const EncodedPacket& packet);
    void holdVideoPacketLocked(const EncodedPacket& packet);
    // Writes the held decode-order prefix that belongs to the recording (every
    // held packet up to and including the last one at or below the cut) and
    // drops the rest. Called with mutex_ held.
    void flushHeldVideoPacketsLocked();
    // Flags the recording as failed after a temporary-WAV write error so
    // finalize() reports it instead of silently publishing truncated audio.
    // Called with mutex_ held.
    void noteAudioWriteFailedLocked(const wchar_t* fileName);
    void flushPendingAudioLocked();
    // Incremental crash recovery: appends one "sample" line to the recovery
    // manifest and flushes at GOP boundaries so a killed process loses at most
    // the current partial GOP. No-op when the manifest could not be opened.
    void appendManifestSampleLocked(const MuxedInputs::VideoSample& sample, bool keyFrame);
    void appendManifestAudioTrackLocked(const char* trackKey, const wchar_t* fileName);
    void closeManifestStreamLocked();

    mutable std::mutex mutex_;
    bool active_ = false;
    VideoSettings settings_;
    std::filesystem::path outputPath_;
    std::filesystem::path outputSubdir_;
    std::filesystem::path tempDirectory_;
    std::filesystem::path videoPath_;
    std::ofstream videoStream_;
    std::ofstream manifestStream_;
    uint64_t unflushedManifestSamples_ = 0;
    bool manifestWriteFailed_ = false;
    std::vector<MuxedInputs::VideoSample> videoSamples_;
    AudioTimelineState systemAudio_;
    AudioTimelineState microphoneAudio_;
    std::vector<AudioPacket> pendingAudio_;
    std::optional<int64_t> firstVideoPts100ns_;
    int64_t lastWrittenVideoPts100ns_ = 0;
    bool warnedRegressingPts_ = false;
    std::optional<int64_t> cutPts100ns_;
    // Latched by the first post-cut packet: from then on every packet is held,
    // even one whose PTS would still fit, because a later picture may reference
    // an earlier one in decode order.
    bool cutLatched_ = false;
    std::vector<EncodedPacket> heldPackets_;
    uint64_t heldBytes_ = 0;
    // Set when the bounded hold overflows: the dropped tail can never be
    // reconstructed, so no packet after it may be written either.
    bool holdOverflowed_ = false;
    uint64_t videoPackets_ = 0;
    uint64_t videoBytes_ = 0;
    bool writeFailed_ = false;
    std::wstring lastError_;
};

// Removes a hidden, empty file at a path previously claimed for output.
// The attributes do not establish identity if another process replaces it.
// Returns true when such a file was removed.
bool removeEmptyOutputReservation(const std::filesystem::path& path);

} // namespace backtrack
