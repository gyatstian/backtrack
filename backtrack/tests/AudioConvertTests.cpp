#include "audio/AudioConvert.h"
#include "audio/AudioMix.h"

#include <Windows.h>
#include <mmreg.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

using backtrack::AudioFormatInfo;
using backtrack::AudioPacket;
using backtrack::AudioSampleEncoding;
using backtrack::AudioTrack;
using backtrack::WaveFormatBlob;

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::shared_ptr<const WaveFormatBlob> makeBlob(const WAVEFORMATEX& format) {
    auto blob = std::make_shared<WaveFormatBlob>();
    const auto* bytes = reinterpret_cast<const uint8_t*>(&format);
    blob->bytes.assign(bytes, bytes + sizeof(format));
    return blob;
}

std::shared_ptr<const WaveFormatBlob> pcmFormat(uint16_t channels, uint32_t rate, uint16_t bits) {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = channels;
    format.nSamplesPerSec = rate;
    format.wBitsPerSample = bits;
    format.nBlockAlign = static_cast<uint16_t>(channels * bits / 8);
    format.nAvgBytesPerSec = rate * format.nBlockAlign;
    return makeBlob(format);
}

// WAVEFORMATEXTENSIBLE sub-formats, defined here without ksmedia.h.
constexpr GUID kPcmSubFormat = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kFloatSubFormat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

std::shared_ptr<const WaveFormatBlob> extensibleFormat(
    uint16_t channels, uint32_t rate, uint16_t bits, const GUID& subFormat,
    uint16_t padding = 0, uint16_t validBits = 0) {
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = channels;
    format.Format.nSamplesPerSec = rate;
    format.Format.wBitsPerSample = bits;
    format.Format.nBlockAlign = static_cast<uint16_t>(channels * (bits / 8) + padding);
    format.Format.nAvgBytesPerSec = rate * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = validBits == 0 ? bits : validBits;
    format.dwChannelMask = channels == 1 ? 0x4 : (channels == 2 ? 0x3 : 0x3F);
    format.SubFormat = subFormat;
    auto blob = std::make_shared<WaveFormatBlob>();
    const auto* bytes = reinterpret_cast<const uint8_t*>(&format);
    blob->bytes.assign(bytes, bytes + sizeof(format));
    return blob;
}

std::shared_ptr<const WaveFormatBlob> unsupportedFormat() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_ADPCM; // Not PCM/float: must be rejected.
    format.nChannels = 2;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 4;
    format.nBlockAlign = 1024;
    format.nAvgBytesPerSec = 48000 * 1024;
    return makeBlob(format);
}

float sampleAt(const AudioPacket& packet, uint32_t frame, int channel) {
    float value = 0.0f;
    const size_t offset = (static_cast<size_t>(frame) * 2 + channel) * sizeof(float);
    std::memcpy(&value, packet.bytes.data() + offset, sizeof(value));
    return value;
}

void testCanonicalPassthrough() {
    AudioPacket packet;
    packet.track = AudioTrack::System;
    packet.format = std::make_shared<WaveFormatBlob>(backtrack::canonicalAudioFormat());
    packet.frameCount = 480;
    packet.pts100ns = 12345;
    // One second of constant -0.25 stereo.
    packet.bytes.assign(480 * 2 * sizeof(float), 0);
    for (uint32_t frame = 0; frame < 480; ++frame) {
        for (int channel = 0; channel < 2; ++channel) {
            const float value = -0.25f;
            std::memcpy(packet.bytes.data() + (frame * 2 + channel) * sizeof(float), &value, sizeof(value));
        }
    }

    const auto originalBytes = packet.bytes;
    expect(backtrack::convertAudioPacketToCanonical(packet), "canonical passthrough was rejected");
    const AudioFormatInfo info = backtrack::parseAudioFormat(*packet.format);
    expect(backtrack::isCanonicalAudioFormat(info), "passthrough lost the canonical format");
    expect(packet.bytes == originalBytes, "passthrough changed the payload");
    expect(packet.frameCount == 480, "passthrough changed the frame count");
    expect(packet.pts100ns == 12345, "conversion touched PTS");
    expect(std::fabs(sampleAt(packet, 0, 0) + 0.25f) < 1e-6f, "passthrough corrupted samples");
}

