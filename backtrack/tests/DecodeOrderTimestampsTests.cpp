#include "encoder/DecodeOrderTimestamps.h"
#include "mux/MuxedInputs.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using backtrack::DecodeOrderTimestamps;

constexpr int64_t kFrameDuration100ns = 100'000;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

int64_t submittedPts(int index) {
    return static_cast<int64_t>(index) * kFrameDuration100ns + (index % 4) * 3'000;
}

// Cumulative PTS for a submission stream whose durations cycle through
// `durations`.
int64_t cumulativePts(int index, const int64_t* durations, int period) {
    int64_t pts = 0;
    for (int i = 0; i < index; ++i) {
        pts += durations[i % period];
    }
    return pts;
}

// Presentation order 0,1,2,3,4,5,6,... becomes decode order 0,3,1,2,6,4,5,...
int presentationIndex(int outputIndex) {
    if (outputIndex == 0) {
        return 0;
    }
    const int group = (outputIndex - 1) / 3;
    const int position = (outputIndex - 1) % 3;
    return group * 3 + (position == 0 ? 3 : position);
}

void testDtsEqualsPtsWithoutReordering() {
    DecodeOrderTimestamps dts(0);
    int64_t previous = (std::numeric_limits<int64_t>::min)();

    for (int k = 0; k < 500; ++k) {
        const int64_t pts = submittedPts(k);
        dts.onSubmit(pts);
        const int64_t decoded = dts.onOutput(pts, kFrameDuration100ns);
        expect(decoded == pts,
            "without reordering the decode timestamp must equal the presentation timestamp");
        expect(decoded > previous, "the decode timestamp sequence is not strictly monotonic");
        previous = decoded;
    }

    dts.reset(0);
    for (int k = 0; k < 500; ++k) {
        dts.onSubmit(submittedPts(k));
        if (k >= 3) {
            const int64_t pts = submittedPts(k - 3);
            expect(dts.onOutput(pts, kFrameDuration100ns) == pts,
                "an unreordered in-flight backlog changed the decode slot");
        }
    }
    for (int k = 497; k < 500; ++k) {
        const int64_t pts = submittedPts(k);
        expect(dts.onOutput(pts, kFrameDuration100ns) == pts,
            "an unreordered drain changed the decode slot");
    }
}

void testReorderedBacklogAndFullDrain() {
    constexpr uint32_t kReorderDelay = 2;
    constexpr int kFrameCount = 301;
    constexpr int kBatchSize = 3;
    constexpr int kBacklogs[] = {2, 3};

    for (const int backlog : kBacklogs) {
        DecodeOrderTimestamps dts(kReorderDelay);
        std::vector<int64_t> decoded;
        int submitted = 0;

        const auto submit = [&] {
            dts.onSubmit(submittedPts(submitted++));
        };
        const auto harvest = [&] {
            const int frame = presentationIndex(static_cast<int>(decoded.size()));
            expect(frame < submitted, "an output referenced a frame not yet submitted");
            decoded.push_back(dts.onOutput(submittedPts(frame), kFrameDuration100ns));
        };

        for (int i = 0; i < 4 + backlog; ++i) {
            submit();
        }
        for (int i = 0; i < 4; ++i) {
            harvest();
        }

        while (submitted + kBatchSize <= kFrameCount) {
            expect(submitted - static_cast<int>(decoded.size()) == backlog,
                "the streaming backlog did not remain in flight");
            for (int i = 0; i < kBatchSize; ++i) {
                submit();
            }
            for (int i = 0; i < kBatchSize; ++i) {
                harvest();
            }
        }
        while (submitted < kFrameCount) {
            submit();
        }
        const std::size_t beforeDrain = decoded.size();
        expect(beforeDrain > 96 && submitted - static_cast<int>(beforeDrain) >= backlog,
            "the sustained backlog was not present before drain");
        while (decoded.size() < static_cast<std::size_t>(submitted)) {
            harvest();
        }
        expect(decoded.size() == static_cast<std::size_t>(kFrameCount) && decoded.size() > beforeDrain,
            "the full drain did not return every outstanding output");

        // NVENC can resume the same session after an EOS drain.
        for (int i = 0; i < kBatchSize; ++i) {
            submit();
        }
        for (int i = 0; i < kBatchSize; ++i) {
            harvest();
        }
        expect(decoded.size() == static_cast<std::size_t>(submitted),
            "the resumed session did not return every output");

        const int64_t timeBase = decoded.front();
        int64_t previousNormalized = -1;
        int64_t previousPts = submittedPts(0);
        bool sawPtsRegression = false;
        for (std::size_t i = 0; i < decoded.size(); ++i) {
            const int64_t pts = submittedPts(presentationIndex(static_cast<int>(i)));
            const int64_t effective = backtrack::effectiveDts(pts, decoded[i], true);
            const int64_t normalized = (std::max)(effective - timeBase, int64_t{0});
            expect(normalized > previousNormalized,
                "normalized DTS did not increase with backlog " + std::to_string(backlog) +
                " at output " + std::to_string(i));
            sawPtsRegression = sawPtsRegression || pts < previousPts;
            previousPts = pts;
            previousNormalized = normalized;
        }
        expect(sawPtsRegression, "the test did not exercise reordered presentation timestamps");

        for (std::size_t i = 0; i < decoded.size(); ++i) {
            const int slot = static_cast<int>(i) - static_cast<int>(kReorderDelay);
            const int64_t expected = slot < 0
                ? static_cast<int64_t>(slot) * kFrameDuration100ns
                : submittedPts(slot);
            const int64_t pts = submittedPts(presentationIndex(static_cast<int>(i)));
            expect(decoded[i] == expected,
                "decode slot changed with backlog " + std::to_string(backlog) +
                " at output " + std::to_string(i));
            expect(backtrack::effectiveDts(pts, decoded[i], true) == decoded[i],
                "a decode timestamp exceeded its presentation timestamp");
        }
    }
}

