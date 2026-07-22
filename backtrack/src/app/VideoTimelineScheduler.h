#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace backtrack {

struct CadenceAdvance {
    std::chrono::steady_clock::time_point emitTime{};
    std::chrono::steady_clock::time_point nextEmit{};
    uint32_t intervalCount = 0;
};

inline std::optional<CadenceAdvance> advanceVideoCadence(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point nextEmit,
    std::chrono::steady_clock::duration frameInterval,
    bool stopRequested,
    uint32_t maximumCollapsedIntervals = 4) {
    if (stopRequested || now < nextEmit || frameInterval <= std::chrono::steady_clock::duration::zero()) {
        return std::nullopt;
    }

    CadenceAdvance result;
    result.emitTime = nextEmit;
    result.intervalCount = 1;
    result.nextEmit = nextEmit + frameInterval;
    maximumCollapsedIntervals = std::max<uint32_t>(maximumCollapsedIntervals, 1);
    while (now >= result.nextEmit && result.intervalCount < maximumCollapsedIntervals) {
        result.nextEmit += frameInterval;
        ++result.intervalCount;
    }
    return result;
}

enum class VideoTimelineAction {
    Submit,
    Coalesce,
};

struct VideoTimelineEmission {
    VideoTimelineAction action = VideoTimelineAction::Submit;
    bool duplicateFrame = false;
    bool requestKeyFrame = false;
    uint32_t intervalCount = 1;
    int64_t pts100ns = 0;
    int64_t duration100ns = 0;
    int64_t previousFramePts100ns = 0;
    int64_t previousFrameDuration100ns = 0;
    int64_t coalescedFramePts100ns = 0;
    int64_t coalescedFrameDuration100ns = 0;
    uint64_t sourceFrameIndex = 0;
};

class VideoTimelineScheduler {
public:
    explicit VideoTimelineScheduler(int64_t nominalFrameDuration100ns)
        : nominalFrameDuration100ns_(std::max<int64_t>(nominalFrameDuration100ns, 1)) {
    }

    VideoTimelineEmission plan(
        uint64_t sourceFrameIndex,
        int64_t emitPts100ns,
        uint32_t intervalCount,
        bool allowIdleCoalescing,
        bool forcedHeartbeat,
        bool periodicHeartbeat) const {
        VideoTimelineEmission emission;
        emission.duplicateFrame = haveLastSubmittedFrame_ && sourceFrameIndex == lastSubmittedSourceFrameIndex_;
        emission.requestKeyFrame = forcedHeartbeat || periodicHeartbeat;
        emission.intervalCount = std::max<uint32_t>(intervalCount, 1);
        emission.pts100ns = emitPts100ns;
        emission.duration100ns = nominalFrameDuration100ns_ * emission.intervalCount;
        emission.sourceFrameIndex = sourceFrameIndex;

        if (emission.duplicateFrame && allowIdleCoalescing && !emission.requestKeyFrame) {
            emission.action = VideoTimelineAction::Coalesce;
            emission.coalescedFramePts100ns = lastSubmittedPts100ns_;
            emission.coalescedFrameDuration100ns = std::max<int64_t>(
                nominalFrameDuration100ns_,
                emitPts100ns + emission.duration100ns - lastSubmittedPts100ns_);
            return emission;
        }

        emission.action = VideoTimelineAction::Submit;
        if (haveLastSubmittedFrame_) {
            emission.previousFramePts100ns = lastSubmittedPts100ns_;
            emission.previousFrameDuration100ns = std::max<int64_t>(
                nominalFrameDuration100ns_,
                emitPts100ns - lastSubmittedPts100ns_);
        }
        return emission;
    }

    void acceptSubmission(const VideoTimelineEmission& emission) {
        if (emission.action != VideoTimelineAction::Submit) {
            return;
        }
        haveLastSubmittedFrame_ = true;
        lastSubmittedSourceFrameIndex_ = emission.sourceFrameIndex;
        lastSubmittedPts100ns_ = emission.pts100ns;
    }

    void reset() {
        haveLastSubmittedFrame_ = false;
        lastSubmittedSourceFrameIndex_ = 0;
        lastSubmittedPts100ns_ = 0;
    }

    bool hasSubmittedFrame() const {
        return haveLastSubmittedFrame_;
    }

    int64_t lastSubmittedPts100ns() const {
        return lastSubmittedPts100ns_;
    }

private:
    int64_t nominalFrameDuration100ns_ = 1;
    bool haveLastSubmittedFrame_ = false;
    uint64_t lastSubmittedSourceFrameIndex_ = 0;
    int64_t lastSubmittedPts100ns_ = 0;
};

} // namespace backtrack
