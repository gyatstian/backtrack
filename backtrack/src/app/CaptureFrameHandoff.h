#pragma once

#include <utility>
#include <vector>

namespace backtrack {

template <typename Queue, typename Frame>
void retainQueuedCaptureFrames(Queue& queue, std::vector<Frame>& retained, Frame* popped = nullptr) {
    // Allocate before removing anything from the queue.
    retained.reserve(retained.size() + queue.capacity() + (popped ? 1 : 0));
    if (popped) {
        retained.push_back(std::move(*popped));
    }
    Frame frame;
    while (queue.tryPop(frame)) {
        retained.push_back(std::move(frame));
    }
}

// Keep capture leases alive until the encoder release step completes.
template <typename Frame, typename Drain>
bool releaseCaptureFrames(Frame& lastFrame, Frame& acquiredFrame, Drain&& drain) {
    if (!std::forward<Drain>(drain)()) {
        return false;
    }
    lastFrame = {};
    acquiredFrame = {};
    return true;
}

} // namespace backtrack
