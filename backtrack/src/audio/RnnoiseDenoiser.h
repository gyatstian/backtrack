#pragma once

#include <cstddef>
#include <memory>
#include <vector>

// Forward declaration from third_party/rnnoise/include/rnnoise.h.
struct DenoiseState;

namespace backtrack {

// Real-time noise suppression for one microphone stream, wrapping the vendored
// RNNoise library (48 kHz mono float, 480-sample frames).
//
// The denoiser is intentionally not thread-safe: like MicrophoneAgc it lives on
// the recorder's audio writer thread only. It preserves the sample count
// exactly, so packet timing, timeline durations and replay sync are unaffected.
//
// Input conditioning: RNNoise's model (and its built-in silence gate) expect
// speech around -12..-9 dBFS RMS, while real microphone levels vary wildly.
// process() therefore applies a smoothed gain that steers the observed RMS
// toward a fixed target before the model and multiplies the exact reciprocal
// afterwards, so the audible signal level is unchanged and downstream stages
// (AGC, manual volume) see the same levels as before.
class RnnoiseDenoiser {
public:
    RnnoiseDenoiser();
    ~RnnoiseDenoiser();

    RnnoiseDenoiser(const RnnoiseDenoiser&) = delete;
    RnnoiseDenoiser& operator=(const RnnoiseDenoiser&) = delete;

    // False only when the RNNoise state could not be created (allocation
    // failure). Callers should treat the feature as unavailable for the whole
    // session instead of retrying.
    bool valid() const noexcept { return state_ != nullptr; }

    // Clears the pending-sample buffer, the level envelope, the conditioning
    // gain and the RNNoise recurrent state. Call when the capture stream
    // restarts or the feature is toggled, mirroring the AGC reset plumbing.
    void reset();

    // Denoises `count` contiguous mono samples from `in`, writing whole
    // 480-sample frames to `out` (at most `outCapacity` samples) and returning
    // how many were written. RNNoise works on 480-sample frames, so input
    // fragments that do not fill a frame are buffered and emitted by a later
    // call; conversely a call may emit up to one buffered frame MORE than it
    // consumed, which is why the output buffer must have headroom. Callers
    // should provide outCapacity >= count + 480: then any backlog carried from
    // earlier calls drains immediately instead of accumulating.
    // `in` and `out` must not overlap.
    std::size_t process(const float* in, std::size_t count, float* out, std::size_t outCapacity);

private:
    struct StateDeleter {
        void operator()(DenoiseState* state) const noexcept;
    };

    void processFrame(float* frame) noexcept;

    std::unique_ptr<DenoiseState, StateDeleter> state_;
    std::vector<float> pending_;
    std::vector<float> frame_;
    float levelRms_ = 0.0f;
    float gain_ = 1.0f;
    bool envelopeInitialized_ = false;
};

} // namespace backtrack
