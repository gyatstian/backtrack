// Regressions for audit findings 06 and 08 in RecorderController:
//  - 06: a missing encoder after a failed capture init is a pipeline fault
//    (the old `encoder_ && !encoder_->encoderAvailable()` treated null as
//    healthy, so the health check never recreated the pipeline);
//  - 08: a recording stop drains by encode-thread acknowledgement, not by PTS
//    (B-frame reordering lets a post-stop P frame complete before a pre-stop
//    B frame, so a highest-PTS comparison cut the recording early).
// The cutoff handshake lives in app/RecordingCutoffTracker.h and the fault
// predicate in app/RecorderController.h, so this suite compiles them
// standalone like the other controller-adjacent suites. The full pipeline
// behavior (capture-loop recovery, encode-thread handshake) is a manual check.

#include "app/RecordingCutoffTracker.h"
#include "app/RecorderController.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using backtrack::RecordingCutoffTracker;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// ---------------------------------------------------------------- finding 08

void testPtsGate() {
    RecordingCutoffTracker tracker;
    expect(tracker.shouldPublishToMuxer(1000), "no pending stop must pass every PTS");

    tracker.beginStop(500, 0);
    expect(tracker.shouldPublishToMuxer(500), "the cutoff packet itself must pass");
    expect(tracker.shouldPublishToMuxer(499), "an older packet must pass");
    expect(!tracker.shouldPublishToMuxer(501), "a post-cutoff packet must be blocked");

    tracker.endStop();
    expect(tracker.shouldPublishToMuxer(9999), "the gate must reopen after endStop");
}

void testNoAckWithoutPostStopBoundary() {
    RecordingCutoffTracker tracker;
    const uint64_t gen = tracker.beginStop(500, 0);
    tracker.notePacketPublished(480, 0);
    // No post-stop frame has been submitted, so pre-stop frames may still sit
    // in the queue. The tracker refuses to acknowledge no matter how long the
    // wait; the caller bounds it (finding 08: a PTS comparison could not tell
    // this stalled case from a completed flush).
    expect(!tracker.acknowledgeIfDrained(10 * 100 * 10000),
           "must not ack before the post-stop boundary");
    expect(!tracker.acknowledged(gen), "not acknowledged without the boundary");
}

void testQuiesceGatesTheAck() {
    RecordingCutoffTracker tracker;
    const uint64_t gen = tracker.beginStop(500, 0);
    tracker.noteFrameSubmitted(600); // boundary reached: capture kept producing
    tracker.notePacketPublished(500, 0);
    expect(!tracker.acknowledgeIfDrained(99 * 10000),
           "must not ack inside the quiesce window");
    expect(tracker.acknowledgeIfDrained(100 * 10000),
           "ack once the quiesce window has passed");
    expect(tracker.acknowledged(gen), "the generation must be acknowledged");
    expect(!tracker.acknowledgeIfDrained(200 * 10000), "the ack fires once");
}

void testDelayedPreCutoffTailIsKept() {
    RecordingCutoffTracker tracker;
    const uint64_t gen = tracker.beginStop(500, 0);
    // Capture keeps producing after the stop, so the boundary latches.
    tracker.noteFrameSubmitted(520);
    // A post-stop P frame completes first (B-frame reordering). It must not
    // reach the recording, and publishing it alone must not acknowledge.
    expect(!tracker.shouldPublishToMuxer(520), "post-stop output must not reach the recording");
    tracker.notePacketPublished(520, 10);
    expect(!tracker.acknowledgeIfDrained(10 + 99 * 10000),
           "must not ack while the pre-stop tail is still retained");
    // The retained pre-stop frames arrive late, in decode order, and must all
    // reach the recording (the regression: they used to go to replay only).
    expect(tracker.shouldPublishToMuxer(490), "delayed pre-stop frame must pass the gate");
    expect(tracker.shouldPublishToMuxer(500), "delayed pre-stop frame must pass the gate");
    tracker.notePacketPublished(490, 20);
    tracker.notePacketPublished(500, 30);
    expect(!tracker.acknowledgeIfDrained(30 + 99 * 10000),
           "must not ack before the newest pre-stop packet is out");
    expect(tracker.acknowledgeIfDrained(30 + 100 * 10000),
           "ack once the newest pre-stop packet is out and quiesced");
    expect(tracker.acknowledged(gen), "the stop generation must be acknowledged");
}

void testDroppedPreCutoffFrameDoesNotStall() {
    RecordingCutoffTracker tracker;
    const uint64_t gen = tracker.beginStop(500, 0);
    tracker.noteFrameSubmitted(600);
    // The encoder dropped the remaining pre-stop frames (queue pressure or a
    // session error); the 100 ms publication quiesce must let the stop finish
    // instead of stalling forever.
    tracker.notePacketPublished(480, 0);
    expect(!tracker.acknowledgeIfDrained(99 * 10000), "not yet inside the quiesce");
    expect(tracker.acknowledgeIfDrained(100 * 10000),
           "a dropped frame must not stall the stop");
    expect(tracker.acknowledged(gen), "the stop must be acknowledged");
}

void testSynchronousDrainAcksWithoutBoundary() {
    RecordingCutoffTracker tracker;
    const uint64_t gen = tracker.beginStop(500, 0);
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
    const uint64_t gen1 = tracker.beginStop(100, 0);
    tracker.noteFrameSubmitted(200);
    tracker.notePacketPublished(100, 0);
    tracker.forceAcknowledge();
    expect(tracker.acknowledged(gen1), "the first stop must be acknowledged");
    tracker.endStop();

    const uint64_t gen2 = tracker.beginStop(200, 0);
    expect(!tracker.acknowledged(gen2), "a fresh stop must not start acknowledged");
    tracker.forceAcknowledge();
    expect(tracker.acknowledged(gen2), "the second stop must be acknowledged");
    tracker.endStop();

    tracker.reset();
    const uint64_t gen3 = tracker.beginStop(300, 0);
    expect(!tracker.acknowledged(gen3), "a reset must not leak an acknowledged stop");
    tracker.endStop();
}

// ---------------------------------------------------------------- finding 06

class FakeEncoder final : public backtrack::IEncoder {
public:
    explicit FakeEncoder(bool available) : available_(available) {}

    bool initialize(backtrack::D3DDevice&, const backtrack::VideoSettings&) override { return true; }
    void requestKeyFrame() override {}
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

} // namespace

int main() {
    try {
        testPtsGate();
        testNoAckWithoutPostStopBoundary();
        testQuiesceGatesTheAck();
        testDelayedPreCutoffTailIsKept();
        testDroppedPreCutoffFrameDoesNotStall();
        testSynchronousDrainAcksWithoutBoundary();
        testStopAndResetCycle();
        testMissingEncoderIsAFault();
        testEncoderFaultBackoff();
        testEncoderHealthProbe();
        std::cout << "RecorderControllerCutoffTests: all tests passed" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RecorderControllerCutoffTests failed: " << error.what() << std::endl;
        return 1;
    }
}