void testResetClearsPendingSlotsAndRestartsTimeline() {
    DecodeOrderTimestamps dts(2);
    dts.onSubmit(9'000'000);
    dts.onSubmit(9'100'000);
    expect(dts.onOutput(9'000'000, kFrameDuration100ns) == 8'800'000,
        "the initial session did not extrapolate its first decode slot");

    dts.reset(2);
    dts.onSubmit(0);
    expect(dts.onOutput(0, kFrameDuration100ns) == -200'000,
        "reset did not discard the old pending timestamps");
    dts.onSubmit(100'000);
    expect(dts.onOutput(100'000, kFrameDuration100ns) == -100'000,
        "reset did not restart the initial decode slots");
    dts.onSubmit(200'000);
    expect(dts.onOutput(200'000, kFrameDuration100ns) == 0,
        "the new session did not consume its first submission slot");

    dts.reset(0);
    dts.onSubmit(-100'000);
    expect(dts.onOutput(-100'000, kFrameDuration100ns) == -100'000,
        "reset did not apply the new depth or preserve a negative timestamp");
}

void testVariableFrameDurationsKeepExtrapolatedDtsMonotone() {
    constexpr uint32_t kReorderDelay = 2;
    // one then four nominal frames: the cadence collapse that used to run the
    // extrapolated slots backwards
    const int64_t durations[] = {kFrameDuration100ns, kFrameDuration100ns, 4 * kFrameDuration100ns};
    constexpr int kPeriod = 3;
    constexpr int kFrameCount = 40;

    DecodeOrderTimestamps dts(kReorderDelay);
    std::vector<int64_t> decoded;

    for (int k = 0; k < kFrameCount; ++k) {
        const int64_t pts = cumulativePts(k, durations, kPeriod);
        dts.onSubmit(pts, durations[k % kPeriod]);
        // the arriving packet reports a duration that is not its own slot's
        decoded.push_back(dts.onOutput(pts, durations[(2 * k) % kPeriod]));
    }

    const int64_t firstSlotDuration = durations[0];
    int64_t previous = (std::numeric_limits<int64_t>::min)();
    for (int i = 0; i < kFrameCount; ++i) {
        const int slot = i - static_cast<int>(kReorderDelay);
        const int64_t expected = slot < 0
            ? -static_cast<int64_t>(-slot) * firstSlotDuration
            : cumulativePts(slot, durations, kPeriod);
        const int64_t decodedAt = decoded[static_cast<std::size_t>(i)];
        expect(decodedAt == expected,
            "a variable-duration slot changed the documented formula at output " + std::to_string(i));
        expect(decodedAt > previous,
            "the variable-duration decode timestamp sequence is not strictly increasing at output " +
                std::to_string(i));
        previous = decodedAt;
    }

    // the per-slot duration comes from the earliest submission, not from the
    // nominal frame duration the mux would otherwise assume
    const int64_t longFirst[] = {4 * kFrameDuration100ns, kFrameDuration100ns, kFrameDuration100ns};
    DecodeOrderTimestamps longFirstDts(kReorderDelay);
    longFirstDts.onSubmit(0, longFirst[0]);
    longFirstDts.onSubmit(longFirst[0], longFirst[1]);
    longFirstDts.onSubmit(longFirst[0] + longFirst[1], longFirst[2]);
    expect(longFirstDts.onOutput(0, kFrameDuration100ns) == -8 * kFrameDuration100ns,
        "the extrapolation ignored the earliest submission's duration");
    expect(longFirstDts.onOutput(longFirst[0], kFrameDuration100ns) == -4 * kFrameDuration100ns,
        "the extrapolation did not keep one stable per-slot duration");
    expect(longFirstDts.onOutput(longFirst[1], kFrameDuration100ns) == 0,
        "the extrapolated slots did not lead into the first submission");
}

} // namespace

int main() {
    try {
        testReorderedBacklogAndFullDrain();
        testDtsEqualsPtsWithoutReordering();
        testResetClearsPendingSlotsAndRestartsTimeline();
        testVariableFrameDurationsKeepExtrapolatedDtsMonotone();
        std::cout << "DecodeOrderTimestamps tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "DecodeOrderTimestamps test failure: " << error.what() << '\n';
        return 1;
    }
}
