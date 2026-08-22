#include "audio/AudioConvert.h"

#include "audio/AudioMix.h"
#include "core/Logger.h"

#include <mmreg.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

static_assert(backtrack::kCanonicalAudioSampleRate == backtrack::kMuxAudioSampleRate,
    "canonical ingest rate must match the MP4 mux audio rate");
static_assert(backtrack::kCanonicalAudioChannels == backtrack::kMuxAudioChannels,
    "canonical ingest channel count must match the MP4 mux audio layout");

namespace backtrack {

namespace {

int32_t readSigned24(const uint8_t* sample) {
    int32_t value = static_cast<int32_t>(sample[0]) |
                    (static_cast<int32_t>(sample[1]) << 8) |
                    (static_cast<int32_t>(sample[2]) << 16);
    if ((value & 0x00800000) != 0) {
        value |= static_cast<int32_t>(0xff000000);
    }
    return value;
}

bool isWaveExtensibleSubFormat(const GUID& guid, uint16_t tag) {
    static constexpr uint8_t kWaveSubFormatTail[8] = {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
    return guid.Data1 == tag &&
           guid.Data2 == 0 &&
           guid.Data3 == 0x0010 &&
           std::equal(std::begin(guid.Data4), std::end(guid.Data4), kWaveSubFormatTail);
}

const std::shared_ptr<const WaveFormatBlob>& canonicalAudioFormatPtr() {
    static const std::shared_ptr<const WaveFormatBlob> blob =
        std::make_shared<const WaveFormatBlob>(canonicalAudioFormat());
    return blob;
}

void logUnsupportedFormatOnce(const AudioPacket& packet) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) {
        const wchar_t* track = packet.track == AudioTrack::System
            ? L"system audio"
            : packet.track == AudioTrack::Microphone ? L"microphone" : L"audio";
        Logger::instance().warning(L"audio",
            std::wstring(L"Dropping ") + track +
            L" with an unsupported capture format; the track is skipped until the "
            L"device produces a PCM or float format again");
    }
}

} // namespace

WaveFormatBlob canonicalAudioFormat() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = kCanonicalAudioChannels;
    format.nSamplesPerSec = kCanonicalAudioSampleRate;
    format.wBitsPerSample = kCanonicalAudioBitsPerSample;
    format.nBlockAlign = static_cast<uint16_t>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    format.cbSize = 0;

    WaveFormatBlob blob;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&format);
    blob.bytes.assign(bytes, bytes + sizeof(format));
    return blob;
}

AudioFormatInfo parseAudioFormat(const WaveFormatBlob& blob) {
    AudioFormatInfo info;
    if (blob.bytes.size() < sizeof(WAVEFORMATEX)) {
        return info;
    }

    const auto* format = reinterpret_cast<const WAVEFORMATEX*>(blob.bytes.data());
    info.bitsPerSample = format->wBitsPerSample;
    info.channels = format->nChannels;
    info.samplesPerSec = format->nSamplesPerSec;
    info.blockAlign = format->nBlockAlign;
    if (format->wFormatTag == WAVE_FORMAT_PCM) {
        info.encoding = AudioSampleEncoding::Pcm;
    } else if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        info.encoding = AudioSampleEncoding::Float;
    } else if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && blob.bytes.size() >= sizeof(WAVEFORMATEXTENSIBLE)) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(blob.bytes.data());
        if (isWaveExtensibleSubFormat(extensible->SubFormat, WAVE_FORMAT_PCM)) {
            info.encoding = AudioSampleEncoding::Pcm;
        } else if (isWaveExtensibleSubFormat(extensible->SubFormat, WAVE_FORMAT_IEEE_FLOAT)) {
            info.encoding = AudioSampleEncoding::Float;
        }
    }
    return info;
}

