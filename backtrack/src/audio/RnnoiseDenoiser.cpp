#include "audio/RnnoiseDenoiser.h"

#include <rnnoise.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace backtrack {
namespace {

// RNNoise consumes 480-sample frames at 48 kHz (10 ms).
constexpr std::size_t kFrameSamples = 480;
constexpr float kSampleRate = 48000.0f;

// Conditioning target: inside the level range where the model demonstrably
// suppresses well and above its silence gate. See third_party/rnnoise/README.md.
constexpr float kTargetRms = 0.25f;
constexpr float kMinGain = 1.0f / 32.0f;
constexpr float kMaxGain = 64.0f;

// Envelope/gain smoothing time constants. Slow enough to avoid pumping, fast
// enough to follow a mic that changes level between game sessions.
constexpr float kLevelTauSeconds = 0.25f;
constexpr float kGainTauSeconds = 0.15f;

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
    if (state_) {
        // rnnoise_init re-creates the recurrent state in place; it cannot fail
        // here because the same embedded model already initialized once.
        (void)rnnoise_init(state_.get(), nullptr);
    }
}

void RnnoiseDenoiser::processFrame(float* frame) noexcept {
    const float gain = gain_;
    for (std::size_t i = 0; i < kFrameSamples; ++i) {
        frame[i] *= gain;
    }
    // In-place is safe: rnnoise_process_frame reads the input into internal
    // buffers before writing the output.
    rnnoise_process_frame(state_.get(), frame, frame);
    const float inverseGain = 1.0f / gain;
    for (std::size_t i = 0; i < kFrameSamples; ++i) {
        frame[i] *= inverseGain;
    }
}

std::size_t RnnoiseDenoiser::process(const float* in, std::size_t count, float* out, std::size_t outCapacity) {
    if (!valid() || count == 0 || outCapacity == 0) {
        return 0;
    }
    pending_.insert(pending_.end(), in, in + count);

    // Level conditioning envelope. The first block after a reset initializes
    // the estimate and the gain directly so the stream does not start with an
    // arbitrary ramp from unity.
    const float measuredRms = blockRms(in, count);
    const float blockSeconds = static_cast<float>(count) / kSampleRate;
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
    return written;
}

} // namespace backtrack
