#pragma once

#include <atomic>
#include <cstdint>

namespace backtrack {

// Decides when a recording-stop drain is complete. PTS alone cannot: B-frame
// reordering lets a post-stop P frame reach the encoder output before a
// pre-stop B frame, so a highest-PTS comparison would cut the recording early
// and strand the pre-stop tail (finding 08). Nor can a publication quiesce:
// with deep lookahead the next submission is a frame period away, so "nothing
// published for 100 ms" can be true while accepted pre-cutoff inputs still sit
// inside the encoder (finding 04).
//
// The acknowledgment is real completion accounting over three encode-thread
// signals:
//   - submissions: the first post-stop frame submitted latches the boundary
//     (the queue delivers frames in PTS order, so every pre-stop frame has
//     reached the encoder by then); every submission at or below the cutoff
//     increments the pending count;
//   - publications: a packet at or below the cutoff decrements it;
//   - drops: an encoder-side drop of a submitted input decrements it too, so a
//     frame that will never be published cannot stall the stop.
// The stop acknowledges once the boundary latched and nothing is pending.
//
// Header-only so a unit suite can compile it standalone, like
// VideoTimelineScheduler.h and PendingDurationTracker.h.
class RecordingCutoffTracker {
public:
    // Starts a stop drain for `cutoffPts100ns`; returns the generation the
    // caller waits on.
    uint64_t beginStop(int64_t cutoffPts100ns) {
        cutoffPts100ns_.store(cutoffPts100ns, std::memory_order_release);
        allSubmitted_.store(false, std::memory_order_release);
        pendingInputs_.store(0, std::memory_order_release);
        return generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    void endStop() {
        cutoffPts100ns_.store(-1, std::memory_order_release);
    }

    // Full reset between pipeline runs; a fresh pipeline starts with no
    // pending or acknowledged stop.
    void reset() {
        cutoffPts100ns_.store(-1, std::memory_order_release);
        generation_.store(0, std::memory_order_release);
        ackedGeneration_.store(0, std::memory_order_release);
        allSubmitted_.store(false, std::memory_order_release);
        pendingInputs_.store(0, std::memory_order_release);
    }

    bool stopping() const {
        return cutoffPts100ns_.load(std::memory_order_acquire) >= 0;
    }

    // Called for every frame the encode thread submits. Latches the boundary
    // on the first post-stop submission and counts the pre-stop ones that are
    // still owed a publication.
    void noteFrameSubmitted(int64_t pts100ns) {
        const int64_t cutoff = cutoffPts100ns_.load(std::memory_order_acquire);
        if (cutoff < 0) {
            return;
        }
        if (pts100ns > cutoff) {
            allSubmitted_.store(true, std::memory_order_release);
            return;
        }
        pendingInputs_.fetch_add(1, std::memory_order_acq_rel);
    }

    // Called for every packet the encode thread publishes. Returns true when
    // this publication completed the drain (the caller wakes the stop wait).
    bool notePacketPublished(int64_t pts100ns) {
        const int64_t cutoff = cutoffPts100ns_.load(std::memory_order_acquire);
        if (cutoff < 0) {
            return false;
        }
        if (pts100ns <= cutoff) {
            // Saturating: a packet from a submission made before beginStop has
            // no matching increment.
            decrementPending();
        }
        return acknowledgeIfDrained();
    }

    // An input the encoder accepted and then abandoned produces no packet, so
    // it has to leave the pending count here or the stop waits out its timeout.
    void noteInputDropped() {
        if (!stopping()) {
            return;
        }
        decrementPending();
    }

    // Escape for a drain whose encoder is gone: nothing in flight can ever be
    // published, so the outstanding inputs are written off and the boundary is
    // taken as reached (no further submission is possible either).
    bool abandonPending() {
        if (!stopping()) {
            return false;
        }
        pendingInputs_.store(0, std::memory_order_release);
        allSubmitted_.store(true, std::memory_order_release);
        return acknowledgeIfDrained();
    }

    // Polled by the encode loop and by stopRecording's wait. Fires once per
    // stop.
    bool acknowledgeIfDrained() {
        if (!stopping() ||
            ackedGeneration_.load(std::memory_order_acquire) >=
                generation_.load(std::memory_order_acquire)) {
            return false;
        }
        if (!allSubmitted_.load(std::memory_order_acquire)) {
            return false;
        }
        if (pendingInputs_.load(std::memory_order_acquire) != 0) {
            return false;
        }
        ackedGeneration_.store(generation_.load(std::memory_order_acquire), std::memory_order_release);
        return true;
    }

    bool acknowledged(uint64_t generation) const {
        return ackedGeneration_.load(std::memory_order_acquire) >= generation;
    }

    // Forces the ack after a synchronous drain (stopPipeline drains the frame
    // queue and the encoder on the same thread, so the drain completing IS the
    // acknowledgement; the boundary latch may never fire when no post-stop
    // frame was captured before the pipeline stopped).
    void forceAcknowledge() {
        ackedGeneration_.store(generation_.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    void decrementPending() {
        uint64_t pending = pendingInputs_.load(std::memory_order_acquire);
        while (pending > 0 &&
               !pendingInputs_.compare_exchange_weak(
                   pending,
                   pending - 1,
                   std::memory_order_acq_rel,
                   std::memory_order_acquire)) {
        }
    }

    std::atomic<int64_t> cutoffPts100ns_{-1};
    std::atomic<uint64_t> generation_{0};
    std::atomic<uint64_t> ackedGeneration_{0};
    std::atomic<bool> allSubmitted_{false};
    std::atomic<uint64_t> pendingInputs_{0};
};

} // namespace backtrack
