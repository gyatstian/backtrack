// Regressions for audit findings 04, 06, 08 and 12 in RecorderController:
//  - 06: a missing encoder after a failed capture init is a pipeline fault
//    (the old `encoder_ && !encoder_->encoderAvailable()` treated null as
//    healthy, so the health check never recreated the pipeline);
//  - 08: a recording stop drains by encode-thread acknowledgement, not by PTS
//    (B-frame reordering lets a post-stop P frame complete before a pre-stop
//    B frame, so a highest-PTS comparison cut the recording early);
//  - 04: the acknowledgement is real completion accounting (submissions at or
//    below the cutoff, minus publications and encoder drops, plus the
//    boundary latch). A publication quiesce acked while accepted pre-cutoff
//    inputs were still retained by a deep-lookahead encoder;
//  - 08 (race): the 5 s encoder health probe reads encoder_ while a live GPU
//    recreate replaces it, so it must hold pipelineGpuMutex_.
//  - 12: a volume/cursor-only change with non-default replay settings is still
//    a live apply, not a pipeline restart that stops the active recording.
// The cutoff handshake lives in app/RecordingCutoffTracker.h and the fault
// predicate, probe snapshot and settings comparison views in
// app/RecorderController.h, so this suite compiles them standalone like the
// other controller-adjacent suites. The full pipeline behavior (capture-loop
// recovery, encode-thread handshake, the live-apply path itself) is a manual
// check.

#include "app/RecordingCutoffTracker.h"
#include "app/RecorderController.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using backtrack::RecordingCutoffTracker;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// ---------------------------------------------------------------- finding 04

// The stop drains on real completion accounting: every submitted frame at or
// below the cutoff is pending until its packet is published, a drop or an
// abandoned drain removes one, and the ack needs the post-stop boundary plus a
// zero pending count. A publication quiesce would ack while accepted pre-cutoff
// inputs still sat inside a deep-lookahead encoder (at 5 fps the next
// submission is a whole frame period away).

void testNoAckBeforeTheBoundary() {
    RecordingCutoffTracker tracker;
    const uint64_t gen = tracker.beginStop(500);
    tracker.noteFrameSubmitted(400);
    tracker.notePacketPublished(400);
    // No post-stop frame was submitted, so further pre-stop frames may still sit
    // in the queue; the boundary is what proves they all reached the encoder.
    expect(!tracker.acknowledgeIfDrained(), "must not ack before the post-stop boundary");
    expect(!tracker.acknowledged(gen), "not acknowledged without the boundary");
}

void testEmptyDrainAcksOnTheBoundary() {
    RecordingCutoffTracker tracker;
    const uint64_t gen = tracker.beginStop(500);
    // A packet from a submission made before beginStop: it decrements nothing
    // (saturating), so nothing is pending.
    tracker.notePacketPublished(400);
    expect(!tracker.acknowledgeIfDrained(), "nothing acks before the boundary");
    tracker.noteFrameSubmitted(600); // boundary reached, nothing in flight
    expect(tracker.acknowledgeIfDrained(), "the boundary alone completes an empty drain");
    expect(tracker.acknowledged(gen), "the generation must be acknowledged");
    expect(!tracker.acknowledgeIfDrained(), "the ack fires once");
}

void testDelayedPreCutoffPublicationKeepsTheDrainOpen() {
    RecordingCutoffTracker tracker;
    const uint64_t gen = tracker.beginStop(500);
    tracker.noteFrameSubmitted(480);
    tracker.noteFrameSubmitted(500);
    tracker.noteFrameSubmitted(520); // boundary: capture keeps producing
    // A post-stop P frame completes first (B-frame reordering). It must not
    // acknowledge: two pre-cutoff inputs are still inside the encoder.
    tracker.notePacketPublished(520);
    expect(!tracker.acknowledgeIfDrained(),
           "a post-cutoff publication must not ack pending pre-cutoff inputs");
    expect(!tracker.acknowledged(gen), "the stop must stay open");
    // The retained pre-cutoff frames arrive late, in decode order.
    tracker.notePacketPublished(480);
    expect(!tracker.acknowledgeIfDrained(), "one pre-cutoff input is still pending");
    // The publication itself carries the ack, so it is the return value to check.
    expect(tracker.notePacketPublished(500), "the drain completes with the last pre-cutoff packet");
    expect(tracker.acknowledged(gen), "the stop generation must be acknowledged");
}

