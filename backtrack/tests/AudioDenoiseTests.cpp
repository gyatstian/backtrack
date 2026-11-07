#include "audio/RnnoiseDenoiser.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using backtrack::RnnoiseDenoiser;

// Matches the recommended contract: output capacity = input size + one frame,
// so buffered backlog from earlier odd-sized calls drains immediately.
constexpr std::size_t kOutputHeadroom = 480;

// Pacing math mirrors the capture thread and WAV pacer at the canonical rate.
constexpr int64_t kHnsPerSecond = 10000000;
constexpr int64_t kSampleRate = 48000;

int64_t sampleDuration100ns(std::size_t samples) {
    return static_cast<int64_t>(samples) * kHnsPerSecond / kSampleRate;
}

// Feeds one packet with the given input PTS, advances the caller's input PTS
// by the packet duration exactly as the WASAPI capture thread accumulates it,
// and reports the PTS the denoiser stamped on the emitted block.
std::size_t processChunk(RnnoiseDenoiser& denoiser, const std::vector<float>& in,
                         std::vector<float>& out, int64_t& nextInputPts, int64_t& outPts) {
    out.resize(in.size() + kOutputHeadroom);
    const std::size_t produced =
        denoiser.process(in.data(), in.size(), out.data(), out.size(), nextInputPts, outPts);
    nextInputPts += sampleDuration100ns(in.size());
    return produced;
}

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

unsigned int lcgState = 42;

float noiseSample() {
    lcgState = lcgState * 1103515245u + 12345u;
    return static_cast<float>(
        (static_cast<double>(lcgState >> 16 & 0x7fff) / 32767.0 * 2.0 - 1.0));
}

void fillNoise(std::vector<float>& samples, float amplitude) {
    for (float& sample : samples) {
        sample = amplitude * noiseSample();
    }
}

double rms(const std::vector<float>& samples, std::size_t count) {
    double sumOfSquares = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        sumOfSquares += static_cast<double>(samples[i]) * samples[i];
    }
    return std::sqrt(sumOfSquares / static_cast<double>(count));
}

void assertAllFinite(const std::vector<float>& samples, std::size_t count, const char* message) {
    for (std::size_t i = 0; i < count; ++i) {
        if (!std::isfinite(samples[i])) {
            throw std::runtime_error(message);
        }
    }
}

void testValidAndSilencePreservation() {
    RnnoiseDenoiser denoiser;
    expect(denoiser.valid(), "RNNoise state should initialize with the embedded model");

    // Digital silence must map to digital silence in any chunking, including
    // across packet boundaries where sub-frame tails are carried over.
    // Regression guard: this exact pattern once overflowed the caller's buffer
    // by emitting more frames than the output capacity allowed.
    std::vector<float> zeros(1447, 0.0f); // 3 frames + 7 leftover
    const std::vector<std::size_t> chunkSizes = {1000, 1, 446};
    std::vector<float> out;
    std::size_t offset = 0;
    int64_t inputPts = 0;
    for (const std::size_t chunk : chunkSizes) {
        const std::vector<float> in(zeros.begin() + offset, zeros.begin() + offset + chunk);
        offset += chunk;
        int64_t outPts = 0;
        const std::size_t produced = processChunk(denoiser, in, out, inputPts, outPts);
        expect(produced <= out.size(), "process must never write past its output capacity");
        expect(produced % 480 == 0, "output must be whole frames");
        for (std::size_t i = 0; i < produced; ++i) {
            expect(out[i] == 0.0f, "silence must stay exactly silent");
        }
    }
    expect(offset == zeros.size(), "chunk sizes must cover the buffer");

    // Zero-count calls are no-ops.
    std::vector<float> empty;
    int64_t emptyPts = 0;
    expect(processChunk(denoiser, empty, out, emptyPts, emptyPts) == 0,
           "empty call must produce nothing");
}