void testPaddedStereoFloatIsRepacked() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 2;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 32;
    format.nBlockAlign = 12;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    const std::shared_ptr<const WaveFormatBlob> formats[] = {
        makeBlob(format), extensibleFormat(2, 48000, 32, kFloatSubFormat, 4)};
    constexpr uint32_t kFrames = 3;
    constexpr float samples[kFrames][2] = {{0.25f, -0.5f}, {0.75f, -0.25f}, {-0.75f, 0.5f}};

    for (const auto& blob : formats) {
        AudioPacket packet;
        packet.format = blob;
        packet.frameCount = 99;
        packet.pts100ns = 12345;
        packet.bytes.assign(kFrames * format.nBlockAlign, 0x7f);
        for (uint32_t frame = 0; frame < kFrames; ++frame) {
            std::memcpy(packet.bytes.data() + frame * format.nBlockAlign, samples[frame], sizeof(samples[frame]));
        }

        const AudioFormatInfo input = backtrack::parseAudioFormat(*packet.format);
        expect(backtrack::audioFormatUsable(input), "padded stereo float format should be usable");
        expect(!backtrack::isCanonicalAudioFormat(input), "padded stereo float is not canonical");
        expect(backtrack::convertAudioPacketToCanonical(packet), "padded stereo float conversion failed");
        expect(packet.frameCount == kFrames && packet.bytes.size() == kFrames * 2 * sizeof(float),
            "padded stereo float was not tightly repacked");
        expect(packet.pts100ns == 12345, "padded stereo conversion changed PTS");
        expect(packet.format->bytes == backtrack::canonicalAudioFormat().bytes,
            "padded stereo conversion did not emit the canonical format");
        for (uint32_t frame = 0; frame < kFrames; ++frame) {
            for (int channel = 0; channel < 2; ++channel) {
                expect(sampleAt(packet, frame, channel) == samples[frame][channel],
                    "padded stereo conversion corrupted samples");
            }
        }
    }
}

void testSupportedSampleWidths() {
    struct SampleCase {
        uint16_t tag;
        uint16_t bits;
        uint16_t validBits;
        uint8_t left[4];
        uint8_t right[4];
    };
    const SampleCase cases[] = {
        {WAVE_FORMAT_PCM, 8, 8, {0xc0}, {0x40}},
        {WAVE_FORMAT_PCM, 16, 16, {0x00, 0x40}, {0x00, 0xc0}},
        {WAVE_FORMAT_PCM, 24, 24, {0x00, 0x00, 0x40}, {0x00, 0x00, 0xc0}},
        {WAVE_FORMAT_PCM, 32, 32, {0x00, 0x00, 0x00, 0x40}, {0x00, 0x00, 0x00, 0xc0}},
        {WAVE_FORMAT_PCM, 32, 24, {0x00, 0x00, 0x00, 0x40}, {0x00, 0x00, 0x00, 0xc0}},
        {WAVE_FORMAT_IEEE_FLOAT, 32, 32, {0x00, 0x00, 0x00, 0x3f}, {0x00, 0x00, 0x00, 0xbf}},
    };
    for (const SampleCase& sample : cases) {
        WAVEFORMATEX format{};
        format.wFormatTag = sample.tag;
        format.nChannels = 2;
        format.nSamplesPerSec = 48000;
        format.wBitsPerSample = sample.bits;
        const uint16_t sampleBytes = sample.bits / 8;
        format.nBlockAlign = static_cast<uint16_t>(2 * sampleBytes);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        const GUID& subFormat = sample.tag == WAVE_FORMAT_PCM ? kPcmSubFormat : kFloatSubFormat;
        const std::shared_ptr<const WaveFormatBlob> formats[] = {
            makeBlob(format), extensibleFormat(2, 48000, sample.bits, subFormat, 0, sample.validBits)};
        for (const auto& blob : formats) {
            AudioPacket packet;
            packet.format = blob;
            packet.bytes.resize(format.nBlockAlign);
            std::memcpy(packet.bytes.data(), sample.left, sampleBytes);
            std::memcpy(packet.bytes.data() + sampleBytes, sample.right, sampleBytes);

            expect(backtrack::audioFormatUsable(backtrack::parseAudioFormat(*packet.format)),
                "supported sample width was rejected");
            expect(backtrack::convertAudioPacketToCanonical(packet), "supported sample conversion failed");
            expect(packet.frameCount == 1 && packet.bytes.size() == 2 * sizeof(float),
                "supported sample conversion has the wrong size");
            expect(backtrack::isCanonicalAudioFormat(backtrack::parseAudioFormat(*packet.format)),
                "supported sample conversion did not produce canonical audio");
            expect(sampleAt(packet, 0, 0) == 0.5f && sampleAt(packet, 0, 1) == -0.5f,
                "supported sample conversion corrupted amplitudes");
        }
    }
}

