#pragma once

#include "core/Types.h"
#include "mux/WavWriter.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <vector>

namespace backtrack {

struct MuxedInputs {
    std::filesystem::path videoPath;
    std::filesystem::path systemAudioPath;
    std::filesystem::path microphoneAudioPath;
    struct VideoSample {
        uint64_t offset = 0;
        uint32_t size = 0;
        int64_t pts100ns = 0;
        // Decode timestamp (<= pts100ns); differs from the PTS only when the
        // encoder used B-frames.
        int64_t dts100ns = 0;
        // Explicit validity flag: decode timestamps can legitimately be zero
        // or negative, so no value works as a "no DTS" sentinel.
        bool hasDts = false;
        int64_t duration100ns = 0;
        bool keyFrame = false;
    };
    std::vector<VideoSample> videoSamples;
    VideoCodec codec = VideoCodec::H264;
    uint32_t fps = 60;
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t bitrateKbps = 24000;
    ResolutionMode resolutionMode = ResolutionMode::P1080;
};

class Mp4Muxer {
public:
    bool startRecording(const std::filesystem::path& outputPath, const VideoSettings& settings);
    void writeVideoPacket(const EncodedPacket& packet);
    bool extendLastVideoDuration(int64_t expectedPts100ns, int64_t duration100ns);
    void writeAudioPacket(const AudioPacket& packet);
    std::filesystem::path finalize();
    void abort();
    bool active() const;
    uint64_t videoPacketCount() const;
    uint64_t videoByteCount() const;
    std::wstring lastError() const;
    const std::filesystem::path& outputPath() const { return outputPath_; }

    // stagingVariant: zero-based index selecting the staged output name
    // (<stem>.publish.mp4, then <stem>.publish.<variant>.mp4). Retry wrappers
    // rotate it when an external handle (antivirus/indexer) pins the previous
    // staging file. win32Error: optional receiver for the raw Win32 code of
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
    struct AudioTimelineState {
        WavWriter writer;
        int64_t nextPts100ns = 0;
        // Set once when a packet arrives whose format differs from the WAV the
        // writer was opened with; such packets are dropped instead of copied
        // raw (which would append wrong-rate samples and skew the timeline).
        bool warnedFormatMismatch = false;
    };

    void writeTimelineAudioPacket(AudioTimelineState& state, const AudioPacket& packet, const wchar_t* fileName);
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
    uint64_t videoPackets_ = 0;
    uint64_t videoBytes_ = 0;
    bool writeFailed_ = false;
    std::wstring lastError_;
};

// Removes path only when it is an empty, hidden zero-byte output-name
// reservation (created when a clip name is claimed before muxing; see
// RecorderController::nextClipPath and RecoveryStore's recovered-output
// claim). The hidden attribute is what distinguishes our reservation from a
// user's own empty .mp4 placeholder at the same path. Real content is never
// removed. Returns true when a reservation was found and removed.
bool removeEmptyOutputReservation(const std::filesystem::path& path);

} // namespace backtrack
