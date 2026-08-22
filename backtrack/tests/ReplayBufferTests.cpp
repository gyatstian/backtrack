#include "audio/WavReader.h"
#include "mux/Mp4Muxer.h"
#include "mux/RecoveryStore.h"
#include "mux/WavWriter.h"
#include "replay/ReplayBuffer.h"

#include <Windows.h>
#include <mmreg.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Link-time stub for the Media Foundation muxer; ReplayBuffer::saveTo only needs
// the sample table MP4 muxing would receive. The stub records the inputs (and
// parses the produced WAV while its temp directory still exists) for asserts.
namespace backtrack {

static MuxedInputs g_lastMuxedInputs;
static std::optional<WavInfo> g_lastSystemWav;

bool Mp4Muxer::muxToMp4(
    const MuxedInputs& inputs,
    const std::filesystem::path& /*outputPath*/,
    std::wstring* /*errorDetail*/,
    unsigned /*stagingVariant*/,
    DWORD* win32Error) {
    if (win32Error) {
        *win32Error = ERROR_SUCCESS;
    }
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

// Link-time stub matching the real muxer helper (Mp4Muxer.cpp is not linked
// into this test binary): removes only empty zero-byte reservation files.
bool removeEmptyOutputReservation(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return false;
    }
    if (std::filesystem::file_size(path, error) != 0 || error) {
        return false;
    }
    std::filesystem::remove(path, error);
    return !error;
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

void testParseWavRecoversUnpatchedCrashHeader() {
    // Crash scenario: a process killed before WavWriter::patchHeader() leaves
    // zeroed RIFF/data size fields while the payload is fully on disk. The
    // reader must fall back to the real file size instead of discarding audio.
    const auto dir = testOutputDir() / L"wavcrash";
    std::error_code error;
    std::filesystem::create_directories(dir, error);

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = 48000 * 4;
    WaveFormatBlob blob;
    const auto* formatBytes = reinterpret_cast<const uint8_t*>(&format);
    blob.bytes.assign(formatBytes, formatBytes + sizeof(format));

    constexpr size_t kPayloadBytes = 48000 * 4; // one second of stereo s16
    std::vector<uint8_t> payload(kPayloadBytes, 0x5A);
    backtrack::WavInfo parsed;
    bool parsedGoodHeader = false;

    {
        backtrack::WavWriter writer;
        expect(writer.open(dir / L"good.wav", blob), "WavWriter open failed");
        writer.write(payload.data(), payload.size());
        writer.close();
        parsedGoodHeader = parseWav(dir / L"good.wav", parsed);
    }
    expect(parsedGoodHeader, "well-formed WAV was rejected");
    expect(parsed.dataBytes == kPayloadBytes, "well-formed WAV lost payload bytes");

    // Zero the two size fields patchHeader() would have written: RIFF size at
    // offset 4 and the data chunk size right after the "data" tag (offset 36).
    std::ifstream in(dir / L"good.wav", std::ios::binary);
    expect(in.is_open(), "could not read back the WAV fixture");
    std::vector<uint8_t> image((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    expect(image.size() == 46 + kPayloadBytes, "unexpected WAV fixture layout");
    for (int i = 0; i < 4; ++i) {
        image[4 + i] = 0;   // RIFF size
        image[42 + i] = 0;  // data chunk size (fmt chunk is sizeof(WAVEFORMATEX) == 18)
    }
    const auto crashedPath = dir / L"crashed.wav";
    std::ofstream out(crashedPath, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    out.close();

    parsed = backtrack::WavInfo{};
    expect(parseWav(crashedPath, parsed), "crashed-header WAV was rejected");
    expect(parsed.dataOffset == 46, "crashed WAV data offset is wrong");
    expect(parsed.dataBytes == kPayloadBytes, "crashed WAV payload was not recovered");
    expect(parsed.samplesPerSec == 48000 && parsed.channels == 2,
        "crashed WAV format metadata changed");
}

// --- Crash-recovery manifest regression tests ---
// These exercise RecoveryStore through its public surface:
// writeManifest + recoverLatestFailedRecording, with muxToMp4 stubbed to
// capture the sample table recovery would feed the real muxer.

struct RecoveryFixture {
    std::filesystem::path clipsDir;
    std::filesystem::path sessionDir;

    static RecoveryFixture create(const wchar_t* name) {
        RecoveryFixture fixture;
        fixture.clipsDir = testOutputDir() / L"recovery" / name;
        fixture.sessionDir = fixture.clipsDir / L".backtrack_tmp" / L"session1";
        std::error_code error;
        std::filesystem::create_directories(fixture.sessionDir, error);
        expect(!error, "could not create recovery fixture directories");
        return fixture;
    }

    void dispose() {
        std::error_code error;
        std::filesystem::remove_all(clipsDir, error);
        expect(!error, "could not remove recovery fixture directories");
    }
};

constexpr size_t kPackets = 10;
constexpr uint32_t kPacketSize = 4;

MuxedInputs::VideoSample makeManifestSample(size_t index) {
    MuxedInputs::VideoSample sample;
    sample.offset = index * kPacketSize;
    sample.size = kPacketSize;
    sample.pts100ns = static_cast<int64_t>(index) * 333333;
    sample.dts100ns = sample.pts100ns;
    sample.hasDts = false;
    sample.duration100ns = 333333;
    sample.keyFrame = index == 0;
    return sample;
}

// Writes a small elementary stream of packetCount*4 distinct bytes.
std::vector<uint8_t> writeElementaryStream(const std::filesystem::path& path, size_t packetCount) {
    std::vector<uint8_t> blob(packetCount * kPacketSize);
    for (size_t i = 0; i < blob.size(); ++i) {
        blob[i] = static_cast<uint8_t>((i * 31 + 7) & 0xFF);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    expect(out.is_open(), "could not write elementary stream fixture");
    out.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
    return blob;
}

MuxedInputs makeMuxedInputs(const std::filesystem::path& videoPath, size_t packetCount) {
    MuxedInputs inputs;
    inputs.videoPath = videoPath;
    inputs.codec = VideoCodec::H264;
    inputs.fps = 60;
    inputs.width = 1920;
    inputs.height = 1080;
    inputs.bitrateKbps = 8000;
    for (size_t i = 0; i < packetCount; ++i) {
        inputs.videoSamples.push_back(makeManifestSample(i));
    }
    return inputs;
}

void writeTextFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    expect(out.is_open(), "could not write manifest fixture");
    out << text;
}

std::string manifestSampleText(size_t index, uint64_t offsetOverride) {
    const auto sample = makeManifestSample(index);
    std::string line = "sample\t" + std::to_string(offsetOverride);
    line += '\t' + std::to_string(sample.size);
    line += '\t' + std::to_string(sample.pts100ns);
    line += '\t' + std::to_string(sample.duration100ns);
    line += '\t' + std::string(sample.keyFrame ? "1" : "0");
    line += '\t' + std::to_string(sample.dts100ns);
    line += '\t' + std::string(sample.hasDts ? "1" : "0");
    line += '\n';
    return line;
}

void expectRecoveredSamples(
    RecoveryFixture& fixture,
    size_t expectedCount,
    const std::string& context) {
    auto narrow = [](const std::wstring& wide) {
        std::string result;
        result.reserve(wide.size());
        for (wchar_t character : wide) {
            result.push_back(character >= 0 && character <= 0x7F ? static_cast<char>(character) : '?');
        }
        return result;
    };
    std::wstring detail;
    const auto recovered =
        backtrack::RecoveryStore::recoverLatestFailedRecording(fixture.clipsDir, detail);
    expect(!recovered.empty(), context + ": recovery did not produce a clip (" +
        narrow(detail) + ")");
    const auto& samples = backtrack::g_lastMuxedInputs.videoSamples;
    expect(samples.size() == expectedCount,
        context + ": expected " + std::to_string(expectedCount) + " samples, got " +
        std::to_string(samples.size()));
    if (!samples.empty()) {
        expect(samples.front().offset == 0, context + ": first sample offset is not 0");
        expect(samples.front().pts100ns == 0, context + ": first sample pts is not 0");
    }
}

void testRecoveryManifestRoundTripIsAtomic() {
    RecoveryFixture fixture = RecoveryFixture::create(L"roundtrip");
    writeElementaryStream(fixture.sessionDir / L"replay.h264", kPackets);

    MuxedInputs inputs = makeMuxedInputs(fixture.sessionDir / L"replay.h264", kPackets);
    expect(backtrack::RecoveryStore::writeManifest(inputs, fixture.sessionDir),
        "writeManifest failed for a valid table");
    const auto stagingPath = fixture.sessionDir /
        (std::wstring(backtrack::RecoveryStore::kRecoveryManifestName) + L".tmp");
    expect(!std::filesystem::exists(stagingPath),
        "atomic manifest publish left its staging file behind");

    expectRecoveredSamples(fixture, kPackets, "round trip");
    expect(backtrack::g_lastMuxedInputs.videoSamples.back().offset ==
               (kPackets - 1) * kPacketSize,
        "round trip changed the last sample offset");
    fixture.dispose();
}

void testRecoveryTrimsSamplesBeyondStreamEnd() {
    // Flush-desync crash: the incremental manifest already lists samples whose
    // bytes were still in the elementary stream's write buffer when the
    // process died. Recovery must drop that tail instead of failing.
    RecoveryFixture fixture = RecoveryFixture::create(L"trimtail");
    // Stream holds only the first N packets; manifest claims N+2.
    writeElementaryStream(fixture.sessionDir / L"replay.h264", kPackets);

    MuxedInputs inputs = makeMuxedInputs(fixture.sessionDir / L"replay.h264", kPackets + 2);
    expect(backtrack::RecoveryStore::writeManifest(inputs, fixture.sessionDir),
        "writeManifest failed for the overlong table");

    expectRecoveredSamples(fixture, kPackets, "unflushed-tail trim");
    fixture.dispose();
}

void testRecoveryKeepsPrefixOnTornFinalLine() {
    // A final sample line torn mid-columns must not reject the whole session.
    RecoveryFixture fixture = RecoveryFixture::create(L"torntail");
    writeElementaryStream(fixture.sessionDir / L"replay.h264", kPackets);

    MuxedInputs inputs = makeMuxedInputs(fixture.sessionDir / L"replay.h264", kPackets);
    expect(backtrack::RecoveryStore::writeManifest(inputs, fixture.sessionDir),
        "writeManifest failed before tearing the tail");

    {
        std::ofstream append(
            fixture.sessionDir / backtrack::RecoveryStore::kRecoveryManifestName,
            std::ios::binary | std::ios::app);
        expect(append.is_open(), "could not open manifest for appending a torn line");
        append << "sample\t40\t"; // torn mid-line, no newline
    }

    expectRecoveredSamples(fixture, kPackets, "torn final line");
    fixture.dispose();
}

void testRecoveryRejectsPlausibleNonContiguousTail() {
    // Regression for prefix parsing: a torn number could previously parse as a
    // smaller-but-plausible offset ("4000000" from "40000001") and make the
    // muxer read wrong bytes. Structural validation must drop it while keeping
    // the valid prefix.
    RecoveryFixture fixture = RecoveryFixture::create(L"noncontiguous");
    writeElementaryStream(fixture.sessionDir / L"replay.h264", kPackets);

    MuxedInputs inputs = makeMuxedInputs(fixture.sessionDir / L"replay.h264", kPackets);
    expect(backtrack::RecoveryStore::writeManifest(inputs, fixture.sessionDir),
        "writeManifest failed before appending the bogus line");

    {
        std::ofstream append(
            fixture.sessionDir / backtrack::RecoveryStore::kRecoveryManifestName,
            std::ios::binary | std::ios::app);
        expect(append.is_open(), "could not open manifest for appending a bogus line");
        // Well-formed columns whose offset breaks the contiguity chain while
        // still pointing INSIDE the elementary stream: only structural
        // validation (not the end-of-stream trim) can catch this. The true
        // next offset would be kPackets * kPacketSize.
        append << manifestSampleText(kPackets, (kPackets - 3) * kPacketSize);
    }

    expectRecoveredSamples(fixture, kPackets, "non-contiguous tail");
    fixture.dispose();
}

void testRecoveryAcceptsFewerLinesThanDeclared() {
    // Belt-and-suspenders for atomic publish: a rewrite interrupted mid-write
    // may leave fewer sample lines than declared. Salvage what is present.
    RecoveryFixture fixture = RecoveryFixture::create(L"shorttable");
    writeElementaryStream(fixture.sessionDir / L"replay.h264", kPackets);

    std::string text = "backtrack-recovery-v1\n";
    text += "codec\th264\nfps\t60\nwidth\t1920\nheight\t1080\nbitrateKbps\t8000\n";
    text += "video\treplay.h264\n";
    text += "samples\t" + std::to_string(kPackets + 3) + "\n";
    for (size_t i = 0; i < kPackets; ++i) {
        text += manifestSampleText(i, i * kPacketSize);
    }
    writeTextFile(fixture.sessionDir / backtrack::RecoveryStore::kRecoveryManifestName, text);

    expectRecoveredSamples(fixture, kPackets, "declared-count shortfall");
    fixture.dispose();
}

void testRecoveryStillRejectsDamagedMetadata() {
    // Tolerance applies only to the sample tail; a damaged header value means
    // the whole file is untrustworthy and must not be recovered.
    RecoveryFixture fixture = RecoveryFixture::create(L"badmeta");
    writeElementaryStream(fixture.sessionDir / L"replay.h264", kPackets);

    std::string text = "backtrack-recovery-v1\n";
    text += "codec\th264\nfps\t6abc0\nwidth\t1920\nheight\t1080\nbitrateKbps\t8000\n";
    text += "video\treplay.h264\n";
    text += "samples\t" + std::to_string(kPackets) + "\n";
    for (size_t i = 0; i < kPackets; ++i) {
        text += manifestSampleText(i, i * kPacketSize);
    }
    writeTextFile(fixture.sessionDir / backtrack::RecoveryStore::kRecoveryManifestName, text);

    std::wstring detail;
    const auto recovered =
        backtrack::RecoveryStore::recoverLatestFailedRecording(fixture.clipsDir, detail);
    expect(recovered.empty(), "manifest with damaged metadata was recovered anyway");
    fixture.dispose();
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
        testParseWavRecoversUnpatchedCrashHeader();
        testRecoveryManifestRoundTripIsAtomic();
        testRecoveryTrimsSamplesBeyondStreamEnd();
        testRecoveryKeepsPrefixOnTornFinalLine();
        testRecoveryRejectsPlausibleNonContiguousTail();
        testRecoveryAcceptsFewerLinesThanDeclared();
        testRecoveryStillRejectsDamagedMetadata();
        std::cout << "ReplayBuffer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ReplayBuffer test failure: " << error.what() << '\n';
        return 1;
    }
}
