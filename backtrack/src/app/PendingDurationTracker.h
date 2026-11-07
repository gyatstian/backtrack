#pragma once

#include "mux/MuxedInputs.h"

#include <cstddef>
#include <cstdint>
#include <map>

namespace backtrack {

// Unmatched video-duration extensions, keyed by the frame PTS they extend.
// Bounded: when an insert would pass the cap, the smallest PTS (the oldest
// submitted frame) is evicted instead of relying on insertion order. A
// dropped encoder input can otherwise leave an orphaned key at the front of
// a separate order deque and grow bookkeeping without bound while the map
// itself stays small (finding 22).
class PendingDurationTracker {
public:
    static constexpr size_t kMaxPending = 512;

    void clear() { pending_.clear(); }

    size_t size() const { return pending_.size(); }

    // Record a duration extension for pts100ns. Coalesces on a second
    // extension to the same PTS. When the map exceeds the cap, the smallest
    // PTS is evicted. Returns the number of evictions performed (0 or 1).
    size_t extend(int64_t pts100ns, int64_t duration100ns) {
        auto [it, inserted] = pending_.try_emplace(pts100ns, duration100ns);
        if (!inserted) {
            it->second = coalesceVideoDuration100ns(it->second, duration100ns);
            return 0;
        }
        if (pending_.size() > kMaxPending) {
            pending_.erase(pending_.begin());
            return 1;
        }
        return 0;
    }

    // Apply the tracked extension for pts100ns to baseDuration100ns and
    // remove the entry. Returns the coalesced duration when pts100ns was
    // tracked, otherwise baseDuration100ns unchanged.
    int64_t consume(int64_t pts100ns, int64_t baseDuration100ns) {
        auto it = pending_.find(pts100ns);
        if (it == pending_.end()) {
            return baseDuration100ns;
        }
        const int64_t duration = coalesceVideoDuration100ns(baseDuration100ns, it->second);
        pending_.erase(it);
        return duration;
    }

    // Drop a tracked extension without applying it (used when the muxer or
    // replay buffer already consumed it via extendLastVideoDuration).
    void erase(int64_t pts100ns) { pending_.erase(pts100ns); }

    // Debug / test access.
    bool contains(int64_t pts100ns) const {
        return pending_.find(pts100ns) != pending_.end();
    }

private:
    std::map<int64_t, int64_t> pending_;
};

} // namespace backtrack