void testPendingReachesZeroWithoutAnyQuiesce() {
    RecordingCutoffTracker tracker;
    tracker.beginStop(500);
    tracker.noteFrameSubmitted(500);
    tracker.noteFrameSubmitted(600);
    // No clock argument exists: the ack depends on the counter, not on elapsed
    // time. A quiesce-based tracker would refuse this immediately after the
    // publication.
    expect(tracker.notePacketPublished(500), "pending reaching zero must ack at once");
}

void testDroppedInputReleasesTheDrain() {
    RecordingCutoffTracker tracker;
    const uint64_t gen = tracker.beginStop(500);
    tracker.noteFrameSubmitted(480);
    tracker.noteFrameSubmitted(500);
    tracker.noteFrameSubmitted(520);
    tracker.notePacketPublished(480);
    expect(!tracker.acknowledgeIfDrained(), "one pre-cutoff input is still pending");
    // The encoder rejected 500; no packet will ever arrive for it.
    tracker.noteInputDropped();
    expect(tracker.acknowledgeIfDrained(), "a dropped input must not stall the stop");
    expect(tracker.acknowledged(gen), "the stop must be acknowledged");
}

void testDecrementsSaturateAtZero() {
    RecordingCutoffTracker tracker;
    tracker.beginStop(500);
    // More decrements than increments: publications from before beginStop plus
    // a drop. The counter must stay at zero instead of wrapping.
    tracker.notePacketPublished(480);
    tracker.notePacketPublished(490);
    tracker.noteInputDropped();
    tracker.noteFrameSubmitted(600);
    expect(tracker.acknowledgeIfDrained(), "a saturated counter reads as an empty drain");
}

void testAbandonedDrainAcksWithoutTheBoundary() {
    RecordingCutoffTracker tracker;
    const uint64_t gen = tracker.beginStop(500);
    tracker.noteFrameSubmitted(480);
    expect(!tracker.acknowledgeIfDrained(), "no ack while a pre-cutoff input is pending");
    // The encoder faulted mid-drain: nothing can be published and no further
    // submission can arrive either.
    expect(tracker.abandonPending(), "abandoning the pending inputs acks the stop");
    expect(tracker.acknowledged(gen), "the faulted-encoder escape must unblock the stop");
    expect(!tracker.abandonPending(), "the ack fires once per stop");
    tracker.endStop();
    expect(!tracker.abandonPending(), "no abandonment without a pending stop");
}

void testSynchronousDrainAcksWithoutBoundary() {
    RecordingCutoffTracker tracker;
    const uint64_t gen = tracker.beginStop(500);
    // stopPipeline drains the queue and the encoder on the same thread. No
    // post-stop frame was ever captured, so the boundary never latched, but
    // the completed synchronous drain IS the acknowledgement.
    expect(!tracker.acknowledged(gen), "not acknowledged until the drain acks");
    tracker.forceAcknowledge();
    expect(tracker.acknowledged(gen), "the synchronous drain must acknowledge the stop");
}