void testMonoPcm16ResampleAndDuplication() {
    constexpr uint32_t kSourceRate = 44100;
    constexpr uint32_t kFrames = kSourceRate / 100; // 10ms @ 44.1k
    AudioPacket packet;
    packet.track = AudioTrack::Microphone;
    packet.format = pcmFormat(1, kSourceRate, 16);
    packet.frameCount = kFrames;
    packet.bytes.resize(static_cast<size_t>(kFrames) * 2);
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        const int16_t value = static_cast<int16_t>((frame % 2 == 0) ? 16384 : -16384);
        std::memcpy(packet.bytes.data() + frame * 2, &value, sizeof(value));
    }

    expect(backtrack::convertAudioPacketToCanonical(packet), "44.1k mono s16 conversion failed");
    const AudioFormatInfo info = backtrack::parseAudioFormat(*packet.format);
    expect(info.samplesPerSec == 48000 && info.channels == 2 && info.bitsPerSample == 32,
        "converted format is not canonical");
    const uint32_t expectedFrames = static_cast<uint32_t>(
        std::llround(static_cast<double>(kFrames) * 48000 / kSourceRate));
    expect(packet.frameCount == expectedFrames, "resampled frame count is wrong");
    expect(packet.bytes.size() == static_cast<size_t>(packet.frameCount) * 2 * sizeof(float),
        "payload size does not match frame count");

    bool leftEqualsRight = true;
    bool sawPositive = false;
    bool sawNegative = false;
    for (uint32_t frame = 0; frame < packet.frameCount; ++frame) {
        if (sampleAt(packet, frame, 0) != sampleAt(packet, frame, 1)) {
            leftEqualsRight = false;
        }
        sawPositive = sawPositive || sampleAt(packet, frame, 0) > 0.4f;
        sawNegative = sawNegative || sampleAt(packet, frame, 0) < -0.4f;
    }
    expect(leftEqualsRight, "mono source was not duplicated to both channels");
    expect(sawPositive && sawNegative, "sample amplitudes were not preserved through resampling");
}

void testFormatChangeMidStreamIsTransparent() {
    // Regression scenario: WASAPI reconnect swaps a 44.1k stereo float device
    // for a 48k mono s16 one mid-recording. Every converted packet must share
    // one identical canonical format so timeline writers never see a mismatch.
    std::vector<AudioPacket> packets(2);
    packets[0].track = AudioTrack::System;
    packets[0].format = extensibleFormat(2, 44100, 32, kFloatSubFormat);
    packets[0].frameCount = 441;
    packets[0].bytes.resize(441 * 2 * sizeof(float));

    packets[1].track = AudioTrack::System;
    packets[1].format = pcmFormat(1, 48000, 16);
    packets[1].frameCount = 480;
    packets[1].bytes.resize(480 * 2);

    for (auto& packet : packets) {
        expect(backtrack::convertAudioPacketToCanonical(packet), "mid-stream conversion failed");
        const AudioFormatInfo info = backtrack::parseAudioFormat(*packet.format);
        expect(backtrack::isCanonicalAudioFormat(info), "converted stream is not uniform");
    }
    expect(packets[0].format->bytes == packets[1].format->bytes,
        "converted formats differ between packets");
    expect(packets[0].frameCount == 480, "44.1k stereo resample produced the wrong length");
    expect(packets[1].frameCount == 480, "48k mono passthrough produced the wrong length");
}

