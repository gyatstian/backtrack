#include "encoder/DecodeOrderTimestamps.h"

#include "core/Logger.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

using backtrack::DecodeOrderTimestamps;

// 100 fps, so every expected value below is a round number.
constexpr int64_t kFrameDuration100ns = 100'000;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// How many resync lines the logger has seen. The logger is a process singleton
// that is never initialized here, so a case that legitimately trips the
// watchdog leaves its line behind for every case after it: a "the watchdog did
// not fire" assertion has to compare against a baseline, not against zero.
size_t resyncLines() {
    size_t count = 0;
    for (const std::wstring& line : backtrack::Logger::instance().recentLines(200)) {
        if (line.find(L"resynchronize decode timestamps") != std::wstring::npos) {
            ++count;
        }
    }
    return count;
}

// The watchdog's whole promise is that the realignment is logged, not silent.
// recentLines keeps the last 200 lines with no initialize() needed, so the test
// asserts against the same sink a Diagnostics page would show.
bool resyncLoggedSince(size_t baseline) {
    return resyncLines() > baseline;
}

// Documented mapping: dts(k) = ptsSubmitted(k - reorderDelayFrames). Below the
// depth there is no submission in the slot yet, so the DTS is extrapolated
// below the first PTS by whole frame durations - the same
// (k - depth) * frameDuration expression, just with a negative offset.
int64_t expectedDts(int outputIndex, uint32_t reorderDelayFrames) {
    return static_cast<int64_t>(outputIndex - static_cast<int>(reorderDelayFrames)) *
        kFrameDuration100ns;
}

// One frame in, one output out, in that order: the app hands the encoder a frame
// before the encode thread drains the previous output.
int64_t submitFirstStep(DecodeOrderTimestamps& dts, int64_t& nextPts) {
    dts.onSubmit(nextPts);
    const int64_t decoded = dts.onOutput(nextPts, kFrameDuration100ns);
    nextPts += kFrameDuration100ns;
    return decoded;
}

// The same 1:1 cadence, phase-shifted: the frame is handed over after the drain.
// Which of the two a real session uses is the caller's threading, not this
// class's choice, and the declared pipeline depth is the same number either way.
int64_t drainFirstStep(DecodeOrderTimestamps& dts, int64_t& nextPts) {
    const int64_t decoded = dts.onOutput(nextPts, kFrameDuration100ns);
    dts.onSubmit(nextPts);
    nextPts += kFrameDuration100ns;
    return decoded;
}

void drainFirstFill(DecodeOrderTimestamps& dts, int64_t& nextPts, int frames) {
    for (int index = 0; index < frames; ++index) {
        dts.onSubmit(nextPts);
        nextPts += kFrameDuration100ns;
    }
}

void testDtsEqualsPtsWithoutReordering() {
    const size_t baseline = resyncLines();
    DecodeOrderTimestamps dts(0);
    int64_t previous = (std::numeric_limits<int64_t>::min)();

    for (int k = 0; k < 500; ++k) {
        const int64_t pts = static_cast<int64_t>(k) * kFrameDuration100ns;
        dts.onSubmit(pts);
        const int64_t decoded = dts.onOutput(pts, kFrameDuration100ns);
        expect(decoded == pts,
            "without reordering the decode timestamp must equal the presentation timestamp");
        expect(decoded > previous, "the decode timestamp sequence is not strictly monotonic");
        previous = decoded;
    }
    expect(!resyncLoggedSince(baseline), "an un-reordered pipeline tripped the desync watchdog");
}