void testStopAndResetCycle() {
    RecordingCutoffTracker tracker;
    tracker.reset();
    const uint64_t gen1 = tracker.beginStop(100);
    tracker.noteFrameSubmitted(200);
    tracker.notePacketPublished(100);
    tracker.forceAcknowledge();
    expect(tracker.acknowledged(gen1), "the first stop must be acknowledged");
    tracker.endStop();
    tracker.noteFrameSubmitted(50);
    expect(!tracker.stopping(), "endStop must clear the pending stop state");

    const uint64_t gen2 = tracker.beginStop(200);
    expect(!tracker.acknowledged(gen2), "a fresh stop must not start acknowledged");
    // A publication of a submission made before this beginStop has no matching
    // increment in the new generation.
    tracker.notePacketPublished(100);
    tracker.noteFrameSubmitted(150);
    expect(!tracker.acknowledgeIfDrained(), "the new stop has its own pending count");
    tracker.noteFrameSubmitted(250);
    expect(!tracker.acknowledgeIfDrained(), "the pre-cutoff submission is still pending");
    expect(tracker.notePacketPublished(150), "the boundary plus the last pre-cutoff packet ack the stop");
    tracker.endStop();

    tracker.reset();
    const uint64_t gen3 = tracker.beginStop(300);
    expect(!tracker.acknowledged(gen3), "a reset must not leak an acknowledged stop");
    expect(tracker.stopping(), "a started stop stays pending until endStop");
    tracker.endStop();
}

// ---------------------------------------------------------------- finding 06

class FakeEncoder final : public backtrack::IEncoder {
public:
    explicit FakeEncoder(bool available) : available_(available) {}

    bool initialize(backtrack::D3DDevice&, const backtrack::VideoSettings&) override { return true; }
    void requestKeyFrame() override {}
    bool resetInputResources() override { return true; }
    bool encodeFrame(const backtrack::GpuFrame&, backtrack::EncodedPacket&) override { return false; }
    void drain(std::vector<backtrack::EncodedPacket>& packets) override { packets.clear(); }
    void shutdown() override {}
    backtrack::EncoderCapabilities capabilities() const override { return {}; }
    backtrack::EncoderStats stats() const override { return {}; }
    bool encoderAvailable() const noexcept override { return available_; }
    DXGI_FORMAT preferredInputFormat() const override { return DXGI_FORMAT_B8G8R8A8_UNORM; }

private:
    bool available_;
};

void testMissingEncoderIsAFault() {
    // D3D init succeeded but capture init failed -> encoder_ is null. The
    // health check must read that as a fault (finding 06); the old
    // `encoder_ && !encoder_->encoderAvailable()` treated null as healthy.
    expect(backtrack::pipelineEncoderFaulted(nullptr),
           "a missing encoder must be a pipeline fault");

    FakeEncoder healthy(true);
    expect(!backtrack::pipelineEncoderFaulted(&healthy),
           "a present, available encoder must not fault");

    FakeEncoder faulted(false);
    expect(backtrack::pipelineEncoderFaulted(&faulted),
           "an unavailable encoder must be a pipeline fault");
}

// A pipeline that faults again before it published a packet is a broken
// pipeline, not a transient: the recreate interval must escalate instead of
// tearing capture down every 500 ms, and a published packet must reset it.
void testEncoderFaultBackoff() {
    backtrack::EncoderFaultBackoff backoff;
    expect(backoff.next() == std::chrono::milliseconds(500),
           "a transient fault must keep the fast retry");
    expect(backoff.next() == std::chrono::milliseconds(1000), "a repeated fault must back off");
    expect(backoff.next() == std::chrono::milliseconds(2000), "backoff must keep doubling");
    expect(backoff.next() == std::chrono::milliseconds(4000), "backoff must keep doubling");
    expect(backoff.next() == std::chrono::milliseconds(8000), "backoff must reach the cap");
    expect(backoff.next() == std::chrono::milliseconds(8000), "backoff must stay capped");
    backoff.reset();
    expect(backoff.next() == std::chrono::milliseconds(500),
           "a published packet must reset the backoff");
}

