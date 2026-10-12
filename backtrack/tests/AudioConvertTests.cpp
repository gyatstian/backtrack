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

// {00000003-0000-0010-8000-00aa00389b71}: IEEE float sub-format for
// WAVEFORMATEXTENSIBLE (defined here so the test does not need ksmedia.h).
constexpr GUID kFloatSubFormat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

std::shared_ptr<const WaveFormatBlob> floatExtensibleFormat(uint16_t channels, uint32_t rate) {
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = channels;
    format.Format.nSamplesPerSec = rate;
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = static_cast<uint16_t>(channels * 4);
    format.Format.nAvgBytesPerSec = rate * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = channels == 1 ? 0x4 : (channels == 2 ? 0x3 : 0x3F);
    format.SubFormat = kFloatSubFormat;
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

    expect(backtrack::convertAudioPacketToCanonical(packet), "canonical passthrough was rejected");
    const AudioFormatInfo info = backtrack::parseAudioFormat(*packet.format);
    expect(backtrack::isCanonicalAudioFormat(info), "passthrough lost the canonical format");
    expect(packet.frameCount == 480, "passthrough changed the frame count");
    expect(packet.pts100ns == 12345, "conversion touched PTS");
    expect(std::fabs(sampleAt(packet, 0, 0) + 0.25f) < 1e-6f, "passthrough corrupted samples");
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
    packets[0].format = floatExtensibleFormat(2, 44100);
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
    packet.format = floatExtensibleFormat(6, 48000);
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
        testMonoPcm16ResampleAndDuplication();
        testFormatChangeMidStreamIsTransparent();
        testUnsupportedFormatDropped();
        testSilenceStaysSilent();
        testSurroundDownmixKeepsCenterAndDropsLfe();
        testMuteGainPrimitive();
        std::cout << "AudioConvert tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AudioConvert test failure: " << error.what() << '\n';
        return 1;
    }
}