void testInvalidFormatsAreRejected() {
    struct InvalidFormat {
        uint16_t tag;
        uint16_t bits;
        uint16_t channels;
        uint16_t blockAlign;
        uint32_t rate;
        const char* message;
    };
    const InvalidFormat cases[] = {
        {WAVE_FORMAT_PCM, 16, 32768, 0, 48000, "wrapped zero stride was accepted"},
        {WAVE_FORMAT_PCM, 16, 32768, 65535, 48000, "unrepresentable stride was accepted"},
        {WAVE_FORMAT_PCM, 16, 32769, 2, 48000, "wrapped nonzero stride was accepted"},
        {WAVE_FORMAT_PCM, 16, 2, 0, 48000, "zero stride was accepted"},
        {WAVE_FORMAT_PCM, 16, 2, 3, 48000, "short PCM stride was accepted"},
        {WAVE_FORMAT_IEEE_FLOAT, 32, 2, 7, 48000, "short float stride was accepted"},
        {WAVE_FORMAT_PCM, 64, 2, 16, 48000, "64-bit PCM was accepted"},
        {WAVE_FORMAT_IEEE_FLOAT, 64, 2, 16, 48000, "64-bit float was accepted"},
        {WAVE_FORMAT_IEEE_FLOAT, 16, 2, 4, 48000, "16-bit float was accepted"},
        {WAVE_FORMAT_IEEE_FLOAT, 24, 2, 6, 48000, "24-bit float was accepted"},
        {WAVE_FORMAT_PCM, 12, 2, 4, 48000, "12-bit PCM was accepted"},
        {WAVE_FORMAT_PCM, 16, 0, 4, 48000, "zero channels were accepted"},
        {WAVE_FORMAT_PCM, 16, 2, 4, 0, "zero sample rate was accepted"},
    };
    for (const InvalidFormat& invalid : cases) {
        WAVEFORMATEX format{};
        format.wFormatTag = invalid.tag;
        format.wBitsPerSample = invalid.bits;
        format.nChannels = invalid.channels;
        format.nSamplesPerSec = invalid.rate;
        format.nBlockAlign = invalid.blockAlign;
        format.nAvgBytesPerSec = invalid.rate * invalid.blockAlign;
        AudioPacket packet;
        packet.format = makeBlob(format);
        packet.frameCount = 7;
        packet.bytes.assign(32, 0x7f);
        const auto originalBytes = packet.bytes;
        const auto originalFormat = packet.format;
        expect(!backtrack::audioFormatUsable(backtrack::parseAudioFormat(*packet.format)), invalid.message);
        expect(!backtrack::convertAudioPacketToCanonical(packet), invalid.message);
        expect(packet.bytes == originalBytes && packet.format == originalFormat && packet.frameCount == 7,
            "rejected conversion modified the packet");
        packet.bytes.clear();
        expect(!backtrack::convertAudioPacketToCanonical(packet), invalid.message);
    }

    struct ExtensibleCase {
        GUID subFormat;
        AudioSampleEncoding encoding;
    };
    const ExtensibleCase extensibleCases[] = {
        {kPcmSubFormat, AudioSampleEncoding::Pcm}, {kFloatSubFormat, AudioSampleEncoding::Float}};
    for (const ExtensibleCase& format : extensibleCases) {
        AudioPacket packet;
        packet.format = extensibleFormat(2, 48000, 64, format.subFormat);
        packet.bytes.assign(16, 0x7f);
        const AudioFormatInfo info = backtrack::parseAudioFormat(*packet.format);
        expect(info.encoding == format.encoding && info.bitsPerSample == 64,
            "64-bit extensible format was not parsed");
        expect(!backtrack::audioFormatUsable(info) && !backtrack::convertAudioPacketToCanonical(packet),
            "64-bit extensible format was accepted");
    }

    AudioFormatInfo canonical = backtrack::parseAudioFormat(backtrack::canonicalAudioFormat());
    canonical.blockAlign = 0;
    expect(!backtrack::isCanonicalAudioFormat(canonical), "zero-stride format was marked canonical");

    AudioPacket empty;
    empty.format = pcmFormat(1, 48000, 16);
    empty.frameCount = 7;
    expect(backtrack::convertAudioPacketToCanonical(empty) && empty.frameCount == 0,
        "empty packet with a valid format was rejected");
}