bool audioFormatUsable(const AudioFormatInfo& format) {
    const uint16_t bytesPerSample = format.bitsPerSample / 8;
    return format.encoding != AudioSampleEncoding::Unsupported &&
           format.bitsPerSample > 0 &&
           format.bitsPerSample % 8 == 0 &&
           bytesPerSample > 0 &&
           format.channels > 0 &&
           format.samplesPerSec > 0 &&
           format.blockAlign >= static_cast<uint16_t>(format.channels * bytesPerSample);
}

float decodeInterleavedSample(const uint8_t* sample, const AudioFormatInfo& info) {
    if (info.encoding == AudioSampleEncoding::Float && info.bitsPerSample == 32) {
        float value = 0.0f;
        std::memcpy(&value, sample, sizeof(value));
        return std::clamp(value, -1.0f, 1.0f);
    }

    if (info.encoding != AudioSampleEncoding::Pcm) {
        return 0.0f;
    }

    switch (info.bitsPerSample) {
    case 8:
        return (static_cast<int32_t>(*sample) - 128) / 128.0f;
    case 16: {
        int16_t value = 0;
        std::memcpy(&value, sample, sizeof(value));
        return value / 32768.0f;
    }
    case 24:
        return readSigned24(sample) / 8388608.0f;
    case 32: {
        int32_t value = 0;
        std::memcpy(&value, sample, sizeof(value));
        return value / 2147483648.0f;
    }
    default:
        return 0.0f;
    }
}

bool isCanonicalAudioFormat(const AudioFormatInfo& info) {
    return info.encoding == AudioSampleEncoding::Float &&
           info.bitsPerSample == kCanonicalAudioBitsPerSample &&
           info.channels == kCanonicalAudioChannels &&
           info.samplesPerSec == kCanonicalAudioSampleRate;
}

namespace {

// Speaker position bits (dwChannelMask values from ksmedia.h, restated locally
// so this translation unit stays independent of kernel streaming headers).
constexpr uint32_t kSpeakerFrontLeft = 0x1;
constexpr uint32_t kSpeakerFrontRight = 0x2;
constexpr uint32_t kSpeakerFrontCenter = 0x4;
constexpr uint32_t kSpeakerLowFrequency = 0x8;
constexpr uint32_t kSpeakerBackLeft = 0x10;
constexpr uint32_t kSpeakerBackRight = 0x20;
constexpr uint32_t kSpeakerFrontLeftOfCenter = 0x40;
constexpr uint32_t kSpeakerFrontRightOfCenter = 0x80;
constexpr uint32_t kSpeakerBackCenter = 0x100;
constexpr uint32_t kSpeakerSideLeft = 0x400;
constexpr uint32_t kSpeakerSideRight = 0x800;

// Left/right weights for one source channel in a stereo downmix.
struct DownmixGain {
    float left = 0.0f;
    float right = 0.0f;
};

// Default Windows speaker ordering per channel count, used when the format
// blob carries no usable channel mask.
uint32_t defaultChannelMask(uint16_t channels) {
    switch (channels) {
    case 1: return kSpeakerFrontCenter;
    case 2: return kSpeakerFrontLeft | kSpeakerFrontRight;
    case 3: return kSpeakerFrontLeft | kSpeakerFrontRight | kSpeakerFrontCenter;
    case 4: return kSpeakerFrontLeft | kSpeakerFrontRight | kSpeakerBackLeft | kSpeakerBackRight;
    case 5: return kSpeakerFrontLeft | kSpeakerFrontRight | kSpeakerFrontCenter |
                 kSpeakerBackLeft | kSpeakerBackRight;
    case 6: return kSpeakerFrontLeft | kSpeakerFrontRight | kSpeakerFrontCenter |
                 kSpeakerLowFrequency | kSpeakerBackLeft | kSpeakerBackRight; // 5.1
    case 7: return defaultChannelMask(6) | kSpeakerBackCenter;
    case 8: return defaultChannelMask(6) | kSpeakerSideLeft | kSpeakerSideRight; // 7.1
    default: return 0;
    }
}

// A mask that disagrees with the channel count cannot be mapped onto the
// interleaved layout reliably; prefer the standard default over guessing.
uint32_t channelMaskForDownmix(const WaveFormatBlob& blob, uint16_t channels) {
    if (blob.bytes.size() >= sizeof(WAVEFORMATEXTENSIBLE)) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(blob.bytes.data());
        if (extensible->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            const uint32_t mask = extensible->dwChannelMask;
            if (mask != 0 && std::popcount(mask) == channels) {
                return mask;
            }
        }
    }
    return defaultChannelMask(channels);
}