// averageEncodeMs is submit->output latency whose healthy floor is about one
// frame period, so the probe must not warn on a healthy pipeline. It warns
// only when the encoder is actually being fed near the target rate (an idle
// hold submits a heartbeat IDR per GOP interval, not real flow) and latency
// exceeds three frame periods.
void testEncoderHealthProbe() {
    // Healthy: ~one frame period of latency, feeding at the target rate.
    expect(!backtrack::encoderBehindBudget(21.0, 60, 300, 5),
           "healthy latency near one frame period must not warn");
    // Latency above three frame periods while feeding at rate: a real stall.
    expect(backtrack::encoderBehindBudget(200.0, 60, 300, 5),
           "high latency while feeding at rate must warn");
    // Same high latency but idle feed (heartbeats only): must not warn.
    expect(!backtrack::encoderBehindBudget(200.0, 60, 3, 5),
           "high latency from idle heartbeat holds must not warn");
    // Just under the three-period threshold stays quiet.
    expect(!backtrack::encoderBehindBudget(49.0, 60, 300, 5),
           "latency under the three-period threshold must not warn");
    // Unknown fps (0) falls back to the 16.667 ms budget.
    expect(backtrack::encoderBehindBudget(200.0, 0, 100, 5),
           "zero fps must fall back to a default budget");
    // No expected-feed baseline: any feed rate is treated as fed.
    expect(backtrack::encoderBehindBudget(200.0, 0, 0, 5),
           "zero fps with no submissions must still warn on high latency");
}

// ---------------------------------------------------------------- finding 08 (encoder probe race)

// Stand-in for IEncoder::stats(): the probe must only reach it while it owns the
// pipeline GPU lock, because a recreate destroys and replaces that encoder there.
struct ProbeEncoder {
    const std::atomic<bool>* recreating = nullptr;
    std::atomic<bool>* reachedDuringRecreate = nullptr;
    int calls = 0;

    backtrack::EncoderStats stats() {
        ++calls;
        if (recreating && reachedDuringRecreate && recreating->load()) {
            reachedDuringRecreate->store(true);
        }
        backtrack::EncoderStats snapshot;
        snapshot.averageEncodeMs = 12.5;
        snapshot.submittedFrames = 7;
        return snapshot;
    }
};

void testEncoderProbeTakesTheGpuLock() {
    std::mutex gpuMutex;
    std::unique_ptr<ProbeEncoder> encoder = std::make_unique<ProbeEncoder>();

    const auto free_ = backtrack::probeEncoderStats(gpuMutex, encoder);
    expect(free_.has_value() && free_->submittedFrames == 7 && encoder->calls == 1,
           "a free pipeline lock must yield the encoder stats");

    {
        std::scoped_lock recreate(gpuMutex);
        expect(!backtrack::probeEncoderStats(gpuMutex, encoder).has_value(),
               "a running recreate must skip the probe tick");
        expect(encoder->calls == 1, "a running recreate must not be raced by the probe");
    }

    encoder.reset();
    expect(!backtrack::probeEncoderStats(gpuMutex, encoder).has_value(),
           "a missing encoder (failed recreate) has no stats to report");
}

// The probe runs on every encode-loop iteration while a recreate replaces the
// encoder on another thread. Reading the pointer outside the lock is the
// finding-08 data race; the helper must never expose the encoder to the probe
// while the recreate owns it.
void testEncoderProbeDuringEncoderReplacement() {
    std::mutex gpuMutex;
    std::atomic<bool> recreating{false};
    std::atomic<bool> reachedDuringRecreate{false};
    std::unique_ptr<ProbeEncoder> encoder = std::make_unique<ProbeEncoder>();
    encoder->recreating = &recreating;
    encoder->reachedDuringRecreate = &reachedDuringRecreate;

    std::thread probe([&] {
        for (int i = 0; i < 2000; ++i) {
            (void)backtrack::probeEncoderStats(gpuMutex, encoder);
        }
    });
    for (int i = 0; i < 50; ++i) {
        {
            std::scoped_lock recreate(gpuMutex);
            recreating.store(true);
            encoder = std::make_unique<ProbeEncoder>();
            encoder->recreating = &recreating;
            encoder->reachedDuringRecreate = &reachedDuringRecreate;
            recreating.store(false);
        }
        std::this_thread::yield();
    }
    probe.join();
    expect(!reachedDuringRecreate.load(),
           "the probe must never read an encoder a recreate is replacing");
}

// ---------------------------------------------------------------- finding 12

