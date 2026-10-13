#pragma once

// Audio-writer helpers for RecorderController::writeAudioPacket: canonical
// gain, microphone AGC, and (behind BACKTRACK_HAS_NOISE_SUPPRESSION) RNNoise
// suppression. Pure packet functions plus the AGC envelope; all run on the
// audio writer thread. Mirrors, queues, and scratch buffers stay in
// RecorderController.

#include "audio/AudioConvert.h"
#include "core/Types.h"

#include <cstddef>
#include <vector>

namespace backtrack {

#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
class RnnoiseDenoiser;
#endif

void applyCanonicalGain(AudioPacket& packet, float gain);

// Voice-oriented automatic gain control. Keeps a slow level envelope and
// drives a smoothed gain so average speech lands near a target RMS. Slow
// attack/release prevents pumping on individual words.
class MicrophoneAgc {
public:
    void reset();
    // Advances the envelopes with this block's RMS (dBFS) and duration and
    // returns the current smoothed linear gain to apply to the block.
    float gainFor(float rmsDb, float blockSeconds);

private:
    float linearGain() const;

    float envelopeDb_ = -90.0f;
    float gainDb_ = 0.0f;
};

struct BlockLevels {
    float rmsDb = -90.0f;
    float peakDb = -90.0f;
};

BlockLevels measureBlockLevels(const AudioPacket& packet, const AudioFormatInfo& format);
void applyMicrophoneAgc(AudioPacket& packet, const AudioFormatInfo& format, MicrophoneAgc& agc);

#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
size_t applyMicrophoneNoiseSuppression(
    AudioPacket& packet,
    const AudioFormatInfo& format,
    RnnoiseDenoiser& denoiser,
    std::vector<float>& scratchMonoIn,
    std::vector<float>& scratchMonoOut);
#endif

} // namespace backtrack