void testBFrameReorderingOffsetsDecodeByExactlyTheDelay() {
    const size_t baseline = resyncLines();
    constexpr uint32_t kReorderDelay = 2;
    DecodeOrderTimestamps dts(kReorderDelay);
    int64_t previous = (std::numeric_limits<int64_t>::min)();

    for (int k = 0; k < 400; ++k) {
        const int64_t pts = static_cast<int64_t>(k) * kFrameDuration100ns;
        dts.onSubmit(pts);
        const int64_t decoded = dts.onOutput(pts, kFrameDuration100ns);

        expect(decoded == expectedDts(k, kReorderDelay),
            "a reordered output did not take the timestamp of the input in its decode slot");
        // The MPEG-4 file sink rejects a negative composition offset.
        expect(pts - decoded >= 0, "a composition offset went negative");
        expect(decoded > previous, "the decode timestamp sequence is not strictly monotonic");
        previous = decoded;
    }
    // The pending queue settles at exactly the declared depth, so the watchdog
    // must stay quiet across a long reordered session.
    expect(!resyncLoggedSince(baseline), "a healthy reordered pipeline tripped the desync watchdog");
}

void testHealthyCadenceNeverTripsTheWatchdog() {
    // Same 1:1 submit/output cadence, both phases, a long run each. A pipeline
    // that is not losing frames must never look desynchronized.
    {
        const size_t baseline = resyncLines();
        constexpr uint32_t kDepth = 2;
        DecodeOrderTimestamps dts(kDepth);
        int64_t nextPts = 0;
        for (int k = 0; k < 400; ++k) {
            expect(submitFirstStep(dts, nextPts) == expectedDts(k, kDepth),
                "the submit-then-drain cadence lost the documented mapping");
        }
        expect(!resyncLoggedSince(baseline),
            "the submit-then-drain cadence tripped the desync watchdog");
    }

    {
        const size_t baseline = resyncLines();
        // A three-deep pipeline (two B-frame slots plus one lookahead) driven
        // drain-first. One frame of fill is what this phase leaves pending on
        // top of the declared depth, so three is the correct depth here.
        constexpr uint32_t kDepth = 3;
        DecodeOrderTimestamps dts(kDepth);
        int64_t nextPts = 0;
        drainFirstFill(dts, nextPts, 1);
        for (int k = 0; k < 400; ++k) {
            (void)drainFirstStep(dts, nextPts);
        }
        expect(!resyncLoggedSince(baseline),
            "the drain-then-submit cadence tripped the desync watchdog");
    }
}

void testResetClearsTheWatchdogCounter() {
    // encoder/AGENTS.md: reset runs on every initialize and component
    // re-creation. If it kept the consecutive-overrun count, the tail of one
    // session would trip the watchdog in the first outputs of the next.
    constexpr uint32_t kDepth = 2;
    DecodeOrderTimestamps dts(kDepth);
    int64_t nextPts = 0;

    const size_t baseline = resyncLines();
    drainFirstFill(dts, nextPts, 2);
    for (int k = 0; k < 50; ++k) {
        (void)drainFirstStep(dts, nextPts);
    }
    expect(!resyncLoggedSince(baseline), "the desync watchdog fired below its threshold");

    dts.reset(kDepth);
    drainFirstFill(dts, nextPts, 2);
    for (int k = 0; k < 50; ++k) {
        (void)drainFirstStep(dts, nextPts);
    }
    expect(!resyncLoggedSince(baseline),
        "reset did not clear the watchdog's consecutive-output counter");
}

