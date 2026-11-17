#include "app/PendingDurationTracker.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using backtrack::PendingDurationTracker;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testInsertConsumeCoalesce() {
    PendingDurationTracker tracker;
    expect(tracker.size() == 0, "tracker is not empty initially");

    expect(tracker.extend(100, 1000) == 0, "first insert evicted an entry");
    expect(tracker.size() == 1, "first insert did not record the extension");
    expect(tracker.consume(100, 500) == 1000, "consume did not apply the tracked extension");
    expect(tracker.size() == 0, "consume did not remove the tracked entry");
    expect(tracker.consume(100, 500) == 500, "consume of an untracked PTS changed the base duration");

    // A second extension to the same PTS coalesces (max) instead of growing.
    expect(tracker.extend(200, 1000) == 0, "second insert evicted an entry");
    expect(tracker.extend(200, 3000) == 0, "coalesce re-insert evicted an entry");
    expect(tracker.size() == 1, "coalesce grew the tracker");
    expect(tracker.consume(200, 2000) == 3000, "coalesce did not keep the larger duration");

    expect(tracker.extend(300, 1000) == 0, "third insert evicted an entry");
    tracker.erase(300);
    expect(tracker.size() == 0, "erase did not remove the tracked entry");
}

void testEvictionKeepsBounded() {
    PendingDurationTracker tracker;
    constexpr int64_t kFirst = 1000;
    for (int64_t pts = kFirst; pts < kFirst + 1024; ++pts) {
        tracker.extend(pts, 1000);
        expect(tracker.size() <= PendingDurationTracker::kMaxPending,
            "tracker grew past its cap");
    }
    expect(tracker.size() == PendingDurationTracker::kMaxPending,
        "tracker did not settle at its cap after 1024 inserts");
    // The smallest PTS (oldest submitted frame) is evicted first.
    expect(!tracker.contains(kFirst), "eviction did not remove the smallest PTS");
    expect(!tracker.contains(kFirst + 511), "eviction stopped too early");
    expect(tracker.contains(kFirst + 512), "eviction removed a retained PTS");
    expect(tracker.contains(kFirst + 1023), "eviction removed the newest PTS");
}

void testEvictionPicksSmallestPtsNotInsertionOrder() {
    PendingDurationTracker tracker;
    // 1000 inserted first but is not the smallest; 100 is the smallest and is
    // inserted among the rest. Eviction must drop 100 (smallest PTS), not
    // 1000 (first inserted).
    expect(tracker.extend(1000, 1000) == 0, "first insert evicted an entry");
    for (int64_t pts = 100; pts < 100 + PendingDurationTracker::kMaxPending; ++pts) {
        tracker.extend(pts, 1000);
    }
    expect(tracker.size() == PendingDurationTracker::kMaxPending,
        "tracker did not settle at its cap");
    expect(!tracker.contains(100), "eviction did not drop the smallest PTS");
    expect(tracker.contains(1000), "eviction dropped the first-inserted but not-smallest PTS");
}

void testUnmatchedPtsKeepsBookkeepingBounded() {
    // Finding 22 regression: one unmatched PTS (its encoder input was dropped)
    // plus 100,000 later extensions that are all consumed. Bookkeeping must
    // stay bounded without a restart; the unmatched entry survives because the
    // map never reaches its cap.
    PendingDurationTracker tracker;
    expect(tracker.extend(7, 5000) == 0, "orphan insert evicted an entry");
    constexpr int64_t kUpdates = 100'000;
    for (int64_t pts = 1000; pts < 1000 + kUpdates; ++pts) {
        tracker.extend(pts, 1000);
        expect(tracker.size() <= PendingDurationTracker::kMaxPending,
            "bookkeeping grew past its cap during consumed updates");
        tracker.consume(pts, 1000);
    }
    expect(tracker.size() == 1, "bookkeeping did not return to just the unmatched PTS");
    expect(tracker.contains(7), "the unmatched PTS was evicted or lost");
    expect(tracker.consume(7, 0) == 5000, "the unmatched PTS lost its tracked duration");
    expect(tracker.size() == 0, "consuming the unmatched PTS left bookkeeping behind");
}

} // namespace

int main() {
    try {
        testInsertConsumeCoalesce();
        testEvictionKeepsBounded();
        testEvictionPicksSmallestPtsNotInsertionOrder();
        testUnmatchedPtsKeepsBookkeepingBounded();
        std::cout << "PendingDurationTracker tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PendingDurationTracker test failure: " << error.what() << '\n';
        return 1;
    }
}
