#pragma once

namespace backtrack {

// Encode-loop exit predicate. `stopRequested_` alone cannot end the loop: the
// capture worker may be paused inside its final queue push when the stop
// lands, and a loop that leaves on the flag alone lets that push land in a
// queue nobody drains, which teardown then clears (audit finding 10). Only the
// capture worker having finished, with the queue and the duration bookkeeping
// drained, proves no later submission can arrive.
//
// Header-only and Windows-free so a unit suite compiles it standalone, like
// VideoTimelineScheduler.h and RecordingCutoffTracker.h.
inline bool videoEncodeLoopDone(
    bool stopRequested,
    bool captureFinished,
    bool queueEmpty,
    bool durationUpdatePending) {
    return stopRequested && captureFinished && queueEmpty && !durationUpdatePending;
}

} // namespace backtrack
