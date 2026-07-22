#include "app/VideoTimelineScheduler.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using backtrack::VideoTimelineAction;
using backtrack::VideoTimelineScheduler;
using Clock = std::chrono::steady_clock;

constexpr int64_t kFrameDuration100ns = 10'000'000 / 60;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testNormalCadence() {
    const auto interval = std::chrono::nanoseconds(1'000'000'000 / 60);
    const Clock::time_point start{};

    expect(!backtrack::advanceVideoCadence(start - std::chrono::nanoseconds(1), start, interval, false),
        "cadence emitted before its deadline");

    const auto first = backtrack::advanceVideoCadence(start, start, interval, false);
    expect(first.has_value(), "cadence did not emit at its deadline");
    expect(first->emitTime == start, "first cadence timestamp changed");
    expect(first->nextEmit == start + interval, "first cadence did not advance by one interval");
    expect(first->intervalCount == 1, "normal cadence collapsed more than one interval");

    const auto second = backtrack::advanceVideoCadence(start + interval, first->nextEmit, interval, false);
    expect(second.has_value(), "second cadence interval was not emitted");
    expect(second->emitTime == start + interval, "second cadence timestamp is not monotonic");
    expect(second->intervalCount == 1, "second normal cadence interval was collapsed");
}

void testCatchUpCollapseAndStop() {
    const auto interval = std::chrono::milliseconds(10);
    const Clock::time_point start{};

    const auto stalled = backtrack::advanceVideoCadence(
        start + 5 * interval,
        start,
        interval,
        false,
        4);
    expect(stalled.has_value(), "stalled cadence did not emit");
    expect(stalled->emitTime == start, "stalled cadence moved the first elapsed timestamp");
    expect(stalled->intervalCount == 4, "stalled cadence did not honor the collapse bound");
    expect(stalled->nextEmit == start + 4 * interval, "stalled cadence advanced to the wrong deadline");

    const auto remainder = backtrack::advanceVideoCadence(
        start + 5 * interval,
        stalled->nextEmit,
        interval,
        false,
        4);
    expect(remainder.has_value(), "elapsed cadence remainder was lost");
    expect(remainder->intervalCount == 2, "elapsed cadence remainder has the wrong interval count");
    expect(remainder->nextEmit == start + 6 * interval, "elapsed cadence remainder ended at the wrong deadline");

    expect(!backtrack::advanceVideoCadence(start, start, interval, true),
        "stop request allowed a new cadence emission");
    expect(!backtrack::advanceVideoCadence(start, start, Clock::duration::zero(), false),
        "zero frame interval allowed a cadence emission");
}

void testIdleCoalescingAndFinalHeldDuration() {
    VideoTimelineScheduler scheduler(kFrameDuration100ns);
    constexpr int64_t startPts = 50'000'000;

    const auto first = scheduler.plan(7, startPts, 1, true, false, false);
    expect(first.action == VideoTimelineAction::Submit, "first source frame was not submitted");
    expect(!first.duplicateFrame, "first source frame was classified as duplicate");
    expect(first.duration100ns == kFrameDuration100ns, "first source frame duration is wrong");
    scheduler.acceptSubmission(first);

    const auto idle = scheduler.plan(7, startPts + kFrameDuration100ns, 1, true, false, false);
    expect(idle.action == VideoTimelineAction::Coalesce, "idle duplicate was submitted to the encoder");
    expect(idle.duplicateFrame, "idle duplicate was not classified as duplicate");
    expect(idle.coalescedFramePts100ns == startPts, "idle update targeted the wrong sample");
    expect(idle.coalescedFrameDuration100ns == 2 * kFrameDuration100ns,
        "idle update did not extend through the second timeline interval");

    const auto finalHold = scheduler.plan(7, startPts + 2 * kFrameDuration100ns, 3, true, false, false);
    expect(finalHold.action == VideoTimelineAction::Coalesce, "collapsed final hold was submitted");
    expect(finalHold.intervalCount == 3, "collapsed final hold lost elapsed intervals");
    expect(finalHold.coalescedFrameDuration100ns == 5 * kFrameDuration100ns,
        "final held-frame duration does not cover the full timeline");
}

void testHeartbeatAndDistinctFrameSubmission() {
    VideoTimelineScheduler scheduler(kFrameDuration100ns);
    constexpr int64_t startPts = 70'000'000;

    const auto first = scheduler.plan(10, startPts, 1, true, false, false);
    scheduler.acceptSubmission(first);

    const auto periodic = scheduler.plan(10, startPts + kFrameDuration100ns, 1, true, false, true);
    expect(periodic.action == VideoTimelineAction::Submit, "periodic heartbeat was coalesced");
    expect(periodic.requestKeyFrame, "periodic heartbeat did not request a keyframe");
    expect(periodic.previousFramePts100ns == startPts, "heartbeat did not close the preceding sample");
    expect(periodic.previousFrameDuration100ns == kFrameDuration100ns,
        "heartbeat assigned the wrong preceding duration");
    scheduler.acceptSubmission(periodic);

    const auto forced = scheduler.plan(10, startPts + 2 * kFrameDuration100ns, 1, true, true, false);
    expect(forced.action == VideoTimelineAction::Submit, "forced heartbeat was coalesced");
    expect(forced.requestKeyFrame, "forced heartbeat did not request a keyframe");
    scheduler.acceptSubmission(forced);

    const auto distinct = scheduler.plan(11, startPts + 3 * kFrameDuration100ns, 1, true, false, false);
    expect(distinct.action == VideoTimelineAction::Submit, "distinct source image was coalesced");
    expect(!distinct.duplicateFrame, "distinct source image was classified as duplicate");
}

void testQueueRejectionDoesNotCommitState() {
    VideoTimelineScheduler scheduler(kFrameDuration100ns);
    constexpr int64_t startPts = 90'000'000;

    const auto first = scheduler.plan(20, startPts, 1, true, false, false);
    scheduler.acceptSubmission(first);

    const auto rejected = scheduler.plan(21, startPts + kFrameDuration100ns, 1, true, false, false);
    expect(rejected.action == VideoTimelineAction::Submit, "new source image was not planned for submission");

    const auto retry = scheduler.plan(21, startPts + 2 * kFrameDuration100ns, 1, true, false, false);
    expect(retry.action == VideoTimelineAction::Submit, "rejected source image was incorrectly coalesced on retry");
    expect(!retry.duplicateFrame, "rejected source image changed scheduler state");
    expect(retry.previousFramePts100ns == startPts, "retry no longer references the last accepted sample");
    expect(retry.previousFrameDuration100ns == 2 * kFrameDuration100ns,
        "retry did not extend the accepted sample across queue saturation");
    scheduler.acceptSubmission(retry);

    const auto duplicate = scheduler.plan(21, startPts + 3 * kFrameDuration100ns, 1, true, false, false);
    expect(duplicate.action == VideoTimelineAction::Coalesce, "accepted retry was not tracked for later idle coalescing");
}

void testResetAndCoalescingGate() {
    VideoTimelineScheduler scheduler(kFrameDuration100ns);
    constexpr int64_t startPts = 110'000'000;

    const auto first = scheduler.plan(30, startPts, 1, true, false, false);
    scheduler.acceptSubmission(first);

    const auto gated = scheduler.plan(30, startPts + kFrameDuration100ns, 1, false, false, false);
    expect(gated.action == VideoTimelineAction::Submit, "disabled coalescing still coalesced a duplicate");

    scheduler.reset();
    expect(!scheduler.hasSubmittedFrame(), "reset retained submitted-frame state");
    expect(scheduler.lastSubmittedPts100ns() == 0, "reset retained the previous timestamp");

    const auto afterReset = scheduler.plan(30, startPts + 2 * kFrameDuration100ns, 1, true, false, false);
    expect(afterReset.action == VideoTimelineAction::Submit, "first frame after reset was coalesced");
    expect(!afterReset.duplicateFrame, "source index reuse after reset was classified as duplicate");
    expect(afterReset.previousFrameDuration100ns == 0, "reset leaked a preceding sample duration");
}

} // namespace

int main() {
    try {
        testNormalCadence();
        testCatchUpCollapseAndStop();
        testIdleCoalescingAndFinalHeldDuration();
        testHeartbeatAndDistinctFrameSubmission();
        testQueueRejectionDoesNotCommitState();
        testResetAndCoalescingGate();
        std::cout << "VideoTimelineScheduler tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "VideoTimelineScheduler test failure: " << error.what() << '\n';
        return 1;
    }
}
