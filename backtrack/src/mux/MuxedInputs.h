#pragma once

#include "core/Types.h"

#include <cstdint>
#include <filesystem>
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

// Effective decode timestamp for one sample: valid only when explicitly
// flagged and not past the presentation time (the encoders never produce a
// DTS past PTS); otherwise the presentation time stands in. Zero and negative
// timestamps are legal values, so validity comes from the flag alone. Shared
// by the recording path and ReplayBuffer::saveTo, which must agree exactly,
// or the same packet muxes with different timing depending on the path.
inline int64_t effectiveDts(int64_t pts100ns, int64_t dts100ns, bool hasDts) {
    return (hasDts && dts100ns <= pts100ns) ? dts100ns : pts100ns;
}

inline bool hasEffectiveDts(int64_t pts100ns, int64_t dts100ns, bool hasDts) {
    return hasDts && dts100ns <= pts100ns;
}

// Canonical single-sample duration ceiling: storage-level backstop for
// wall-clock durations from coalesced hold intervals. Covers the largest
// healthy heartbeat (30 s max GOP plus low-fps coalesce slop) yet stays ~1400x
// below a day-long exclusive-fullscreen hold. Shared by the recording path
// (Mp4Muxer::extendLastVideoDuration) and the replay path
// (ReplayBuffer::pushVideo/extendLastVideoDuration), which must agree exactly
// or the same hold muxes with different timing depending on the path.
inline constexpr int64_t kMaxSingleVideoDuration100ns = 60 * kHundredNanosecondsPerSecond;

inline int64_t clampSingleVideoDuration100ns(int64_t duration100ns) {
    return duration100ns > kMaxSingleVideoDuration100ns ? kMaxSingleVideoDuration100ns : duration100ns;
}

// Duration coalescing for static-screen holds: the tail only ever stretches,
// then the ceiling applies. Both writers call this so neither can publish an
// unclamped tail the other would have cut.
inline int64_t coalesceVideoDuration100ns(int64_t current100ns, int64_t incoming100ns) {
    return clampSingleVideoDuration100ns(current100ns > incoming100ns ? current100ns : incoming100ns);
}

} // namespace backtrack
