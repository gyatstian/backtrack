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

std::size_t processChunk(RnnoiseDenoiser& denoiser, const std::vector<float>& in,
                         std::vector<float>& out) {
    out.resize(in.size() + kOutputHeadroom);
    return denoiser.process(in.data(), in.size(), out.data(), out.size());
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
    for (const std::size_t chunk : chunkSizes) {
        const std::vector<float> in(zeros.begin() + offset, zeros.begin() + offset + chunk);
        offset += chunk;
        const std::size_t produced = processChunk(denoiser, in, out);
        expect(produced <= out.size(), "process must never write past its output capacity");
        expect(produced % 480 == 0, "output must be whole frames");
        for (std::size_t i = 0; i < produced; ++i) {
            expect(out[i] == 0.0f, "silence must stay exactly silent");
        }
    }
    expect(offset == zeros.size(), "chunk sizes must cover the buffer");

    // Zero-count calls are no-ops.
    std::vector<float> empty;
    expect(processChunk(denoiser, empty, out) == 0, "empty call must produce nothing");
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
    for (const std::size_t chunk : chunkSizes) {
        if (received >= samples.size()) {
            break;
        }
        const std::size_t take = std::min(chunk, samples.size() - received);
        const std::vector<float> in(samples.begin() + received, samples.begin() + received + take);
        producedTotal += processChunk(denoiser, in, out);
        assertAllFinite(out, producedTotal > out.size() ? out.size() : producedTotal,
                        "chunked processing produced non-finite samples");
        received += take;
    }
    if (received < samples.size()) {
        const std::vector<float> in(samples.begin() + received, samples.end());
        producedTotal += processChunk(denoiser, in, out);
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
    const std::size_t firstProduced = processChunk(reused, input, firstOut);
    expect(firstProduced == input.size(),
           "frame-aligned call with headroom must produce everything");

    reused.reset();
    std::vector<float> secondOut;
    const std::size_t secondProduced = processChunk(reused, input, secondOut);
    expect(secondProduced == firstProduced, "post-reset run must produce the same amount");
    firstOut.resize(secondProduced);
    secondOut.resize(secondProduced);
    expect(secondOut == firstOut,
           "reset() must reproduce a fresh instance's output bit-for-bit");
    expect(rms(firstOut, firstProduced) > 0.001,
           "test lost sensitivity: denoiser appears to be a no-op");
}

void testSuppressesNoiseAtQuietAndNormalLevels() {
    // Both levels sit well below RNNoise's own usable range without Backtrack's
    // input conditioning (the upstream silence gate bypasses anything quieter
    // than ~-9 dBFS RMS); conditioning must make suppression level-independent.
    for (const float amplitude : {0.05f, 0.005f}) {
        RnnoiseDenoiser denoiser;
        expect(denoiser.valid(), "RNNoise state should initialize");

        std::vector<float> warmup(200 * 480);
        fillNoise(warmup, amplitude);
        std::vector<float> warmupOut;
        const std::size_t warmupProduced = processChunk(denoiser, warmup, warmupOut);
        expect(warmupProduced == warmup.size(), "frame-aligned call must be fully produced");
        assertAllFinite(warmupOut, warmupProduced, "warm-up produced non-finite samples");
        expect(rms(warmupOut, warmupProduced) > 0.0, "warm-up output must carry energy");

        std::vector<float> measured(200 * 480);
        fillNoise(measured, amplitude);
        const double inputLevel = rms(measured, measured.size());
        std::vector<float> measuredOut;
        const std::size_t produced = processChunk(denoiser, measured, measuredOut);
        expect(produced == measured.size(), "frame-aligned call must be fully produced");
        assertAllFinite(measuredOut, produced, "denoiser produced non-finite samples");
        const double outputLevel = rms(measuredOut, produced);

        expect(outputLevel < inputLevel * 0.8, "stationary noise must be attenuated");
    }
}

} // namespace

int main() {
    try {
        testValidAndSilencePreservation();
        testStreamConservationAcrossOddChunks();
        testResetRestoresFreshBehavior();
        testSuppressesNoiseAtQuietAndNormalLevels();
        std::cout << "AudioDenoise tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AudioDenoise test failure: " << error.what() << '\n';
        return 1;
    }
}