// ITU-R BS.775-style stereo fold-down for >2-channel input: front left/right
// at unity, center and surround channels at -3 dB into their outputs, LFE
// omitted (standard practice keeps the subwoofer out of a stereo mix). Gains
// are normalized so full-scale input cannot clip after summing. Callers fall
// back to plain front-left/front-right selection when no gains come back.
std::vector<DownmixGain> surroundDownmixGains(const WaveFormatBlob& blob, uint16_t channels) {
    const uint32_t mask = channelMaskForDownmix(blob, channels);
    if (mask == 0 || std::popcount(mask) != channels) {
        return {};
    }

    std::vector<DownmixGain> gains(channels);
    float leftSum = 0.0f;
    float rightSum = 0.0f;
    uint32_t remainingBits = mask;
    for (uint16_t channel = 0; channel < channels && remainingBits != 0; ++channel) {
        const uint32_t speaker = 1u << std::countr_zero(remainingBits);
        remainingBits &= remainingBits - 1;
        DownmixGain& gain = gains[channel];
        switch (speaker) {
        case kSpeakerFrontLeft:
        case kSpeakerFrontLeftOfCenter:
            gain.left = 1.0f;
            break;
        case kSpeakerFrontRight:
        case kSpeakerFrontRightOfCenter:
            gain.right = 1.0f;
            break;
        case kSpeakerFrontCenter:
        case kSpeakerBackCenter:
            gain.left = 0.70710678f;
            gain.right = 0.70710678f;
            break;
        case kSpeakerBackLeft:
        case kSpeakerSideLeft:
            gain.left = 0.70710678f;
            break;
        case kSpeakerBackRight:
        case kSpeakerSideRight:
            gain.right = 0.70710678f;
            break;
        case kSpeakerLowFrequency:
        default:
            break; // LFE and unknown positions stay out of the stereo mix.
        }
        leftSum += gain.left;
        rightSum += gain.right;
    }
    // Normalize by the loudest possible output sum so full-scale content on
    // several channels at once cannot clip after folding.
    const float peak = std::max(leftSum, rightSum);
    if (peak > 1.0f) {
        const float scale = 1.0f / peak;
        for (auto& gain : gains) {
            gain.left *= scale;
            gain.right *= scale;
        }
    }
    return gains;
}

} // namespace

