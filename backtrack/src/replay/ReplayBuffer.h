#pragma once

#include "core/Types.h"

#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>

namespace backtrack {

class ReplayBuffer {
public:
    void configure(const ReplaySettings& settings);
    void pushVideo(EncodedPacket packet);
    bool extendLastVideoDuration(int64_t expectedPts100ns, int64_t duration100ns);
    void pushAudio(AudioPacket packet);
    bool saveTo(const std::filesystem::path& outputPath, const VideoSettings& videoSettings);
    void clear();
    size_t videoPacketCount() const;
    size_t videoKeyFrameCount() const;
    // Duration (100ns) of muxable video currently buffered, from the first
    // buffered keyframe to the newest packet end. 0 when empty or keyframe-less.
    int64_t videoSpan100ns() const;

private:
    void trimLocked();

    mutable std::mutex mutex_;
    ReplaySettings settings_;
    std::deque<std::shared_ptr<const EncodedPacket>> video_;
    std::deque<std::shared_ptr<const AudioPacket>> systemAudio_;
    std::deque<std::shared_ptr<const AudioPacket>> microphoneAudio_;
    size_t videoKeyFrames_ = 0;
    int64_t lastSaveEndPts100ns_ = 0;
};

} // namespace backtrack
