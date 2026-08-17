#include "audio/WavReader.h"
#include "mux/Mp4Muxer.h"
#include "replay/ReplayBuffer.h"

#include <Windows.h>
#include <mmreg.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

// Link-time stub for the Media Foundation muxer; ReplayBuffer::saveTo only needs
// the sample table MP4 muxing would receive. The stub records the inputs (and
// parses the produced WAV while its temp directory still exists) for asserts.
namespace backtrack {

static MuxedInputs g_lastMuxedInputs;
static std::optional<WavInfo> g_lastSystemWav;

bool Mp4Muxer::muxToMp4(const MuxedInputs& inputs, const std::filesystem::path& /*outputPath*/) {
    g_lastMuxedInputs = inputs;
    g_lastSystemWav.reset();
    if (!inputs.systemAudioPath.empty()) {
        WavInfo info;
        if (parseWav(inputs.systemAudioPath, info)) {
            g_lastSystemWav = info;
        }
    }
    return true;
}

} // namespace backtrack

namespace {

using backtrack::AudioPacket;
using backtrack::AudioTrack;
using backtrack::EncodedPacket;
using backtrack::MuxedInputs;
using backtrack::ReplayBuffer;
using backtrack::ReplaySettings;
using backtrack::VideoCodec;
using backtrack::VideoSettings;
using backtrack::WaveFormatBlob;

constexpr int64_t kSecond100ns = backtrack::kHundredNanosecondsPerSecond;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

EncodedPacket makeVideo(int64_t pts100ns, int64_t duration100ns, bool keyFrame) {
    EncodedPacket packet;
    packet.bytes.assign(4, 0x65);
    packet.pts100ns = pts100ns;
    packet.duration100ns = duration100ns;
    packet.keyFrame = keyFrame;
    packet.codec = VideoCodec::H264;
    return packet;
}

std::shared_ptr<const WaveFormatBlob> makePcmFormat() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = 48000 * 4;
    auto blob = std::make_shared<WaveFormatBlob>();
    const auto* bytes = reinterpret_cast<const uint8_t*>(&format);
    blob->bytes.assign(bytes, bytes + sizeof(WAVEFORMATEX));
    return blob;
}

AudioPacket makeAudio(int64_t pts100ns, uint32_t seconds) {
    AudioPacket packet;
    packet.track = AudioTrack::System;
    packet.frameCount = seconds * 48000;
    packet.pts100ns = pts100ns;
    packet.format = makePcmFormat();
    packet.bytes.assign(static_cast<size_t>(seconds) * 48000 * 4, 0x80);
    return packet;
}

ReplaySettings settings(uint32_t seconds) {
    ReplaySettings result;
    result.seconds = seconds;
    result.enabled = true;
    return result;
}

std::filesystem::path testOutputDir() {
    return std::filesystem::temp_directory_path() / L"backtrack_replay_test";
}

void testKeyframeCountingAndTrim() {
    ReplayBuffer buffer;
    buffer.configure(settings(10));
    // 12 one-second packets, keyframe every 2s (pts 0..11).
    for (int64_t pts = 0; pts < 12; ++pts) {
        buffer.pushVideo(makeVideo(pts * kSecond100ns, kSecond100ns, (pts % 2) == 0));
    }
    // Trim keeps [newest-10s, newest] aligned to the first keyframe: packets
    // with pts < 1 are dropped (pts 0), then non-keyframe packets before the
    // first buffered keyframe are dropped (pts 1). Remaining: pts 2..11.
    expect(buffer.videoPacketCount() == 10, "trim did not keep a 10-second window");
    expect(buffer.videoKeyFrameCount() == 5, "trim did not re-count buffered keyframes");
    expect(buffer.videoSpan100ns() == 10 * kSecond100ns,
        "video span does not cover the buffered keyframe window");
    buffer.pushVideo(makeVideo(12 * kSecond100ns, kSecond100ns, false));
    expect(buffer.videoKeyFrameCount() == 5, "keyframe count changed on a P-frame push");
    buffer.clear();
    expect(buffer.videoPacketCount() == 0, "clear did not empty the video buffer");
    expect(buffer.videoKeyFrameCount() == 0, "clear did not reset the keyframe count");
    expect(buffer.videoSpan100ns() == 0, "clear did not reset the video span");
}