void testStreamConservationAcrossOddChunks() {
    // Cumulative output length must converge to cumulative input length minus
    // a sub-frame remainder: no sample dropped or duplicated whatever the
    // packet boundaries are.
    RnnoiseDenoiser denoiser;
    std::vector<float> samples(300 * 480 + 137);
    fillNoise(samples, 0.05f);

    const std::vector<std::size_t> chunkSizes = {44100, 1024, 997, 480, 479, 481, 12345};
    std::size_t received = 0;
    std::size_t producedTotal = 0;
    std::vector<float> out;
    int64_t inputPts = 0;
    for (const std::size_t chunk : chunkSizes) {
        if (received >= samples.size()) {
            break;
        }
        const std::size_t take = std::min(chunk, samples.size() - received);
        const std::vector<float> in(samples.begin() + received, samples.begin() + received + take);
        int64_t outPts = 0;
        producedTotal += processChunk(denoiser, in, out, inputPts, outPts);
        assertAllFinite(out, producedTotal > out.size() ? out.size() : producedTotal,
                        "chunked processing produced non-finite samples");
        received += take;
    }
    if (received < samples.size()) {
        const std::vector<float> in(samples.begin() + received, samples.end());
        int64_t outPts = 0;
        producedTotal += processChunk(denoiser, in, out, inputPts, outPts);
        received = samples.size();
    }

    expect(received == samples.size(), "every input sample must be consumed");
    expect(producedTotal <= received, "output can never exceed input");
    expect(received - producedTotal < 480, "at most a sub-frame remainder may stay buffered");
}

void testResetRestoresFreshBehavior() {
    std::vector<float> input(5 * 480);
    fillNoise(input, 0.05f);

    RnnoiseDenoiser reused;
    std::vector<float> firstOut;
    int64_t inputPts = 0;
    int64_t outPts = 0;
    const std::size_t firstProduced = processChunk(reused, input, firstOut, inputPts, outPts);
    expect(firstProduced == input.size(),
           "frame-aligned call with headroom must produce everything");

    reused.reset();
    std::vector<float> secondOut;
    const std::size_t secondProduced = processChunk(reused, input, secondOut, inputPts, outPts);
    expect(secondProduced == firstProduced, "post-reset run must produce the same amount");
    firstOut.resize(secondProduced);
    secondOut.resize(secondProduced);
    expect(secondOut == firstOut,
           "reset() must reproduce a fresh instance's output bit-for-bit");
    // Pure noise has no speech to keep, so the real model returns it ~-50 dB
    // down (measured 1.6e-4 from 5e-2). Only a stub that emits silence gives
    // exactly zero, and that is what this guard has to catch.
    expect(rms(firstOut, firstProduced) > 0.0,
           "test lost sensitivity: denoiser appears to be a no-op");
}

void testSuppressesNoiseAtQuietAndNormalLevels() {
    // Conditioning (gain up to the 64x cap, already reached at 0.005) plus the
    // PCM16 boundary conversion must keep the model in its trained feature
    // range, so suppression stays level-independent. This is a regression that
    // the units change did not break suppression, not a proof of audible
    // quality: the model's improvement from correct units is a listening check.
    for (const float amplitude : {0.05f, 0.005f}) {
        RnnoiseDenoiser denoiser;
        expect(denoiser.valid(), "RNNoise state should initialize");

        std::vector<float> warmup(200 * 480);
        fillNoise(warmup, amplitude);
        std::vector<float> warmupOut;
        int64_t inputPts = 0;
        int64_t outPts = 0;
        const std::size_t warmupProduced = processChunk(denoiser, warmup, warmupOut, inputPts, outPts);
        expect(warmupProduced == warmup.size(), "frame-aligned call must be fully produced");
        assertAllFinite(warmupOut, warmupProduced, "warm-up produced non-finite samples");
        expect(rms(warmupOut, warmupProduced) > 0.0, "warm-up output must carry energy");

        std::vector<float> measured(200 * 480);
        fillNoise(measured, amplitude);
        const double inputLevel = rms(measured, measured.size());
        std::vector<float> measuredOut;
        const std::size_t produced = processChunk(denoiser, measured, measuredOut, inputPts, outPts);
        expect(produced == measured.size(), "frame-aligned call must be fully produced");
        assertAllFinite(measuredOut, produced, "denoiser produced non-finite samples");
        const double outputLevel = rms(measuredOut, produced);

        expect(outputLevel < inputLevel * 0.8, "stationary noise must be attenuated");
    }

    // The reciprocal PCM16 conversion must bring the output back to normalized
    // units. A missing or wrong reciprocal would leave the output scaled by the
    // PCM16 factor (~32768x), which this bound catches deterministically.
    RnnoiseDenoiser loudDenoiser;
    std::vector<float> loud(10 * 480);
    fillNoise(loud, 1.0f);
    std::vector<float> loudOut;
    int64_t inputPts = 0;
    int64_t outPts = 0;
    const std::size_t loudProduced = processChunk(loudDenoiser, loud, loudOut, inputPts, outPts);
    expect(loudProduced == loud.size(), "frame-aligned call must be fully produced");
    assertAllFinite(loudOut, loudProduced, "full-scale processing produced non-finite samples");
    expect(rms(loudOut, loudProduced) < 1.0,
           "output must stay in normalized range after the PCM16 conversion");
}

