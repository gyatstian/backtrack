#pragma once

#include <cstdint>
#include <deque>

namespace backtrack {

// Assigns decode timestamps to hardware-encoder outputs when B-frame
// reordering delays them relative to the inputs.
//
// Hardware encoders emit one packet per accepted input, in decode order.
// Output number k occupies decoder slot (k - reorderDelayFrames), whose
// timestamp comes from the input submitted in that slot:
//
//     dts(k) = ptsSubmitted(k - reorderDelayFrames)
//
// Earlier slots are extrapolated backwards from the first submitted PTS by
// whole frame durations. The submission queue can exceed the configured
// offset when inputs are still in flight or outputs are harvested in batches;
// its size alone cannot identify a lost output.
class DecodeOrderTimestamps {
public:
    explicit DecodeOrderTimestamps(uint32_t reorderDelayFrames = 0)
        : reorderDelayFrames_(reorderDelayFrames) {
    }

    // Call once per accepted encoder submission, with the frame's PTS.
    void onSubmit(int64_t pts100ns) {
        submittedPts_.push_back(pts100ns);
    }

    // Call once per produced output packet, in decode order. Returns the
    // packet's decode timestamp; falls back to the presentation timestamp
    // minus the configured delay when no submission slots are pending.
    int64_t onOutput(int64_t fallbackPts100ns, int64_t frameDuration100ns) {
        const int64_t outputIndex = static_cast<int64_t>(outputsConsumed_);
        ++outputsConsumed_;
        const int64_t frameDuration = frameDuration100ns > 0 ? frameDuration100ns : 1;

        if (submittedPts_.empty()) {
            return fallbackPts100ns - static_cast<int64_t>(reorderDelayFrames_) * frameDuration;
        }
        if (outputIndex >= static_cast<int64_t>(reorderDelayFrames_)) {
            const int64_t dts = submittedPts_.front();
            submittedPts_.pop_front();
            return dts;
        }
        return submittedPts_.front()
            - static_cast<int64_t>(reorderDelayFrames_ - outputIndex) * frameDuration;
    }

    // Discards pending state when the encoder session is torn down or
    // re-initialized.
    void reset(uint32_t reorderDelayFrames) {
        submittedPts_.clear();
        outputsConsumed_ = 0;
        reorderDelayFrames_ = reorderDelayFrames;
    }

private:
    std::deque<int64_t> submittedPts_;
    uint64_t outputsConsumed_ = 0;
    uint32_t reorderDelayFrames_;
};

} // namespace backtrack