// A volume/cursor/AGC-only change applies through the mirror fields, so it must
// classify as live even when the replay settings are non-default (disabled, a
// custom window, a ring-folder override). The old comparison stripped replay
// from the incoming side only, so those settings looked like a pipeline change
// and a full restart stopped the active recording.
void testLiveSettingsClassification() {
    backtrack::AppSettings current;
    current.replay.enabled = false;
    current.replay.seconds = 300;
    current.replay.diskBackedVideo = false;
    current.replayRingDirectory = L"D:\\ring";
    current.audioOutputVolumePercent = 80;
    current.captureCursor = true;

    auto volumeOnly = current;
    volumeOnly.audioOutputVolumePercent = 60;
    expect(volumeOnly != current, "the volume change must be a real change");
    // The live view keeps the real replay settings. The finding-12 defect was an
    // asymmetric strip (incoming replay cleared, current replay real), which made
    // every such change look like a pipeline change and restart the pipeline.
    expect(backtrack::liveFieldsCleared(current).replay == current.replay &&
               backtrack::liveFieldsCleared(current).replayRingDirectory == current.replayRingDirectory,
           "the live comparison must compare the real replay settings, not cleared ones");
    expect(backtrack::liveFieldsCleared(volumeOnly) == backtrack::liveFieldsCleared(current),
           "a volume-only change with non-default replay settings must take the live path");

    auto cursorOnly = current;
    cursorOnly.captureCursor = false;
    expect(backtrack::liveFieldsCleared(cursorOnly) == backtrack::liveFieldsCleared(current),
           "a cursor-only change must take the live path");

    auto agcOnly = current;
    agcOnly.audioInputAutoGainEnabled = true;
    agcOnly.audioInputVolumePercent = 140;
    expect(backtrack::liveFieldsCleared(agcOnly) == backtrack::liveFieldsCleared(current),
           "an AGC/input-volume change must take the live path");

    auto pipelineChange = current;
    pipelineChange.video.fps = current.video.fps + 30;
    expect(backtrack::liveFieldsCleared(pipelineChange) != backtrack::liveFieldsCleared(current),
           "a video change must still take the restart path");

    // A replay change is never a live-apply: the replay branch above owns it,
    // and the restart path owns it while the pipeline is stopped.
    auto replayWindow = current;
    replayWindow.replay.seconds = 30;
    expect(backtrack::replayFieldsCleared(replayWindow) == backtrack::replayFieldsCleared(current),
           "a replay-window change is confined to the replay fields");
    expect(backtrack::liveFieldsCleared(replayWindow) != backtrack::liveFieldsCleared(current),
           "a replay-window change must never be applied as a live change");

    auto replayToggled = current;
    replayToggled.replay.enabled = true;
    expect(backtrack::replayFieldsCleared(replayToggled) == backtrack::replayFieldsCleared(current),
           "re-enabling replay is confined to the replay fields");
    expect(backtrack::liveFieldsCleared(replayToggled) != backtrack::liveFieldsCleared(current),
           "re-enabling replay must not be swallowed by the live path");

    // Live fields on both sides of one update still classify as live, and the
    // replay fields are untouched, so the replay branch cannot claim it either.
    auto both = current;
    both.audioOutputVolumePercent = 90;
    both.captureCursor = false;
    expect(backtrack::liveFieldsCleared(both) == backtrack::liveFieldsCleared(current),
           "several live-field changes together must still be live");
    expect(both.replay == current.replay &&
               both.replayRingDirectory == current.replayRingDirectory,
           "a live-only change leaves the replay settings alone");
}

