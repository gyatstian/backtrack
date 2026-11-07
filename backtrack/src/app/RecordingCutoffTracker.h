#pragma once

#include <atomic>
#include <cstdint>

namespace backtrack {

// Decides when a recording-stop drain is complete. PTS alone cannot: B-frame
// reordering lets a post-stop P frame reach the encoder output before a
// pre-stop B frame, so a highest-PTS comparison would cut the recording early
// and strand the pre-stop tail (finding 08). The encode thread reports every
// frame it submits and every packet it publishes; the stop acknowledges once
// every frame captured at or before the cutoff has been published.
//
// The acknowledgment needs two independent signals:
//   - the boundary: the first post-stop frame submitted proves every pre-stop
//     frame reached the encoder (the queue delivers frames in PTS order), so
//     the encoder's reorder buffer will flush;
//   - the quiesce: no pre-stop packet published for the grace period, so a
//     dropped frame or an in-flight packet cannot stall the stop forever.
// Both together keep the replay-enabled case correct (capture keeps producing
// post-stop frames, so the boundary alone would acknowledge while pre-stop
// frames still sat in the queue).
//
// Header-only so a unit suite can compile it standalone, like
// VideoTimelineScheduler.h and PendingDurationTracker.h.
class RecordingCutoffTracker {
public:
    // Starts a stop drain for `cutoffPts100ns`; returns the generation the
    // caller waits on. `now100ns` seeds the publication quiesce so a stop with
    // no pre-stop frame left in flight still acknowledges after the grace
    // period.
    uint64_t beginStop(int64_t cutoffPts100ns, int64_t now100ns) {
        cutoffPts100ns_.store(cutoffPts100ns, std::memory_order_release);
        allSubmitted_.store(false, std::memory_order_release);
        lastPreCutoffPublishAt100ns_.store(now100ns, std::memory_order_release);
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
        lastPreCutoffPublishAt100ns_.store(0, std::memory_order_release);
    }

    bool stopping() const {
        return cutoffPts100ns_.load(std::memory_order_acquire) >= 0;
    }

    // PTS gate for the muxer while a stop is pending: only packets at or
    // below the cutoff belong in the recording.
    bool shouldPublishToMuxer(int64_t pts100ns) const {
        const int64_t cutoff = cutoffPts100ns_.load(std::memory_order_acquire);
        return cutoff < 0 || pts100ns <= cutoff;
    }

    // Called for every frame the encode thread submits. Latches the boundary
    // on the first post-stop submission.
    void noteFrameSubmitted(int64_t pts100ns) {
        const int64_t cutoff = cutoffPts100ns_.load(std::memory_order_acquire);
        if (cutoff >= 0 && pts100ns > cutoff) {
            allSubmitted_.store(true, std::memory_order_release);
        }
    }

    // Called for every packet the encode thread publishes. Returns true when
    // this publication completed the drain (the caller wakes the stop wait).
    bool notePacketPublished(int64_t pts100ns, int64_t now100ns) {
        if (!stopping()) {
            return false;
        }
        if (pts100ns <= cutoffPts100ns_.load(std::memory_order_acquire)) {
            lastPreCutoffPublishAt100ns_.store(now100ns, std::memory_order_release);
            return acknowledgeIfDrained(now100ns);
        }
        return false;
    }

    // Polled by the encode loop and by stopRecording's wait. Fires once per
    // stop.
    bool acknowledgeIfDrained(int64_t now100ns) {
        if (!stopping() ||
            ackedGeneration_.load(std::memory_order_acquire) >=
                generation_.load(std::memory_order_acquire)) {
            return false;
        }
        if (!allSubmitted_.load(std::memory_order_acquire)) {
            return false;
        }
        const int64_t last = lastPreCutoffPublishAt100ns_.load(std::memory_order_acquire);
        if (now100ns < last || now100ns - last < kQuiesce100ns) {
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
    // A pre-stop publication can lag the boundary by a reorder depth of a few
    // frames; 100 ms covers that plus dropped-frame jitter without stretching
    // the stop into the seconds the old PTS wait could take.
    static constexpr int64_t kQuiesce100ns = 100 * 10'000;

    std::atomic<int64_t> cutoffPts100ns_{-1};
    std::atomic<uint64_t> generation_{0};
    std::atomic<uint64_t> ackedGeneration_{0};
    std::atomic<bool> allSubmitted_{false};
    std::atomic<int64_t> lastPreCutoffPublishAt100ns_{0};
};

} // namespace backtrack