void testExtensibleSizeAndTagGuards() {
    const auto full = extensibleFormat(3, 48000, 32, kFloatSubFormat);
    for (size_t size = 0; size < sizeof(WAVEFORMATEXTENSIBLE); ++size) {
        auto truncated = std::make_shared<WaveFormatBlob>(*full);
        truncated->bytes.resize(size);
        expect(backtrack::parseAudioFormat(*truncated).encoding == AudioSampleEncoding::Unsupported,
            "truncated extensible header was interpreted as PCM or float");
        AudioPacket packet;
        packet.format = truncated;
        packet.frameCount = 7;
        packet.pts100ns = 12345;
        packet.bytes.assign(12, 0x7f);
        const auto originalBytes = packet.bytes;
        expect(!backtrack::convertAudioPacketToCanonical(packet), "truncated extensible header was converted");
        expect(packet.bytes == originalBytes && packet.format == truncated &&
               packet.frameCount == 7 && packet.pts100ns == 12345,
            "rejected extensible header modified the packet");
    }

    auto unknown = std::make_shared<WaveFormatBlob>(*full);
    auto* format = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(unknown->bytes.data());
    format->SubFormat.Data4[7] ^= 1;
    expect(backtrack::parseAudioFormat(*unknown).encoding == AudioSampleEncoding::Unsupported,
        "non-wave subformat GUID was accepted");
    format->SubFormat = kFloatSubFormat;
    format->Format.wFormatTag = WAVE_FORMAT_ADPCM;
    expect(backtrack::parseAudioFormat(*unknown).encoding == AudioSampleEncoding::Unsupported,
        "extensible-sized non-extensible header used its subformat");

    format->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format->Format.cbSize = 0;
    expect(backtrack::parseAudioFormat(*unknown).encoding == AudioSampleEncoding::Float,
        "complete extensible blob must retain the existing size/tag-only guard");
}

void testChannelMaskGuards() {
    struct MaskCase {
        uint16_t tag;
        uint32_t mask;
        bool usesLfe;
    };
    const MaskCase cases[] = {
        {WAVE_FORMAT_EXTENSIBLE, 0xB, true}, // FL FR LFE rather than the default FL FR FC.
        {WAVE_FORMAT_EXTENSIBLE, 0, false},
        {WAVE_FORMAT_EXTENSIBLE, 0x3, false},
        {WAVE_FORMAT_IEEE_FLOAT, 0xB, false},
    };
    for (const auto& test : cases) {
        auto blob = std::make_shared<WaveFormatBlob>(*extensibleFormat(3, 48000, 32, kFloatSubFormat));
        auto* format = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(blob->bytes.data());
        format->Format.wFormatTag = test.tag;
        format->dwChannelMask = test.mask;
        AudioPacket packet;
        packet.format = blob;
        const float samples[] = {0.25f, -0.5f, 1.0f};
        packet.bytes.resize(sizeof(samples));
        std::memcpy(packet.bytes.data(), samples, sizeof(samples));
        expect(backtrack::convertAudioPacketToCanonical(packet), "channel-mask guard conversion failed");
        constexpr float centerGain = 0.70710678f;
        const float left = test.usesLfe ? samples[0] : (samples[0] + centerGain) / (1.0f + centerGain);
        const float right = test.usesLfe ? samples[1] : (samples[1] + centerGain) / (1.0f + centerGain);
        expect(std::fabs(sampleAt(packet, 0, 0) - left) < 1e-6f &&
               std::fabs(sampleAt(packet, 0, 1) - right) < 1e-6f,
            "channel-mask size/tag/popcount fallback changed");
    }
}

void testLargestRepresentableStride() {
    AudioPacket packet;
    packet.format = pcmFormat(32767, 48000, 16);
    packet.bytes.resize(65534);
    const int16_t firstSamples[] = {16384, -16384};
    std::memcpy(packet.bytes.data(), firstSamples, sizeof(firstSamples));
    AudioFormatInfo info = backtrack::parseAudioFormat(*packet.format);
    expect(backtrack::audioFormatUsable(info), "largest PCM16 stride was rejected");
    expect(backtrack::convertAudioPacketToCanonical(packet), "largest PCM16 stride could not be converted");
    expect(packet.frameCount == 1 && packet.bytes.size() == 2 * sizeof(float),
        "largest PCM16 stride produced the wrong output size");
    expect(sampleAt(packet, 0, 0) == 0.5f && sampleAt(packet, 0, 1) == -0.5f,
        "largest PCM16 stride produced the wrong samples");

    info.bitsPerSample = 8;
    info.channels = 65535;
    info.blockAlign = 65535;
    expect(backtrack::audioFormatUsable(info), "largest PCM8 stride was rejected");
}