// The dispatch itself, not just the two comparison views: updateSettings has to
// pick the live path for a live-only change even with non-default replay
// settings, and the replay path only for a replay-only change. Dropping the
// moved-replay-fields term makes a volume tick take the replay branch, which
// rewrites the unchanged replay settings and silently drops the volume, and
// every view assertion above still holds.
void testSettingsApplyDispatch() {
    backtrack::AppSettings current;
    current.replay.enabled = false;
    current.replay.seconds = 300;
    current.replay.diskBackedVideo = false;
    current.replayRingDirectory = L"D:\\ring";
    current.audioOutputVolumePercent = 80;
    const bool running = true;
    const bool startOnly = false;

    const auto path = [&](const backtrack::AppSettings& incoming, bool pipelineRunning = true,
                          bool startReplayPipeline = false) {
        return backtrack::settingsApplyPath(current, incoming, pipelineRunning, startReplayPipeline);
    };
    const auto replayPath = backtrack::SettingsApplyPath::ReplayInPlace;
    const auto livePath = backtrack::SettingsApplyPath::LiveInPlace;
    const auto restartPath = backtrack::SettingsApplyPath::Restart;

    auto volume = current;
    volume.audioOutputVolumePercent = 60;
    expect(path(volume) == livePath,
           "a volume tick with non-default replay settings must take the live path");
    expect(path(volume) != replayPath,
           "a volume tick must never take the in-place replay path (it drops the volume)");

    auto cursor = current;
    cursor.captureCursor = false;
    expect(path(cursor) == livePath, "a cursor-only change must take the live path");

    auto inputGain = current;
    inputGain.audioInputAutoGainEnabled = true;
    inputGain.audioInputVolumePercent = 140;
    expect(path(inputGain) == livePath, "an AGC/input-volume change must take the live path");

    auto replayWindow = current;
    replayWindow.replay.seconds = 30;
    expect(path(replayWindow) == replayPath,
           "a replay-only change with the pipeline running must reconfigure in place");
    expect(path(replayWindow) != livePath, "a replay-only change must never take the live path");
    expect(path(replayWindow, !running) == restartPath,
           "a replay-only change with the pipeline stopped must take the restart path");

    auto ringDirectory = current;
    ringDirectory.replayRingDirectory = L"E:\\ring";
    expect(path(ringDirectory) == replayPath,
           "a ring-folder-only change must reconfigure the buffer in place");

    auto replayEnabled = current;
    replayEnabled.replay.enabled = true;
    expect(path(replayEnabled) == restartPath,
           "toggling replay must not be applied by an in-place replay reconfigure");

    auto videoChange = current;
    videoChange.video.fps = current.video.fps + 30;
    expect(path(videoChange) == restartPath, "a video change must take the restart path");

    // The live mirrors are published even while the pipeline is stopped, and a
    // run that only starts the pipeline resolves Restart through ensurePipeline.
    expect(path(volume, !running) == livePath,
           "a live-only change applies with the pipeline stopped too");
    expect(path(volume, running, true) == restartPath,
           "a start-only run must reach ensurePipeline, not the live path");
    expect(path(volume, !running, startOnly) == livePath,
           "an ordinary live-only change is unaffected by the start-only flag");
    // The in-place replay path requires the replay fields to have actually moved:
    // an unchanged run must never be routed into a buffer reconfigure, not even a
    // start-only one whose running flag already reads settled.
    expect(path(current, running, true) == restartPath,
           "the in-place replay path needs a real replay change");
}

} // namespace

int main() {
    try {
        testNoAckBeforeTheBoundary();
        testEmptyDrainAcksOnTheBoundary();
        testDelayedPreCutoffPublicationKeepsTheDrainOpen();
        testPendingReachesZeroWithoutAnyQuiesce();
        testDroppedInputReleasesTheDrain();
        testDecrementsSaturateAtZero();
        testAbandonedDrainAcksWithoutTheBoundary();
        testSynchronousDrainAcksWithoutBoundary();
        testStopAndResetCycle();
        testMissingEncoderIsAFault();
        testEncoderFaultBackoff();
        testEncoderHealthProbe();
        testEncoderProbeTakesTheGpuLock();
        testEncoderProbeDuringEncoderReplacement();
        testLiveSettingsClassification();
        testSettingsApplyDispatch();
        std::cout << "RecorderControllerCutoffTests: all tests passed" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RecorderControllerCutoffTests failed: " << error.what() << std::endl;
        return 1;
    }
}