void testPacedFragmentedPacketsStayContiguous() {
    // The finding: RNNoise emits whole 480-frame blocks but the output packet
    // kept the newest input packet's PTS, so the WAV pacer inserted silence
    // (or trimmed) at every fragment boundary. The emitted block must be
    // stamped with the OLDEST pending sample's position and advance by exactly
    // the emitted duration, so consecutive outputs stay contiguous on the
    // pacer's integer timeline.
    RnnoiseDenoiser denoiser;
    std::vector<float> out;
    const std::vector<std::size_t> fragments = {1000, 1000, 480, 479, 481, 1234, 137, 960};
    int64_t inputPts = 0;
    int64_t expectedOutPts = 0;
    std::size_t totalIn = 0;
    std::size_t totalOut = 0;
    for (const std::size_t fragment : fragments) {
        std::vector<float> in(fragment);
        fillNoise(in, 0.05f);
        int64_t outPts = -1;
        const std::size_t produced = processChunk(denoiser, in, out, inputPts, outPts);
        expect(outPts == expectedOutPts,
               "output must start at the oldest pending position, not the newest input PTS");
        expect(produced % 480 == 0, "output must be whole frames");
        expectedOutPts += sampleDuration100ns(produced);
        totalIn += fragment;
        totalOut += produced;
    }
    expect(totalOut <= totalIn, "output can never exceed input");
    expect(totalIn - totalOut < 480, "at most a sub-frame remainder may stay buffered");
    expect(denoiser.pendingSamples() == totalIn - totalOut,
           "pending count must match the remainder");
}

void testDiscontinuityResetRestartsTimeline() {
    // A real break (capture restart, dropped packet, device switch) jumps the
    // packet PTS. The denoiser must drop its stale pending tail and recurrent
    // state and restart the output timeline at the new packet's PTS, never
    // splicing old audio onto the new stream.
    RnnoiseDenoiser denoiser;
    std::vector<float> out;
    int64_t inputPts = 0;

    // 1000 samples: two full frames emitted at PTS 0, 40 left pending.
    std::vector<float> in(1000);
    fillNoise(in, 0.05f);
    int64_t outPts = -1;
    const std::size_t p1 = processChunk(denoiser, in, out, inputPts, outPts);
    expect(p1 == 960 && outPts == 0, "first run emits two frames at PTS 0");
    expect(denoiser.pendingSamples() == 40, "1000 samples leave a 40-sample tail");

    // 100 more samples, still contiguous: nothing emitted, tail grows to 140.
    in.assign(100, 0.0f);
    fillNoise(in, 0.05f);
    const std::size_t p2 = processChunk(denoiser, in, out, inputPts, outPts);
    expect(p2 == 0 && outPts == sampleDuration100ns(960),
           "sub-frame tail stays buffered at the pending position");
    expect(denoiser.pendingSamples() == 140, "contiguous run keeps the pending tail");

    // The stream restarts 300 ms past the accumulated position: reset drops
    // the 140 stale samples and restarts the output timeline at the new PTS.
    // streamPts is mutable because processChunk advances it to the next input
    // position, mirroring the capture thread.
    int64_t streamPts = inputPts + 3000000;
    const int64_t restartPts = streamPts;
    in.assign(100, 0.0f);
    fillNoise(in, 0.05f);
    const std::size_t p3 = processChunk(denoiser, in, out, streamPts, outPts);
    expect(p3 == 0 && outPts == restartPts,
           "discontinuous packet restarts the output timeline at its own PTS");
    expect(denoiser.pendingSamples() == 100, "discontinuity must drop the stale pending tail");

    // The resumed stream continues contiguously from the restart position.
    in.assign(400, 0.0f);
    fillNoise(in, 0.05f);
    const std::size_t p4 = processChunk(denoiser, in, out, streamPts, outPts);
    expect(p4 == 480 && outPts == restartPts,
           "resumed run emits one frame from the restart position");
    expect(denoiser.pendingSamples() == 20, "500 resumed samples leave a 20-sample tail");
}

} // namespace

int main() {
    try {
        testValidAndSilencePreservation();
        testStreamConservationAcrossOddChunks();
        testResetRestoresFreshBehavior();
        testSuppressesNoiseAtQuietAndNormalLevels();
        testPacedFragmentedPacketsStayContiguous();
        testDiscontinuityResetRestartsTimeline();
        std::cout << "AudioDenoise tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AudioDenoise test failure: " << error.what() << '\n';
        return 1;
    }
}
