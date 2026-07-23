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
        int64_t duration100ns = 0;
        bool keyFrame = false;
    };
    std::vector<VideoSample> videoSamples;
    VideoCodec codec = VideoCodec::H264;
    uint32_t fps = 60;
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t bitrateKbps = 24000;
    ResolutionMode resolutionMode = ResolutionMode::Native;
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

    static bool muxToMp4(const MuxedInputs& inputs, const std::filesystem::path& outputPath);
    static std::filesystem::path recoverLatestFailedRecording(
        const std::filesystem::path& clipDirectory,
        std::wstring& detail);

private:
    struct AudioTimelineState {
        WavWriter writer;
        int64_t nextPts100ns = 0;
    };

    void writeTimelineAudioPacket(AudioTimelineState& state, const AudioPacket& packet, const wchar_t* fileName);
    void flushPendingAudioLocked();

    mutable std::mutex mutex_;
    bool active_ = false;
    VideoSettings settings_;
    std::filesystem::path outputPath_;
    std::filesystem::path tempDirectory_;
    std::filesystem::path videoPath_;
    std::ofstream videoStream_;
    std::vector<MuxedInputs::VideoSample> videoSamples_;
    AudioTimelineState systemAudio_;
    AudioTimelineState microphoneAudio_;
    std::vector<AudioPacket> pendingAudio_;
    std::optional<int64_t> firstVideoPts100ns_;
    uint64_t videoPackets_ = 0;
    uint64_t videoBytes_ = 0;
    bool writeFailed_ = false;
    std::wstring lastError_;
};

} // namespace backtrack
