#pragma once

#include <cstddef>
#include <cstdint>
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
// Input conditioning and units: RNNoise's model is trained on int16 samples
// cast to float (PCM16 scale, range ~+-32768) and expects speech around
// -12..-9 dBFS RMS, while Backtrack hands it normalized floats and real
// microphone levels vary wildly. process() scales each frame by a smoothed
// gain times 32768 before the model and by the exact reciprocal afterwards, so
// the model sees a -12 dBFS RMS signal in the units it was trained on while
// the audible level and downstream stages (AGC, manual volume) keep the
// original normalized level. The units matter because the model's features are
// nonlinear (log band energies), so a linear reciprocal cannot compensate for
// feeding it the wrong scale. At PCM16 scale the vendored silence gate (0.04
// or the local 1e-9 patch) only fires on true digital silence; the patch's
// original quiet-microphone motivation is moot, but both thresholds are
// equivalent here and the vendored file is out of scope.
//
// Timeline: RNNoise emits whole 480-sample frames, so sub-frame tails are
// buffered across packets. process() stamps outPts100ns with the position of
// the OLDEST still-pending sample (not the incoming packet's PTS), which keeps
// fragmented packets exactly contiguous on the WAV pacer's integer timeline
// instead of drifting by the buffered remainder. A packet whose PTS is not
// contiguous with the previous one (dropped packet, device switch, capture
// clock resync) is a discontinuity: the pending buffer and recurrent state are
// reset and the output timeline restarts at that packet's PTS, so stale audio
// is never spliced across the break.
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
    // gain, the RNNoise recurrent state and the output-timeline position.
    // Call when the capture stream restarts or the feature is toggled,
    // mirroring the AGC reset plumbing.
    void reset();

    // How many mono samples are buffered awaiting a full 480-sample frame
    // (0..479). The next process() output starts at the oldest one.
    std::size_t pendingSamples() const noexcept { return pending_.size(); }

    // Denoises `count` contiguous mono samples from `in`, writing whole
    // 480-sample frames to `out` (at most `outCapacity` samples) and returning
    // how many were written. RNNoise works on 480-sample frames, so input
    // fragments that do not fill a frame are buffered and emitted by a later
    // call; conversely a call may emit up to one buffered frame MORE than it
    // consumed, which is why the output buffer must have headroom. Callers
    // should provide outCapacity >= count + 480: then any backlog carried from
    // earlier calls drains immediately instead of accumulating.
    // `in` and `out` must not overlap.
    //
    // `inputPts100ns` is the PTS the caller's packet carries; `outPts100ns`
    // receives the PTS of the first emitted sample (see the class comment).
    // The two may name the same variable.
    std::size_t process(const float* in, std::size_t count, float* out, std::size_t outCapacity,
                        int64_t inputPts100ns, int64_t& outPts100ns);

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
    // Position of the oldest still-pending sample. haveStream_ is an explicit
    // flag (timestamps may legitimately be zero or negative, so no value works
    // as a sentinel); lastInputEndPts100ns_ tracks the input stream so a
    // non-contiguous packet can be detected. All three belong with the pending
    // buffer and are cleared by reset().
    int64_t nextEmitPts100ns_ = 0;
    int64_t lastInputEndPts100ns_ = 0;
    bool haveStream_ = false;
};

} // namespace backtrack
