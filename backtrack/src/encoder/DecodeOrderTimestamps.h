#pragma once

#include "core/Logger.h"

#include <cstdint>
#include <deque>

namespace backtrack {

// Assigns decode timestamps to hardware-encoder outputs when B-frame
// reordering delays them relative to the inputs.
//
// Hardware encoders emit exactly one output packet per accepted input, in
// decode order, but that output can lag the submission by up to
// reorderDelayFrames while B-frames wait for their forward reference. Output
// number k therefore occupies decoder slot (k - reorderDelayFrames), and its
// decode timestamp is taken from the input submitted in that same slot:
//
//     dts(k) = ptsSubmitted(k - reorderDelayFrames)
//
// Slots before the first output are extrapolated backwards by whole frame
// durations, so the first DTS sits below every PTS. That keeps the sequence
// strictly monotonic and every MP4 ctts composition offset (pts - dts)
// non-negative, which is what the MPEG-4 file sink requires for B-frame
// streams.
//
// Desync watchdog: once outputs flow 1:1 with submissions, the pending queue
// settles at exactly reorderDelayFrames entries. A driver that swallows an
// accepted frame (seen on flaky drivers after internal resets) leaves one
// entry unconsumed forever; every later output then pops the wrong slot and
// the whole rest of the recording plays with DTS shifted by whole frame
// durations (subtle A/V skew plus ctts jitter). When the surplus persists
// across many consecutive outputs - long enough to rule out normal pipeline
// fill, which drains within roughly reorderDelayFrames outputs - the stale
// slots are dropped from the front so the mapping realigns for all following
// outputs, and the condition is logged instead of silently misaligning.
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
    // when no submissions were recorded (defensive; cannot normally happen
    // because every output corresponds to one earlier submission).
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
            resyncIfDesynchronized();
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
        overrunOutputs_ = 0;
        reorderDelayFrames_ = reorderDelayFrames;
    }

private:
    // Consecutive outputs with a surplus queue before acting. Normal fill
    // overshoot decays within about reorderDelayFrames outputs; a swallowed
    // frame keeps the surplus forever, so this only adds a short detection
    // delay while ruling out bursty-but-healthy drivers.
    static constexpr uint64_t kMaxOverrunOutputs = 96;

    // Exactly-one-output-per-input watchdog; see class comment.
    void resyncIfDesynchronized() {
        const size_t nominalPending = reorderDelayFrames_;
        if (submittedPts_.size() <= nominalPending) {
            overrunOutputs_ = 0;
            return;
        }
        if (++overrunOutputs_ < kMaxOverrunOutputs) {
            return;
        }
        // FIFO order guarantees the surplus entries sit at the front: they
        // belong to frames whose outputs never came, and every legitimate
        // newer entry is queued behind them. Dropping them realigns all
        // following outputs with their true submission slots.
        const size_t surplus = submittedPts_.size() - nominalPending;
        submittedPts_.erase(
            submittedPts_.begin(),
            submittedPts_.begin() + static_cast<std::ptrdiff_t>(surplus));
        overrunOutputs_ = 0;
        Logger::instance().warning(L"encoder",
            L"Encoder produced fewer outputs than accepted inputs; dropped " +
            std::to_wstring(surplus) +
            L" stale timestamp slot(s) to resynchronize decode timestamps");
    }

    std::deque<int64_t> submittedPts_;
    uint64_t outputsConsumed_ = 0;
    uint64_t overrunOutputs_ = 0;
    uint32_t reorderDelayFrames_;
};

} // namespace backtrack