void testWatchdogFiresExactlyAtTheNinetySixthSurplusOutput() {
    // The only trigger is a surplus that outlives normal pipeline fill: this
    // cadence settles one slot deeper than the declared depth, which is what a
    // driver that accepted a frame and never emitted an output for looks like.
    constexpr uint32_t kDepth = 2;
    constexpr int kInitialFill = 2;
    constexpr int kWatchdogOutputs = 96;

    DecodeOrderTimestamps dts(kDepth);
    int64_t nextPts = 0;
    drainFirstFill(dts, nextPts, kInitialFill);

    const size_t baseline = resyncLines();
    int resyncAt = -1;
    for (int k = 0; k < 400 && resyncAt < 0; ++k) {
        const size_t before = resyncLines();
        const int64_t decoded = drainFirstStep(dts, nextPts);
        if (resyncLoggedSince(before)) {
            resyncAt = k;
        } else {
            // Nothing is dropped before the threshold, so the documented mapping
            // still holds right up to it.
            expect(decoded == static_cast<int64_t>(k - static_cast<int>(kDepth)) * kFrameDuration100ns,
                "a decode timestamp shifted before the desync watchdog fired");
        }
    }
    expect(resyncAt >= 0, "a surplus that never cleared never tripped the desync watchdog");
    expect(resyncLines() == baseline + 1,
        "the desync watchdog did not log exactly one resync for the case that tripped it");
    expect(resyncAt - static_cast<int>(kDepth) + 1 == kWatchdogOutputs,
        "the desync watchdog did not fire on its 96th consecutive surplus output");

    // The drop realigns every following output by exactly one whole frame, and
    // it stays that far off: a realignment, not a per-output drift. The output
    // that tripped the watchdog already returned its pre-drop slot, so the run
    // below starts one frame earlier than that slot rather than at it.
    int64_t previous =
        static_cast<int64_t>(resyncAt - static_cast<int>(kDepth) + 1) * kFrameDuration100ns;
    for (int k = resyncAt + 1; k < resyncAt + 100; ++k) {
        const int64_t decoded = drainFirstStep(dts, nextPts);
        expect(decoded == static_cast<int64_t>(k - static_cast<int>(kDepth) + 1) * kFrameDuration100ns,
            "a post-resync output was not realigned by exactly one frame duration");
        expect(decoded - previous == kFrameDuration100ns,
            "a post-resync decode timestamp drifted by other than one frame duration");
        previous = decoded;
    }
}

void testResetWithTooSmallDepthTripsTheWatchdog() {
    // encoder/AGENTS.md: reset with less than the real pipeline depth and every
    // steady-state output looks like a swallowed frame.
    //
    // The cadence matters for what "the pending depth" is. The first
    // `reorderDelayFrames` outputs extrapolate without popping a slot, so after a
    // fill of F the queue settles at F + depth - 1 by the time each output is
    // checked, and the surplus the watchdog sees is therefore F - 1, independent
    // of the declared depth. A four-frame pipeline is a fill of four with the
    // correct depth of four (surplus 0), and the same four frames in flight
    // declared as one (fill four, surplus 3).
    constexpr uint32_t kRealDepth = 4;
    DecodeOrderTimestamps dts(kRealDepth);
    int64_t nextPts = 0;

    const size_t baseline = resyncLines();

    drainFirstFill(dts, nextPts, 1);
    for (int k = 0; k < 300; ++k) {
        (void)drainFirstStep(dts, nextPts);
    }
    expect(!resyncLoggedSince(baseline), "the correct pipeline depth tripped the desync watchdog");

    dts.reset(1);
    drainFirstFill(dts, nextPts, kRealDepth);

    int resyncAt = -1;
    for (int k = 0; k < 300 && resyncAt < 0; ++k) {
        const size_t before = resyncLines();
        (void)drainFirstStep(dts, nextPts);
        if (resyncLoggedSince(before)) {
            resyncAt = k;
        }
    }
    expect(resyncAt >= 0, "a too-small pipeline depth never tripped the desync watchdog");
    expect(resyncAt >= 1, "the desync watchdog fired before any output consumed a slot");
}

} // namespace

int main() {
    try {
        testDtsEqualsPtsWithoutReordering();
        testBFrameReorderingOffsetsDecodeByExactlyTheDelay();
        testHealthyCadenceNeverTripsTheWatchdog();
        testResetClearsTheWatchdogCounter();
        testWatchdogFiresExactlyAtTheNinetySixthSurplusOutput();
        testResetWithTooSmallDepthTripsTheWatchdog();
        std::cout << "DecodeOrderTimestamps tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "DecodeOrderTimestamps test failure: " << error.what() << '\n';
        return 1;
    }
}