void testTailExtension() {
    ReplayBuffer buffer;
    buffer.configure(settings(30));
    buffer.pushVideo(makeVideo(0, kSecond100ns, true));
    buffer.pushVideo(makeVideo(kSecond100ns, kSecond100ns, false));
    buffer.pushVideo(makeVideo(2 * kSecond100ns, kSecond100ns, false));
    expect(buffer.videoSpan100ns() == 3 * kSecond100ns, "video span ignores packet durations");

    expect(buffer.extendLastVideoDuration(2 * kSecond100ns, 5 * kSecond100ns),
        "tail extension was rejected for the newest packet");
    expect(buffer.videoSpan100ns() == 7 * kSecond100ns,
        "tail extension did not extend the video span");
    expect(!buffer.extendLastVideoDuration(kSecond100ns, 5 * kSecond100ns),
        "non-newest packet was extended");
    expect(buffer.videoSpan100ns() == 7 * kSecond100ns,
        "non-newest extension changed the timeline");
    expect(buffer.videoPacketCount() == 3, "tail extension appended instead of replacing");
}

void testSaveToHonorsLiveTailAndKeyframeStart() {
    ReplayBuffer buffer;
    buffer.configure(settings(30));
    buffer.pushVideo(makeVideo(0, kSecond100ns, true));
    buffer.pushVideo(makeVideo(kSecond100ns, kSecond100ns, false));
    buffer.pushVideo(makeVideo(2 * kSecond100ns, kSecond100ns, false));
    buffer.extendLastVideoDuration(2 * kSecond100ns, 2 * kSecond100ns);

    const auto output = testOutputDir() / L"tail.mp4";
    std::error_code error;
    std::filesystem::create_directories(output.parent_path(), error);
    VideoSettings videoSettings;
    videoSettings.codec = VideoCodec::H264;
    videoSettings.fps = 60;
    videoSettings.width = 1920;
    videoSettings.height = 1080;
    expect(buffer.saveTo(output, videoSettings), "saveTo failed for a warm buffer");

    const auto& samples = backtrack::g_lastMuxedInputs.videoSamples;
    expect(samples.size() == 3, "saved clip lost buffered packets");
    expect(samples.front().keyFrame, "saved clip does not start at a keyframe");
    expect(samples.front().pts100ns == 0, "saved clip starts at the wrong timestamp");
    expect(samples.back().pts100ns == 2 * kSecond100ns, "saved clip tail is at the wrong timestamp");
    expect(samples.back().duration100ns == 2 * kSecond100ns,
        "saved clip lost the live tail extension");
    std::filesystem::remove_all(output.parent_path(), error);
}

