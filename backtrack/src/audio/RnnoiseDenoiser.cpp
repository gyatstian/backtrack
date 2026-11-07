#include "audio/RnnoiseDenoiser.h"

#include "audio/AudioConvert.h"
#include <rnnoise.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace backtrack {
namespace {

// RNNoise consumes 480-sample frames at 48 kHz (10 ms).
constexpr std::size_t kFrameSamples = 480;

// RNNoise's model is trained on int16 samples cast to float (range ~+-32768).
// Backtrack hands it normalized floats, so the boundary converts by this
// exact power-of-two scale.
constexpr float kPcm16Scale = 32768.0f;

// Conditioning target: inside the level range where the model demonstrably
// suppresses well and above its silence gate. See third_party/rnnoise/README.md.
constexpr float kTargetRms = 0.25f;
constexpr float kMinGain = 1.0f / 32.0f;
constexpr float kMaxGain = 64.0f;

// Envelope/gain smoothing time constants. Slow enough to avoid pumping, fast
// enough to follow a mic that changes level between game sessions.
constexpr float kLevelTauSeconds = 0.25f;
constexpr float kGainTauSeconds = 0.15f;

// How far a packet's PTS may sit from the previous packet's end before the
// stream is treated as broken. Normal operation is exact (the capture thread
// accumulates the same integer durations), and canonical resampling adds at
// most ~0.1 ms of rounding per packet; a real break is at least one dropped
// packet (>= 1 ms) or a capture resync (>= 50 ms). 1 ms separates the two
// without ever firing on rounding.
constexpr int64_t kDiscontinuityTolerance100ns = 10000;

float blockRms(const float* samples, std::size_t count) {
    double sumOfSquares = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        sumOfSquares += static_cast<double>(samples[i]) * samples[i];
    }
    return static_cast<float>(std::sqrt(sumOfSquares / static_cast<double>(count)));
}

} // namespace

void RnnoiseDenoiser::StateDeleter::operator()(DenoiseState* state) const noexcept {
    rnnoise_destroy(state);
}

RnnoiseDenoiser::RnnoiseDenoiser()
    : state_(rnnoise_create(nullptr)) {
    frame_.resize(kFrameSamples);
    pending_.reserve(4096);
}

RnnoiseDenoiser::~RnnoiseDenoiser() = default;

void RnnoiseDenoiser::reset() {
    pending_.clear();
    levelRms_ = 0.0f;
    gain_ = 1.0f;
    envelopeInitialized_ = false;
    nextEmitPts100ns_ = 0;
    lastInputEndPts100ns_ = 0;
    haveStream_ = false;
    if (state_) {
        // rnnoise_init re-creates the recurrent state in place; it cannot fail
        // here because the same embedded model already initialized once.
        (void)rnnoise_init(state_.get(), nullptr);
    }
}

void RnnoiseDenoiser::processFrame(float* frame) noexcept {
    // Conditioning gain plus the PCM16 unit conversion in one scale; the exact
    // reciprocal restores the original normalized level afterwards. The gain
    // must ride inside the same scale so the model sees a -12 dBFS RMS signal
    // in the units it was trained on.
    const float scale = gain_ * kPcm16Scale;
    for (std::size_t i = 0; i < kFrameSamples; ++i) {
        frame[i] *= scale;
    }
    // In-place is safe: rnnoise_process_frame reads the input into internal
    // buffers before writing the output.
    rnnoise_process_frame(state_.get(), frame, frame);
    const float inverseScale = 1.0f / scale;
    for (std::size_t i = 0; i < kFrameSamples; ++i) {
        frame[i] *= inverseScale;
    }
}

std::size_t RnnoiseDenoiser::process(const float* in, std::size_t count, float* out, std::size_t outCapacity,
                                     int64_t inputPts100ns, int64_t& outPts100ns) {
    if (!valid() || count == 0 || outCapacity == 0) {
        outPts100ns = inputPts100ns;
        return 0;
    }

    // Timeline bookkeeping runs before buffering so a discontinuity drops the
    // stale tail instead of mixing it into the new stream. A break means
    // samples were genuinely lost upstream (dropped packet, device switch,
    // capture resync), so the sub-frame tail is stale: discard it and restart
    // the output timeline at the new packet's PTS. The pacer fills the gap with
    // silence, which is what the timeline should say.
    const int64_t duration100ns = static_cast<int64_t>(count) * kHundredNanosecondsPerSecond /
                                  static_cast<int64_t>(kCanonicalAudioSampleRate);
    if (!haveStream_) {
        nextEmitPts100ns_ = inputPts100ns;
    } else if (std::llabs(inputPts100ns - lastInputEndPts100ns_) > kDiscontinuityTolerance100ns) {
        reset();
        nextEmitPts100ns_ = inputPts100ns;
    }
    haveStream_ = true;
    lastInputEndPts100ns_ = inputPts100ns + duration100ns;
    outPts100ns = nextEmitPts100ns_;

    pending_.insert(pending_.end(), in, in + count);

    // Level conditioning envelope. The first block after a reset initializes
    // the estimate and the gain directly so the stream does not start with an
    // arbitrary ramp from unity.
    const float measuredRms = blockRms(in, count);
    const float blockSeconds = static_cast<float>(count) / static_cast<float>(kCanonicalAudioSampleRate);
    if (!envelopeInitialized_) {
        levelRms_ = measuredRms;
        envelopeInitialized_ = true;
        gain_ = std::clamp(kTargetRms / std::max(measuredRms, 1e-6f), kMinGain, kMaxGain);
    } else {
        const float levelCoeff =
            blockSeconds > 0.0f ? 1.0f - std::exp(-blockSeconds / kLevelTauSeconds) : 1.0f;
        levelRms_ += (measuredRms - levelRms_) * levelCoeff;

        const float desiredGain =
            std::clamp(kTargetRms / std::max(levelRms_, 1e-6f), kMinGain, kMaxGain);
        const float gainCoeff =
            blockSeconds > 0.0f ? 1.0f - std::exp(-blockSeconds / kGainTauSeconds) : 1.0f;
        gain_ += (desiredGain - gain_) * gainCoeff;
    }

    // Consume whole frames; the caller's output capacity bounds how many can
    // be emitted this call, and everything else stays buffered for later.
    const std::size_t available = pending_.size();
    const std::size_t readyFrames = available / kFrameSamples;
    const std::size_t writableFrames = outCapacity / kFrameSamples;
    const std::size_t producedFrames = std::min(readyFrames, writableFrames);

    std::size_t consumed = 0;
    std::size_t written = 0;
    for (std::size_t frame = 0; frame < producedFrames; ++frame) {
        std::memcpy(frame_.data(), pending_.data() + consumed, kFrameSamples * sizeof(float));
        processFrame(frame_.data());
        std::memcpy(out + written, frame_.data(), kFrameSamples * sizeof(float));
        consumed += kFrameSamples;
        written += kFrameSamples;
    }
    if (consumed > 0) {
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(consumed));
    }

    // Advance the output position by the emitted samples. Exact: each emitted
    // block is a whole number of 480-sample frames (10 ms each), so this
    // matches the pacer's integer arithmetic and cannot drift.
    nextEmitPts100ns_ += static_cast<int64_t>(written) * kHundredNanosecondsPerSecond /
                         static_cast<int64_t>(kCanonicalAudioSampleRate);
    return written;
}

} // namespace backtrack