bool convertAudioPacketToCanonical(AudioPacket& packet) {
    if (!packet.format) {
        if (!packet.bytes.empty()) {
            logUnsupportedFormatOnce(packet);
            return false;
        }
        return true;
    }

    const AudioFormatInfo source = parseAudioFormat(*packet.format);
    if (packet.bytes.empty()) {
        packet.frameCount = 0;
        return true;
    }
    if (!audioFormatUsable(source)) {
        logUnsupportedFormatOnce(packet);
        return false;
    }

    const size_t totalFrames = packet.bytes.size() / source.blockAlign;
    if (totalFrames == 0) {
        packet.frameCount = 0;
        packet.bytes.clear();
        return true;
    }

    if (isCanonicalAudioFormat(source)) {
        // Fast path: already canonical. Re-derive frameCount from the payload
        // so the timeline math downstream can trust it.
        packet.frameCount = static_cast<uint32_t>(
            std::min<uint64_t>(totalFrames, (std::numeric_limits<uint32_t>::max)()));
        packet.format = canonicalAudioFormatPtr();
        return true;
    }

    const uint64_t sourceRate = source.samplesPerSec;
    const uint64_t targetRate = kCanonicalAudioSampleRate;
    const auto outputFrames = static_cast<uint64_t>(
        std::llround(static_cast<double>(totalFrames) * targetRate / sourceRate));
    if (outputFrames == 0 || outputFrames > ((std::numeric_limits<size_t>::max)() / (2 * sizeof(float)))) {
        logUnsupportedFormatOnce(packet);
        return false;
    }

    // Per-source-channel stereo weights. Mono duplicates to both outputs,
    // stereo maps front-left/front-right exactly as before, and layouts with
    // more than two channels fold down through a standard matrix so center
    // dialogue and surrounds survive instead of being dropped.
    struct ChannelWeight {
        size_t offset;
        float left;
        float right;
    };
    const size_t sourceBytesPerSample = source.bitsPerSample / 8;
    std::vector<ChannelWeight> weights;
    weights.reserve(source.channels);
    if (source.channels == 1) {
        weights.push_back({0, 1.0f, 1.0f});
    } else if (source.channels == 2) {
        weights.push_back({0, 1.0f, 0.0f});
        weights.push_back({sourceBytesPerSample, 0.0f, 1.0f});
    } else if (std::vector<DownmixGain> gains =
                   surroundDownmixGains(*packet.format, source.channels);
               !gains.empty()) {
        bool anyLeft = false;
        bool anyRight = false;
        for (uint16_t channel = 0; channel < source.channels; ++channel) {
            if (gains[channel].left != 0.0f || gains[channel].right != 0.0f) {
                weights.push_back({
                    static_cast<size_t>(channel) * sourceBytesPerSample,
                    gains[channel].left,
                    gains[channel].right});
            }
            anyLeft = anyLeft || gains[channel].left > 0.0f;
            anyRight = anyRight || gains[channel].right > 0.0f;
        }
        // A mask that folds everything onto one side would silently lose the
        // other channel; fall back to front-left/front-right instead.
        if (!anyLeft || !anyRight) {
            weights.clear();
        }
    }
    if (weights.empty()) {
        // Unknown layout without a usable mask: keep the previous
        // front-left/front-right selection rather than emitting silence.
        weights.push_back({0, 1.0f, 0.0f});
        weights.push_back({static_cast<size_t>(source.channels > 1) * sourceBytesPerSample, 0.0f, 1.0f});
    }

    const uint8_t* data = packet.bytes.data();
    std::vector<float> converted(static_cast<size_t>(outputFrames) * 2);
    for (uint64_t frame = 0; frame < outputFrames; ++frame) {
        // Exact rational position of this output frame on the source timeline
        // (fixed-point instead of an accumulating float phase).
        const uint64_t numerator = frame * sourceRate;
        const uint64_t wholeFrame = numerator / targetRate;
        const uint64_t remainder = numerator % targetRate;
        const uint64_t nextFrame = std::min<uint64_t>(wholeFrame + 1, totalFrames - 1);
        const float fraction = static_cast<float>(static_cast<double>(remainder) / targetRate);

        const size_t base0 = static_cast<size_t>(wholeFrame) * source.blockAlign;
        const size_t base1 = static_cast<size_t>(nextFrame) * source.blockAlign;
        float left = 0.0f;
        float right = 0.0f;
        for (const ChannelWeight& weight : weights) {
            const float value0 = decodeInterleavedSample(data + base0 + weight.offset, source);
            const float value1 = decodeInterleavedSample(data + base1 + weight.offset, source);
            const float value = value0 + (value1 - value0) * fraction;
            left += weight.left * value;
            right += weight.right * value;
        }
        converted[static_cast<size_t>(frame) * 2] = left;
        converted[static_cast<size_t>(frame) * 2 + 1] = right;
    }

    packet.bytes.resize(converted.size() * sizeof(float));
    std::memcpy(packet.bytes.data(), converted.data(), packet.bytes.size());
    packet.frameCount = static_cast<uint32_t>(outputFrames);
    packet.format = canonicalAudioFormatPtr();
    return true;
}

} // namespace backtrack
