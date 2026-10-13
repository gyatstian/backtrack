#include "app/AudioWriter.h"

#include "core/Logger.h"
#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
#include "audio/RnnoiseDenoiser.h"
#endif

#include <algorithm>
#include <cmath>
#include <span>

namespace backtrack {

// Canonical float gain helper for the mic AGC path: packets are canonical by
// the time AGC/volume run, so integer PCM scaling is dead. AudioConvert owns
// the pure mute/gain primitive; this thin wrapper reuses it.
void applyCanonicalGain(AudioPacket& packet, float gain) {
    if (gain == 1.0f || packet.bytes.empty()) {
        return;
    }
    auto* floats = reinterpret_cast<float*>(packet.bytes.data());
    applyMuteGain(
        std::span<float>(floats, packet.bytes.size() / sizeof(float)), gain, false);
}

constexpr float kAgcFloorDb = -90.0f;

void MicrophoneAgc::reset() {
    envelopeDb_ = -90.0f;
    gainDb_ = 0.0f;
}

float MicrophoneAgc::gainFor(float rmsDb, float blockSeconds) {
    const float levelDb = std::max(rmsDb, -90.0f);
    if (blockSeconds <= 0.0f) {
        return linearGain();
    }

    // Fast attack on loud content, slow release when the mic goes quiet.
    constexpr float kAttackTauSeconds = 0.03f;
    constexpr float kReleaseTauSeconds = 0.40f;
    const float envelopeTau = levelDb > envelopeDb_ ? kAttackTauSeconds : kReleaseTauSeconds;
    const float envelopeCoeff = 1.0f - std::exp(-blockSeconds / envelopeTau);
    envelopeDb_ += (levelDb - envelopeDb_) * envelopeCoeff;

    // Bring the envelope to the target speech RMS. Boost up to +30 dB (the
    // same range as a hardware "Microphone Boost"); only cut very loud mics.
    constexpr float kTargetRmsDb = -22.0f;
    constexpr float kMaxBoostDb = 30.0f;
    constexpr float kMaxCutDb = 6.0f;
    const float desiredGainDb = std::clamp(kTargetRmsDb - envelopeDb_, -kMaxCutDb, kMaxBoostDb);

    // Smooth the gain itself so level changes do not pump on single words.
    constexpr float kGainTauSeconds = 0.15f;
    const float gainCoeff = 1.0f - std::exp(-blockSeconds / kGainTauSeconds);
    gainDb_ += (desiredGainDb - gainDb_) * gainCoeff;
    return linearGain();
}

float MicrophoneAgc::linearGain() const {
    return std::pow(10.0f, gainDb_ / 20.0f);
}

BlockLevels measureBlockLevels(const AudioPacket& packet, const AudioFormatInfo& format) {
    BlockLevels levels;
    const uint16_t bytesPerSample = format.bitsPerSample / 8;
    if (bytesPerSample == 0 || packet.bytes.empty()) {
        return levels;
    }

    // Intentional RMS pool in double: float accumulation loses quiet-mic
    // resolution over long blocks; the sum itself never allocates.
    uint64_t sampleCount = 0;
    double sumOfSquares = 0.0;
    float peak = 0.0f;
    const uint8_t* data = packet.bytes.data();
    const size_t byteCount = packet.bytes.size();
    for (size_t offset = 0; offset + bytesPerSample <= byteCount; offset += bytesPerSample) {
        // Single decode path (8/16/24/32 + float32) owned by AudioConvert;
        // no hand-rolled integer magic here.
        const float value = decodeInterleavedSample(data + offset, format);
        const float magnitude = std::fabs(value);
        peak = std::max(peak, magnitude);
        sumOfSquares += static_cast<double>(value) * value;
        ++sampleCount;
    }
    if (sampleCount == 0) {
        return levels;
    }
    const double rms = std::sqrt(sumOfSquares / static_cast<double>(sampleCount));
    levels.rmsDb = rms > 0.0 ? static_cast<float>(20.0 * std::log10(rms)) : kAgcFloorDb;
    levels.peakDb = peak > 0.0f ? 20.0f * std::log10(peak) : kAgcFloorDb;
    return levels;
}

void applyMicrophoneAgc(AudioPacket& packet, const AudioFormatInfo& format, MicrophoneAgc& agc) {
    const uint16_t bytesPerSample = format.bitsPerSample / 8;
    const float blockSeconds =
        bytesPerSample > 0 && format.channels > 0 && format.samplesPerSec > 0
            ? static_cast<float>(packet.bytes.size()) /
                  (static_cast<float>(bytesPerSample) * static_cast<float>(format.channels) *
                   static_cast<float>(format.samplesPerSec))
            : 0.010f;
    const BlockLevels levels = measureBlockLevels(packet, format);
    const float gain = agc.gainFor(levels.rmsDb, blockSeconds);
    // Packets are canonical float32 by the time AGC runs, so the old integer
    // PCM scaling is dead; AudioConvert owns the single gain primitive.
    applyCanonicalGain(packet, gain);
}

#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
// Noise suppression runs after canonicalization, so the packet is guaranteed
// stereo float32 at 48 kHz; the guard keeps an unexpected format from being
// misinterpreted. The stereo mic signal is folded to mono (channels are
// near-identical for voice capture), denoised, and written back to both
// channels. RNNoise consumes 480-sample frames, so a call may emit up to one
// buffered frame more or fewer samples than it consumed: the helper resizes
// the packet payload to match what was actually produced, and held-back
// samples arrive in a later packet, keeping cumulative stream length exact.
// Returns the produced mono-sample count.
size_t applyMicrophoneNoiseSuppression(
    AudioPacket& packet,
    const AudioFormatInfo& format,
    RnnoiseDenoiser& denoiser,
    std::vector<float>& scratchMonoIn,
    std::vector<float>& scratchMonoOut) {
    const size_t frameCount = packet.bytes.size() / (2 * sizeof(float));
    if (!isCanonicalAudioFormat(format) || !denoiser.valid() || frameCount == 0) {
        static bool loggedUnsupportedOnce = false;
        if (!loggedUnsupportedOnce && !isCanonicalAudioFormat(format)) {
            loggedUnsupportedOnce = true;
            Logger::instance().warning(L"recorder", L"Microphone noise suppression skipped non-canonical audio packet");
        }
        return frameCount; // Bypass: everything "produced" unprocessed.
    }

    scratchMonoIn.resize(frameCount);
    const float* interleaved = reinterpret_cast<const float*>(packet.bytes.data());
    for (size_t frame = 0; frame < frameCount; ++frame) {
        // Average the channels: mono sources duplicate to L/R in canonical
        // conversion, so averaging avoids a spurious +6 dB level shift.
        scratchMonoIn[frame] = 0.5f * (interleaved[2 * frame] + interleaved[2 * frame + 1]);
    }

    // +480 lets the denoiser flush any frame buffered by an earlier odd-sized
    // packet instead of accumulating backlog.
    scratchMonoOut.resize(frameCount + 480);
    const size_t produced =
        denoiser.process(scratchMonoIn.data(), frameCount, scratchMonoOut.data(), scratchMonoOut.size());

    packet.bytes.resize(produced * 2 * sizeof(float));
    float* out = reinterpret_cast<float*>(packet.bytes.data());
    for (size_t frame = 0; frame < produced; ++frame) {
        out[2 * frame] = scratchMonoOut[frame];
        out[2 * frame + 1] = scratchMonoOut[frame];
    }
    return produced;
}
#endif // BACKTRACK_HAS_NOISE_SUPPRESSION

} // namespace backtrack