void testConsecutiveSaveOverlap() {
    ReplayBuffer buffer;
    buffer.configure(settings(30));
    for (int64_t pts = 0; pts < 12; ++pts) {
        buffer.pushVideo(makeVideo(pts * kSecond100ns, kSecond100ns, (pts % 3) == 0));
    }
    const auto output = testOutputDir() / L"overlap.mp4";
    std::error_code error;
    std::filesystem::create_directories(output.parent_path(), error);
    VideoSettings videoSettings;
    videoSettings.codec = VideoCodec::H264;
    expect(buffer.saveTo(output, videoSettings), "first consecutive save failed");
    expect(backtrack::g_lastMuxedInputs.videoSamples.front().pts100ns == 0,
        "first save did not start at the first buffered keyframe");

    buffer.pushVideo(makeVideo(12 * kSecond100ns, kSecond100ns, false));
    buffer.pushVideo(makeVideo(13 * kSecond100ns, kSecond100ns, false));
    buffer.pushVideo(makeVideo(15 * kSecond100ns, kSecond100ns, true));
    buffer.pushVideo(makeVideo(16 * kSecond100ns, kSecond100ns, false));

    expect(buffer.saveTo(output, videoSettings), "second consecutive save failed");
    const auto& samples = backtrack::g_lastMuxedInputs.videoSamples;
    // The second clip must start no earlier than (firstSaveEnd - 3s) and must
    // include the content recorded after the first save.
    expect(samples.front().pts100ns == 9 * kSecond100ns,
        "second save started too early or too late relative to the previous clip");
    expect(samples.back().pts100ns == 16 * kSecond100ns,
        "second save dropped content recorded after the first save");
    std::filesystem::remove_all(output.parent_path(), error);
}

void testAudioTimelineMatchesVideo() {
    ReplayBuffer buffer;
    buffer.configure(settings(30));
    for (int64_t pts = 0; pts <= 6; ++pts) {
        buffer.pushVideo(makeVideo(pts * kSecond100ns, kSecond100ns, pts == 0));
    }
    // Audio covers [1,4] and [5,6]; [0,1] and [4,5] have no audio.
    buffer.pushAudio(makeAudio(kSecond100ns, 1));
    buffer.pushAudio(makeAudio(2 * kSecond100ns, 1));
    buffer.pushAudio(makeAudio(3 * kSecond100ns, 1));
    buffer.pushAudio(makeAudio(5 * kSecond100ns, 1));

    const auto output = testOutputDir() / L"audio.mp4";
    std::error_code error;
    std::filesystem::create_directories(output.parent_path(), error);
    VideoSettings videoSettings;
    videoSettings.codec = VideoCodec::H264;
    expect(buffer.saveTo(output, videoSettings), "saveTo with audio failed");
    expect(backtrack::g_lastSystemWav.has_value(), "system audio WAV was not produced");
    const auto& wav = *backtrack::g_lastSystemWav;
    expect(wav.samplesPerSec == 48000, "system audio WAV sample rate changed");
    expect(wav.blockAlign == 4, "system audio WAV block alignment changed");
    // The WAV must span [videoStart, lastAudioEnd] on the shared timeline:
    // silence before the first audio packet (video-anchored start) and silence
    // in the [4,5] gap. No audio sample may be dropped from the timeline.
    expect(wav.dataBytes == 6ull * 48000 * 4,
        "system audio WAV does not span the video timeline (gap or start misaligned)");
    std::filesystem::remove_all(output.parent_path(), error);
}

void testSaveWithoutKeyframeFails() {
    ReplayBuffer buffer;
    buffer.configure(settings(30));
    buffer.pushVideo(makeVideo(0, kSecond100ns, false));
    buffer.pushVideo(makeVideo(kSecond100ns, kSecond100ns, false));
    expect(buffer.videoSpan100ns() == 0, "keyframe-less buffer reported a muxable span");
    const auto output = testOutputDir() / L"nokeyframe.mp4";
    std::error_code error;
    std::filesystem::create_directories(output.parent_path(), error);
    VideoSettings videoSettings;
    videoSettings.codec = VideoCodec::H264;
    expect(!buffer.saveTo(output, videoSettings), "saveTo succeeded without a keyframe");
    std::filesystem::remove_all(output.parent_path(), error);
}

} // namespace

int main() {
    try {
        testKeyframeCountingAndTrim();
        testTailExtension();
        testSaveToHonorsLiveTailAndKeyframeStart();
        testConsecutiveSaveOverlap();
        testAudioTimelineMatchesVideo();
        testSaveWithoutKeyframeFails();
        std::cout << "ReplayBuffer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ReplayBuffer test failure: " << error.what() << '\n';
        return 1;
    }
}
