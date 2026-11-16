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
// Slots before the first submission are extrapolated backwards from the first
// submitted PTS by that submission's own duration, once per slot. Using the
// arriving packet's duration instead steps those slots backwards: cadence
// collapse makes it larger than the PTS gaps the queue actually had. The
// submission queue can exceed the configured offset when inputs are still in
// flight or outputs are harvested in batches; its size alone cannot identify a
// lost output.
class DecodeOrderTimestamps {
public:
    explicit DecodeOrderTimestamps(uint32_t reorderDelayFrames = 0)
        : reorderDelayFrames_(reorderDelayFrames) {
    }

    // Call once per accepted encoder submission, with the frame's PTS and its
    // duration. duration100ns only feeds the extrapolated slots; 0 means
    // unknown and defers to onOutput's duration.
    void onSubmit(int64_t pts100ns, int64_t duration100ns = 0) {
        submissions_.push_back({pts100ns, duration100ns});
    }

    // Call once per produced output packet, in decode order. Returns the
    // packet's decode timestamp; falls back to the presentation timestamp
    // minus the configured delay when no submission slots are pending.
    int64_t onOutput(int64_t fallbackPts100ns, int64_t frameDuration100ns) {
        const int64_t outputIndex = static_cast<int64_t>(outputsConsumed_);
        ++outputsConsumed_;
        const int64_t frameDuration = frameDuration100ns > 0 ? frameDuration100ns : 1;

        if (submissions_.empty()) {
            return fallbackPts100ns - static_cast<int64_t>(reorderDelayFrames_) * frameDuration;
        }
        const Submission& earliest = submissions_.front();
        if (outputIndex >= static_cast<int64_t>(reorderDelayFrames_)) {
            const int64_t dts = earliest.pts100ns;
            submissions_.pop_front();
            return dts;
        }
        // One stable per-slot duration for every extrapolated slot, clamped off
        // zero so the sequence stays strictly increasing.
        const int64_t slotDuration = earliest.duration100ns > 0 ? earliest.duration100ns : frameDuration;
        return earliest.pts100ns
            - static_cast<int64_t>(reorderDelayFrames_ - outputIndex) * slotDuration;
    }

    // Discards pending state when the encoder session is torn down or
    // re-initialized.
    void reset(uint32_t reorderDelayFrames) {
        submissions_.clear();
        outputsConsumed_ = 0;
        reorderDelayFrames_ = reorderDelayFrames;
    }

private:
    struct Submission {
        int64_t pts100ns;
        int64_t duration100ns;
    };

    std::deque<Submission> submissions_;
    uint64_t outputsConsumed_ = 0;
    uint32_t reorderDelayFrames_;
};

} // namespace backtrack