void testUnsupportedFormatDropped() {
    AudioPacket packet;
    packet.track = AudioTrack::System;
    packet.format = unsupportedFormat();
    packet.frameCount = 10;
    packet.bytes.assign(64, 0x7F);
    expect(!backtrack::convertAudioPacketToCanonical(packet),
        "unsupported format should fail conversion so callers drop it");
}

void testSilenceStaysSilent() {
    AudioPacket packet;
    packet.track = AudioTrack::Microphone;
    packet.format = pcmFormat(2, 48000, 24);
    packet.frameCount = 480;
    packet.bytes.assign(480 * 2 * 3, 0);
    expect(backtrack::convertAudioPacketToCanonical(packet), "s24 silence conversion failed");
    for (size_t offset = 0; offset < packet.bytes.size(); offset += sizeof(float)) {
        float value = 1.0f;
        std::memcpy(&value, packet.bytes.data() + offset, sizeof(value));
        expect(value == 0.0f, "silence became non-silent");
    }
}

void testSurroundDownmixKeepsCenterAndDropsLfe() {
    // 5.1 input (mask 0x3F): FL FR FC LFE BL BR. The fold-down must keep the
    // center channel (dialogue) in both outputs and leave the LFE out.
    constexpr uint32_t kFrames = 480;
    AudioPacket packet;
    packet.track = AudioTrack::System;
    packet.format = extensibleFormat(6, 48000, 32, kFloatSubFormat);
    packet.frameCount = kFrames;
    packet.bytes.resize(static_cast<size_t>(kFrames) * 6 * sizeof(float));
    constexpr float kFl = 0.25f;
    constexpr float kFr = -0.25f;
    constexpr float kFc = 1.0f;
    constexpr float kLfe = 0.75f; // Must not leak into the stereo mix.
    constexpr float kBl = 0.5f;
    constexpr float kBr = -0.5f;
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        const float values[6] = {kFl, kFr, kFc, kLfe, kBl, kBr};
        for (int channel = 0; channel < 6; ++channel) {
            std::memcpy(packet.bytes.data() + (static_cast<size_t>(frame) * 6 + channel) * sizeof(float),
                        &values[channel], sizeof(float));
        }
    }

    expect(backtrack::convertAudioPacketToCanonical(packet), "5.1 conversion failed");

    // BS.775 fold-down normalized by the largest per-output gain sum:
    // L = FL + C*0.707 + BL*0.707, R = FR + C*0.707 + BR*0.707, then scaled by
    // 1 / (1 + 2 * 0.707).
    constexpr float kSqrtHalf = 0.70710678f;
    const float scale = 1.0f / (1.0f + 2.0f * kSqrtHalf);
    const float expectedLeft = (kFl + kSqrtHalf * kFc + kSqrtHalf * kBl) * scale;
    const float expectedRight = (kFr + kSqrtHalf * kFc + kSqrtHalf * kBr) * scale;

    bool leftMatches = true;
    bool rightMatches = true;
    for (uint32_t frame = 0; frame < packet.frameCount; ++frame) {
        if (!(std::fabs(sampleAt(packet, frame, 0) - expectedLeft) < 1e-4f)) {
            leftMatches = false;
        }
        if (!(std::fabs(sampleAt(packet, frame, 1) - expectedRight) < 1e-4f)) {
            rightMatches = false;
        }
    }
    expect(leftMatches, "5.1 left output does not match the standard fold-down");
    expect(rightMatches, "5.1 right output does not match the standard fold-down");

    // Center dialogue must survive: without FC the left level would differ.
    const float withoutCenter = (kFl + kSqrtHalf * kBl) * scale;
    expect(std::fabs(expectedLeft - withoutCenter) > 0.05f,
        "test lost sensitivity to a dropped center channel");
}

