#pragma once

#include "core/Types.h"

#include <cstdint>
#include <span>

namespace backtrack {

enum class AudioSampleEncoding {
    Pcm,
    Float,
    Unsupported,
};

// Parsed essentials of a WAVEFORMATEX / WAVEFORMATEXTENSIBLE format blob.
struct AudioFormatInfo {
    AudioSampleEncoding encoding = AudioSampleEncoding::Unsupported;
    uint16_t bitsPerSample = 0;
    uint16_t channels = 0;
    uint32_t samplesPerSec = 0;
    uint16_t blockAlign = 0;

    bool operator==(const AudioFormatInfo&) const = default;
};

// Canonical ingest format for every audio packet that enters the recording or
// replay pipeline: stereo float32 at 48 kHz (the mux target rate). WASAPI
// reconnects (headset swap, default-device change) silently change the capture
// mix format; without canonicalization the per-track WAV writers would keep
// appending raw bytes declared in the first packet's format, producing
// pitch-shifted audio and timeline skew. Converting at ingest fixes the
// recording and replay paths in one place. Equality with the mux constants is
// asserted in AudioConvert.cpp so the two cannot drift apart.
inline constexpr uint32_t kCanonicalAudioSampleRate = 48000;
inline constexpr uint16_t kCanonicalAudioChannels = 2;
inline constexpr uint16_t kCanonicalAudioBitsPerSample = 32;

AudioFormatInfo parseAudioFormat(const WaveFormatBlob& blob);

bool audioFormatUsable(const AudioFormatInfo& format);

// Decodes one interleaved sample as a normalized float in [-1, 1].
float decodeInterleavedSample(const uint8_t* sample, const AudioFormatInfo& info);

WaveFormatBlob canonicalAudioFormat();

// True when info describes exactly the canonical ingest format.
bool isCanonicalAudioFormat(const AudioFormatInfo& info);

// Converts a captured packet to the canonical format in place: any supported
// PCM/float bit depth and channel count is resampled (linear interpolation)
// to stereo float32 at kCanonicalAudioSampleRate. PTS is left untouched; it
// stays the authoritative timeline anchor. Returns false only when the
// packet's format cannot be interpreted; callers must then drop the packet
// instead of writing its bytes into a differently formatted stream.
bool convertAudioPacketToCanonical(AudioPacket& packet);

// Pure gain/mute for canonical float samples (post-canonicalization only).
// No format lookup, no clamp of the gain itself: sanitizeSettings owns the
// 0-200% range, so gain is percent/100 exactly. Muted zeroes, gain==1 is a
// no-op, otherwise multiply and clamp to [-1, 1]. No allocation.
void applyMuteGain(std::span<float> samples, float gain, bool muted);

} // namespace backtrack