void testSurroundScratchReuseAcrossMasks() {
    constexpr float surround = 0.70710678f;
    const float scale51 = 1.0f / (1.0f + surround + surround);
    const float gain51 = surround * scale51;
    const float scaleSide = 1.0f / (1.0f + surround);
    const float gainSide = surround * scaleSide;
    struct MaskCase {
        uint16_t channels;
        uint32_t mask;
        float samples[6];
        float left;
        float right;
    };
    const MaskCase cases[] = {
        {6, 0x3F, {0.25f, -0.5f, 0.75f, 1.0f, 0.5f, -0.25f},
            0.25f * scale51 + 0.75f * gain51 + 0.5f * gain51,
            -0.5f * scale51 + 0.75f * gain51 - 0.25f * gain51},
        {3, 0x51, {0.25f, -0.5f, 0.75f}, 0.25f, -0.5f}, // FL BL FLC: left-only fallback.
        {5, 0xE09, {0.25f, 1.0f, -1.0f, 0.5f, -0.75f}, // FL LFE unknown SL SR.
            0.25f * scaleSide + 0.5f * gainSide, -0.75f * gainSide},
        {3, 0xA2, {0.25f, -0.5f, 0.75f}, 0.25f, -0.5f}, // FR BR FRC: right-only fallback.
        {3, 0xB, {-0.75f, 0.5f, 1.0f}, -0.75f, 0.5f}, // FL FR LFE: no normalization.
    };
    backtrack::AudioConvertScratch scratch;
    for (const auto& test : cases) {
        auto blob = std::make_shared<WaveFormatBlob>(
            *extensibleFormat(test.channels, 48000, 32, kFloatSubFormat));
        reinterpret_cast<WAVEFORMATEXTENSIBLE*>(blob->bytes.data())->dwChannelMask = test.mask;
        AudioPacket packet;
        packet.format = blob;
        packet.pts100ns = 12345;
        packet.bytes.resize(test.channels * sizeof(float));
        std::memcpy(packet.bytes.data(), test.samples, packet.bytes.size());
        expect(backtrack::convertAudioPacketToCanonical(packet, scratch),
            "surround conversion with reused scratch failed");
        expect(packet.frameCount == 1 && packet.bytes.size() == 2 * sizeof(float) &&
               packet.pts100ns == 12345 &&
               backtrack::isCanonicalAudioFormat(backtrack::parseAudioFormat(*packet.format)),
            "surround scratch reuse changed packet metadata");
        expect(std::fabs(sampleAt(packet, 0, 0) - test.left) < 1e-6f &&
               std::fabs(sampleAt(packet, 0, 1) - test.right) < 1e-6f,
            "surround scratch reuse changed channel offsets, gains, or fallback");
    }
}

void testMuteGainPrimitive() {
    // applyMuteGain is the sole gain stage for canonical float samples: muted
    // zeroes, unity is a no-op, otherwise scale and clamp, never allocating.
    std::vector<float> samples = {0.5f, -0.25f, 0.9f, -1.0f};
    backtrack::applyMuteGain(std::span<float>(samples), 1.0f, false);
    expect(samples[0] == 0.5f && samples[3] == -1.0f, "unity gain must leave samples untouched");
    backtrack::applyMuteGain(std::span<float>(samples), 0.5f, false);
    expect(std::fabs(samples[0] - 0.25f) < 1e-6f, "half gain did not scale");
    backtrack::applyMuteGain(std::span<float>(samples), 4.0f, false);
    expect(samples[2] == 1.0f && samples[3] == -1.0f, "gain must clamp to [-1, 1]");
    backtrack::applyMuteGain(std::span<float>(samples), 1.0f, true);
    expect(samples[0] == 0.0f && samples[2] == 0.0f, "muted must zero the span");
    std::vector<float> empty;
    backtrack::applyMuteGain(std::span<float>(empty), 2.0f, false); // must not crash
}

} // namespace

int main() {
    try {
        testCanonicalPassthrough();
        testPaddedStereoFloatIsRepacked();
        testSupportedSampleWidths();
        testMonoPcm16ResampleAndDuplication();
        testFormatChangeMidStreamIsTransparent();
        testInvalidFormatsAreRejected();
        testExtensibleSizeAndTagGuards();
        testChannelMaskGuards();
        testLargestRepresentableStride();
        testUnsupportedFormatDropped();
        testSilenceStaysSilent();
        testSurroundDownmixKeepsCenterAndDropsLfe();
        testSurroundScratchReuseAcrossMasks();
        testMuteGainPrimitive();
        std::cout << "AudioConvert tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AudioConvert test failure: " << error.what() << '\n';
        return 1;
    }
}
