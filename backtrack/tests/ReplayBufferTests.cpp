#include "audio/WavReader.h"
#include "encoder/BitstreamUtils.h"
#include "mux/Mp4Muxer.h"
#include "mux/RecoveryStore.h"
#include "mux/WavWriter.h"
#include "replay/ReplayBuffer.h"

#include <Windows.h>
#include <mmreg.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Link-time stub for the Media Foundation muxer; ReplayBuffer::saveTo only needs
// the sample table MP4 muxing would receive. The stub records the inputs (and
// parses the produced WAV while its temp directory still exists) for asserts.
namespace backtrack {

static MuxedInputs g_lastMuxedInputs;
static std::optional<WavInfo> g_lastSystemWav;
static std::optional<WavInfo> g_lastMicrophoneWav;
static bool g_captureAudioBytes = false;
static std::vector<uint8_t> g_lastSystemAudioBytes;
static std::vector<uint8_t> g_lastMicrophoneAudioBytes;
// Snapshot of the elementary stream at mux time: saveTo deletes its staging
// directory right after the stub returns, so integrity checks must copy.
static std::vector<char> g_lastVideoStreamBytes;
// When set, the stub reports a muxing failure so saveTo's failure branch is
// reachable; the real muxer returning false is otherwise never exercised.
static bool g_muxShouldFail = false;
// Recovery muxes an owned work copy of the streams, so the input path is not
// the session identity any more; the recovered output name is.
static std::filesystem::path g_muxFailOutputName;
static std::vector<std::filesystem::path> g_muxedVideoPaths;
static std::vector<std::filesystem::path> g_muxedOutputPaths;
static std::vector<size_t> g_muxedSampleCounts;
static std::wstring g_muxFailureDetail;

static void captureWav(const std::filesystem::path& path, std::optional<WavInfo>& wav,
                       std::vector<uint8_t>& bytes) {
    wav.reset();
    bytes.clear();
    WavInfo info;
    if (path.empty() || !parseWav(path, info)) {
        return;
    }
    wav = info;
    if (g_captureAudioBytes) {
        std::ifstream stream(path, std::ios::binary);
        stream.seekg(static_cast<std::streamoff>(info.dataOffset));
        bytes.resize(static_cast<size_t>(info.dataBytes));
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            bytes.clear();
        }
    }
}

bool Mp4Muxer::muxToMp4(
    const MuxedInputs& inputs,
    const std::filesystem::path& outputPath,
    std::wstring* errorDetail,
    unsigned /*stagingVariant*/,
    DWORD* win32Error) {
    if (win32Error) {
        *win32Error = ERROR_SUCCESS;
    }
    g_lastMuxedInputs = inputs;
    g_muxedVideoPaths.push_back(inputs.videoPath);
    g_muxedOutputPaths.push_back(outputPath);
    g_muxedSampleCounts.push_back(inputs.videoSamples.size());
    g_lastSystemWav.reset();
    g_lastMicrophoneWav.reset();
    g_lastSystemAudioBytes.clear();
    g_lastMicrophoneAudioBytes.clear();
    g_lastVideoStreamBytes.clear();
    if (g_muxShouldFail ||
        (!g_muxFailOutputName.empty() && outputPath.filename() == g_muxFailOutputName)) {
        if (errorDetail) {
            *errorDetail = g_muxFailureDetail;
        }
        return false;
    }
    if (!inputs.videoPath.empty()) {
        std::ifstream stream(inputs.videoPath, std::ios::binary);
        if (stream.is_open()) {
            g_lastVideoStreamBytes.assign(
                std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        }
    }
    captureWav(inputs.systemAudioPath, g_lastSystemWav, g_lastSystemAudioBytes);
    captureWav(inputs.microphoneAudioPath, g_lastMicrophoneWav, g_lastMicrophoneAudioBytes);
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

// Grants the suite access to ReplayBuffer's private fault-injection hook. The
// app never uses it; it exists so the save path's ring-read failure - the one
// that used to publish a silently truncated clip - can be tested at all.
struct ReplayBufferTestAccess {
    static void breakRing(ReplayBuffer& buffer) {
        buffer.breakRingForTesting();
    }
    static void breakAudioRing(ReplayBuffer& buffer) {
        buffer.breakAudioRingForTesting();
    }
    static uint64_t ramVideoBytes(const ReplayBuffer& buffer) {
        return buffer.ramVideoBytes_;
    }
    static uint64_t ramAudioBytes(const ReplayBuffer& buffer) {
        return buffer.ramAudioBytes_;
    }
    static uint64_t diskAudioBytes(const ReplayBuffer& buffer) {
        return buffer.audioRing_.liveBytes();
    }
};

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

// Failure messages are ASCII in practice; anything else becomes '?' so a
// locale-dependent assertion message cannot itself break the suite.
std::string narrowForTest(const std::wstring& wide) {
    std::string result;
    result.reserve(wide.size());
    for (wchar_t character : wide) {
        result.push_back(character >= 0 && character <= 0x7F ? static_cast<char>(character) : '?');
    }
    return result;
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
    // Tests pick the backend explicitly, independent of the app default.
    result.diskBackedVideo = false;
    result.diskBackedAudio = false;
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

void testConfiguredConsecutiveSaveOverlap() {
    for (const bool diskBacked : {false, true}) {
        for (const uint32_t overlapSeconds : {0u, 5u, 3600u}) {
            const auto root = testOutputDir() / L"configured_overlap";
            std::error_code error;
            std::filesystem::remove_all(root, error);
            std::filesystem::create_directories(root);
            {
                ReplayBuffer buffer;
                auto replaySettings = settings(30);
                replaySettings.diskBackedVideo = diskBacked;
                buffer.configure(replaySettings, root, 8ULL * 1024 * 1024);
                for (int64_t pts = 0; pts < 12; ++pts) {
                    buffer.pushVideo(makeVideo(pts * kSecond100ns, kSecond100ns, pts % 3 == 0));
                }
                const auto output = root / L"clip.mp4";
                VideoSettings videoSettings;
                videoSettings.codec = VideoCodec::H264;
                expect(buffer.saveTo(output, videoSettings), "initial configured-overlap save failed");

                replaySettings.overlapSeconds = overlapSeconds;
                buffer.configure(replaySettings, root, 8ULL * 1024 * 1024);
                for (int64_t pts = 12; pts < 18; ++pts) {
                    buffer.pushVideo(makeVideo(pts * kSecond100ns, kSecond100ns, pts % 3 == 0));
                }
                const int64_t expectedStart = overlapSeconds == 0 ? 12 : overlapSeconds == 5 ? 6 : 0;
                backtrack::g_muxShouldFail = true;
                const bool failedSave = buffer.saveTo(output, videoSettings);
                backtrack::g_muxShouldFail = false;
                expect(!failedSave, "injected configured-overlap save did not fail");
                expect(backtrack::g_lastMuxedInputs.videoSamples.front().pts100ns == expectedStart * kSecond100ns,
                    "configured overlap did not align to the expected keyframe");

                buffer.pushVideo(makeVideo(18 * kSecond100ns, kSecond100ns, true));
                expect(buffer.saveTo(output, videoSettings), "configured-overlap retry failed");
                const auto& samples = backtrack::g_lastMuxedInputs.videoSamples;
                expect(samples.front().pts100ns == expectedStart * kSecond100ns,
                    "failed save advanced consecutive-save accounting");
                expect(samples.front().keyFrame && samples.back().pts100ns == 18 * kSecond100ns,
                    "configured overlap lost keyframe alignment or the live tail");
            }
            std::filesystem::remove_all(root, error);
        }
    }
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
    auto narrow = [](const std::wstring& wide) { return narrowForTest(wide); };
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

void testSessionDirectoriesAreExclusivelyCreated() {
    RecoveryFixture fixture = RecoveryFixture::create(L"exclusive_sessions");
    const auto marker = fixture.sessionDir / L"existing.txt";
    writeTextFile(marker, "original");
    constexpr size_t count = 8;
    std::vector<std::filesystem::path> paths(count);
    std::vector<std::error_code> errors(count);
    std::vector<std::thread> threads;
    for (size_t i = 0; i < count; ++i) {
        threads.emplace_back([&, i] {
            paths[i] = backtrack::RecoveryStore::createUniqueDirectory(fixture.sessionDir, errors[i]);
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (size_t i = 0; i < count; ++i) {
        expect(!errors[i] && !paths[i].empty(), "could not create a unique session");
        expect(paths[i] != fixture.sessionDir, "reused an existing session");
        expect(std::find(paths.begin(), paths.begin() + i, paths[i]) == paths.begin() + i,
            "concurrent saves claimed the same session");
    }
    std::ifstream stream(marker);
    std::string value;
    stream >> value;
    stream.close();
    expect(value == "original", "session creation changed existing data");
    fixture.dispose();
}

void testFailedManifestRewriteKeepsIncrementalPrefix() {
    RecoveryFixture fixture = RecoveryFixture::create(L"writefailure");
    const auto videoPath = fixture.sessionDir / L"video.h264";
    writeElementaryStream(videoPath, kPackets);
    const auto incremental = makeMuxedInputs(videoPath, kPackets + 2);
    expect(backtrack::RecoveryStore::writeManifest(incremental, fixture.sessionDir),
        "could not write incremental manifest fixture");
    const auto manifestPath = fixture.sessionDir / backtrack::RecoveryStore::kRecoveryManifestName;
    std::ifstream beforeFile(manifestPath, std::ios::binary);
    const std::string before((std::istreambuf_iterator<char>(beforeFile)), std::istreambuf_iterator<char>());
    beforeFile.close();

    std::error_code error;
    std::filesystem::create_directory(fixture.sessionDir / L"recovery.tsv.tmp", error);
    expect(!error, "could not block the manifest rewrite");
    backtrack::g_muxShouldFail = true;
    std::wstring detail;
    const auto result = backtrack::RecoveryStore::publishMuxedInputs(
        incremental, fixture.clipsDir / L"session1.mp4", fixture.sessionDir, &detail);
    backtrack::g_muxShouldFail = false;
    expect(result == backtrack::RecoveryStore::MuxPublishResult::kPreserved,
        "failed rewrite discarded the incremental recovery manifest");
    expect(detail.find(L"Recover Failed") != std::wstring::npos,
        "usable prefix was not reported as recoverable");
    std::ifstream afterFile(manifestPath, std::ios::binary);
    const std::string after((std::istreambuf_iterator<char>(afterFile)), std::istreambuf_iterator<char>());
    afterFile.close();
    expect(after == before, "failed rewrite changed the incremental manifest");
    expectRecoveredSamples(fixture, kPackets, "failed rewrite and missing tail");
    fixture.dispose();
}

void testFailedMuxWithoutManifestKeepsStreams() {
    RecoveryFixture fixture = RecoveryFixture::create(L"manual_recovery");
    const auto videoPath = fixture.sessionDir / L"video.h264";
    const auto original = writeElementaryStream(videoPath, kPackets);
    std::error_code error;
    std::filesystem::create_directory(fixture.sessionDir / L"recovery.tsv.tmp", error);
    expect(!error, "could not block the recovery manifest");
    backtrack::g_muxShouldFail = true;
    std::wstring detail;
    const auto result = backtrack::RecoveryStore::publishMuxedInputs(
        makeMuxedInputs(videoPath, kPackets), fixture.clipsDir / L"session1.mp4", fixture.sessionDir, &detail);
    backtrack::g_muxShouldFail = false;
    expect(result == backtrack::RecoveryStore::MuxPublishResult::kPreserved,
        "failed mux did not preserve streams without a manifest");
    expect(detail.find(L"manual recovery") != std::wstring::npos,
        "missing manifest was not reported as a manual recovery case");
    std::ifstream stream(videoPath, std::ios::binary);
    const std::vector<uint8_t> after((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    stream.close();
    expect(after == original, "failed mux changed streams without a manifest");
    fixture.dispose();
}

void testStartupRecoversAllValidSessionsAndKeepsFailures() {
    RecoveryFixture fixture = RecoveryFixture::create(L"startup_multi");
    const auto root = fixture.sessionDir.parent_path();
    const auto failedDir = fixture.sessionDir;
    const auto firstDir = root / L"session2";
    const auto secondDir = root / L"session3";
    const auto unknownDir = root / L"unknown";
    std::error_code error;
    for (const auto& dir : {firstDir, secondDir, unknownDir}) {
        std::filesystem::create_directory(dir, error);
        expect(!error, "could not create startup recovery fixture");
    }
    std::vector<uint8_t> failedBytes;
    for (const auto& dir : {failedDir, firstDir, secondDir}) {
        const auto video = dir / L"video.h264";
        const auto bytes = writeElementaryStream(video, kPackets);
        if (dir == failedDir) {
            failedBytes = bytes;
        }
        const auto samples = dir == secondDir ? kPackets + 2 : kPackets;
        expect(backtrack::RecoveryStore::writeManifest(makeMuxedInputs(video, samples), dir),
            "could not write startup recovery manifest");
    }
    writeElementaryStream(unknownDir / L"video.h264", kPackets);
    const auto old = std::filesystem::file_time_type::clock::now() - std::chrono::hours(48);
    for (const auto& dir : {failedDir, firstDir, secondDir, unknownDir}) {
        std::filesystem::last_write_time(dir, old, error);
        expect(!error, "could not age startup recovery directory");
    }
    for (const auto& dir : {failedDir, firstDir, secondDir}) {
        std::filesystem::last_write_time(dir / backtrack::RecoveryStore::kRecoveryManifestName, old, error);
        expect(!error, "could not age startup recovery manifest");
    }
    std::filesystem::last_write_time(
        failedDir / backtrack::RecoveryStore::kRecoveryManifestName, old + std::chrono::hours(2), error);
    expect(!error, "could not make the failed manifest newest");
    backtrack::g_muxedVideoPaths.clear();
    backtrack::g_muxedOutputPaths.clear();
    backtrack::g_muxedSampleCounts.clear();
    backtrack::g_muxFailOutputName = L"session1.mp4";
    backtrack::g_muxFailureDetail = L"simulated startup failure";
    backtrack::RecoveryStore::sweepOrphanedRecoveryData(fixture.clipsDir);
    backtrack::g_muxFailOutputName.clear();
    backtrack::g_muxFailureDetail.clear();

    expect(std::filesystem::exists(failedDir / L"video.h264"), "startup deleted the failed session");
    expect(std::filesystem::exists(failedDir / backtrack::RecoveryStore::kRecoveryManifestName),
        "startup deleted the failed session manifest");
    std::ifstream failedStream(failedDir / L"video.h264", std::ios::binary);
    const std::vector<uint8_t> afterFailure(
        (std::istreambuf_iterator<char>(failedStream)), std::istreambuf_iterator<char>());
    failedStream.close();
    expect(afterFailure == failedBytes, "startup changed the failed session's video");
    expect(std::filesystem::exists(unknownDir / L"video.h264"), "startup deleted unrecognized data");
    // Recovery muxes from an exclusively created work copy, so the original
    // sessions and their streams are retained; the sweep only adds the clips.
    expect(std::filesystem::exists(firstDir) && std::filesystem::exists(secondDir),
        "startup recovery deleted a valid session's streams");
    expect(std::count(backtrack::g_muxedOutputPaths.begin(), backtrack::g_muxedOutputPaths.end(),
                      fixture.clipsDir / L"session1.mp4") == 4,
        "startup did not make one bounded recovery attempt for the failed session");
    for (const auto& dir : {firstDir, secondDir}) {
        const auto output = fixture.clipsDir / (dir.filename().wstring() + L".mp4");
        const auto it = std::find(backtrack::g_muxedOutputPaths.begin(), backtrack::g_muxedOutputPaths.end(),
                                  output);
        expect(it != backtrack::g_muxedOutputPaths.end(), "startup skipped a valid session");
        expect(std::count(backtrack::g_muxedOutputPaths.begin(), backtrack::g_muxedOutputPaths.end(),
                          output) == 1,
            "startup recovered a valid session more than once");
        expect(std::filesystem::exists(output), "startup did not publish a recovered session");
        const auto index = static_cast<size_t>(it - backtrack::g_muxedOutputPaths.begin());
        expect(backtrack::g_muxedSampleCounts[index] == kPackets,
            "startup failed to trim a missing stream tail");
    }
    expectRecoveredSamples(fixture, kPackets, "retry after failed startup recovery");
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

EncodedPacket makeVideoPayload(
    int64_t pts100ns, int64_t duration100ns, bool keyFrame, uint8_t fill,
    size_t payloadBytes = 1024 * 1024 + 12345) {
    EncodedPacket packet = makeVideo(pts100ns, duration100ns, keyFrame);
    // Payload deliberately does not divide the ring capacity so packets
    // genuinely straddle the wrap boundary (two-extent split).
    packet.bytes.assign(payloadBytes, fill);
    return packet;
}

// Annex B SPS + PPS followed by filler. Only the start codes and NAL type
// bytes are inspected by BitstreamUtils, so this is a valid parameter-set
// payload for the cut-safety and header-prepending logic.
std::vector<uint8_t> parameterSetPayload(size_t payloadBytes = 512) {
    const uint8_t header[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1E,  // SPS
        0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80}; // PPS
    std::vector<uint8_t> bytes(header, header + sizeof(header));
    // 0x41 never forms a start code, so the filler cannot fake a NAL unit.
    bytes.resize(payloadBytes, 0x41);
    return bytes;
}

// Runs `body` against both backends so the disk and RAM paths of a behavior
// cannot drift apart.
template <typename Body>
void forEachBackend(const std::filesystem::path& storageRoot, Body&& body) {
    ReplaySettings diskSettings = settings(30);
    diskSettings.diskBackedVideo = true;
    ReplayBuffer disk;
    disk.configure(diskSettings, storageRoot, 8ull * 1024 * 1024);
    body(disk);

    ReplayBuffer ram;
    ram.configure(settings(30));
    body(ram);
}

VideoSettings defaultVideoSettings() {
    VideoSettings videoSettings;
    videoSettings.codec = VideoCodec::H264;
    videoSettings.fps = 60;
    videoSettings.width = 1920;
    videoSettings.height = 1080;
    return videoSettings;
}

// The ring capacity is derived from replay.seconds, so editing the replay
// window used to arrive here as a capacity change and wipe the buffer: the
// switch path cleared and closed the ring, and closeRingLocked deleted the
// file. A capacity edit must keep the buffered video; only a backend or
// directory switch may drop it.
void testDiskRingSurvivesCapacityChange() {
    const auto storageRoot = testOutputDir() / L"ring_capacity_change";
    std::error_code error;
    std::filesystem::remove_all(storageRoot, error);
    std::filesystem::create_directories(storageRoot, error);

    ReplaySettings diskSettings = settings(30);
    diskSettings.diskBackedVideo = true;
    ReplayBuffer buffer;
    buffer.configure(diskSettings, storageRoot, 8ull * 1024 * 1024);
    for (int64_t pts = 0; pts < 6; ++pts) {
        // Distinct fill per packet so a read-back check can catch reordering,
        // not just a truncated or zeroed buffer.
        buffer.pushVideo(makeVideoPayload(
            pts * kSecond100ns, kSecond100ns, pts == 0, static_cast<uint8_t>(0x50 + pts), 1024));
    }
    expect(buffer.videoPacketCount() == 6, "disk ring did not buffer the warm-up packets");
    const auto ringPath = storageRoot / L".backtrack_ring" / L"replay_video.bin";
    expect(std::filesystem::exists(ringPath, error), "ring file was not created");

    // Grow, then shrink: both are what a replay-window edit produces. Every
    // value stays at or above kMinRingCapacityBytes, because a capacity below
    // it is not a capacity change at all - it turns disk backing off.
    buffer.configure(diskSettings, storageRoot, 32ull * 1024 * 1024);
    expect(buffer.videoPacketCount() == 6, "growing the ring capacity wiped the buffer");
    expect(std::filesystem::exists(ringPath, error), "growing the ring capacity deleted the ring file");
    buffer.configure(diskSettings, storageRoot, 12ull * 1024 * 1024);
    expect(buffer.videoPacketCount() == 6, "shrinking the ring capacity wiped the buffer");
    expect(buffer.storageFallbackReason().empty(),
        "a capacity-only change reported a storage fallback");
    // A non-empty ring keeps its old size, because re-laying it out would
    // invalidate every live extent.
    expect(std::filesystem::file_size(ringPath, error) == 8ull * 1024 * 1024,
        "a capacity change re-laid out a ring that still held packets");

    // The point of keeping the buffer: the retained extents must still read
    // back, byte for byte. A wipe would pass a count-only check.
    const auto retainedOutput = testOutputDir() / L"retained.mp4";
    expect(buffer.saveTo(retainedOutput, defaultVideoSettings()),
        "saving after a capacity change failed");
    const auto& samples = backtrack::g_lastMuxedInputs.videoSamples;
    expect(samples.size() == 6, "saving after a capacity change lost packets");
    const auto& stream = backtrack::g_lastVideoStreamBytes;
    for (size_t index = 0; index < samples.size(); ++index) {
        const auto& sample = samples[index];
        const auto expectedFill = static_cast<uint8_t>(0x50 + static_cast<int64_t>(index));
        expect(static_cast<size_t>(sample.offset) + sample.size <= stream.size(),
            "a retained sample ran past the elementary stream");
        expect(std::all_of(
                   stream.begin() + static_cast<std::ptrdiff_t>(sample.offset),
                   stream.begin() + static_cast<std::ptrdiff_t>(sample.offset + sample.size),
                   [expectedFill](char c) { return static_cast<uint8_t>(c) == expectedFill; }),
            "a retained extent read back corrupted bytes");
    }

    // The new window length must still take effect: settings_.seconds drives
    // the age trim, which is what actually shapes the buffer.
    ReplaySettings shorter = diskSettings;
    shorter.seconds = 2;
    buffer.configure(shorter, storageRoot, 12ull * 1024 * 1024);
    buffer.pushVideo(makeVideoPayload(6 * kSecond100ns, kSecond100ns, true, 0x50, 1024));
    buffer.pushVideo(makeVideoPayload(7 * kSecond100ns, kSecond100ns, false, 0x50, 1024));
    expect(buffer.videoPacketCount() <= 4,
        "the shortened replay window did not trim the buffer");

    // Once the buffer drains, the deferred resize can be applied safely. The
    // ring file is pre-sized to the capacity, so its length is observable.
    buffer.clear();
    expect(buffer.videoPacketCount() == 0, "clear did not drain the buffer");
    buffer.configure(diskSettings, storageRoot, 24ull * 1024 * 1024);
    expect(std::filesystem::file_size(ringPath, error) == 24ull * 1024 * 1024,
        "the deferred capacity change was never applied after the buffer drained");

    buffer.clear();
    std::filesystem::remove_all(storageRoot, error);
}

// An unusable ring folder used to degrade to RAM with only a log line, leaving
// the checkbox ticked. The buffer must keep working in RAM, but say so.
void testRingOpenFailureReportsFallback() {
    const auto storageRoot = testOutputDir() / L"ring_open_failure";
    std::error_code error;
    std::filesystem::remove_all(storageRoot, error);
    std::filesystem::create_directories(storageRoot, error);
    // A regular file where the ring folder belongs: create_directories fails.
    const auto blocker = storageRoot / L".backtrack_ring";
    {
        std::ofstream block(blocker, std::ios::binary | std::ios::trunc);
        expect(block.is_open(), "could not create the ring-folder blocker file");
    }

    ReplaySettings diskSettings = settings(30);
    diskSettings.diskBackedVideo = true;
    ReplayBuffer buffer;
    buffer.configure(diskSettings, storageRoot, 8ull * 1024 * 1024);
    expect(!buffer.storageFallbackReason().empty(),
        "a failed ring open was not reported to the user");
    for (int64_t pts = 0; pts < 4; ++pts) {
        buffer.pushVideo(makeVideoPayload(pts * kSecond100ns, kSecond100ns, pts == 0, 0x70, 1024));
    }
    expect(buffer.videoPacketCount() == 4,
        "the buffer stopped working after falling back to memory");
    const auto output = testOutputDir() / L"ring_open_failure.mp4";
    expect(buffer.saveTo(output, defaultVideoSettings()),
        "a save failed after falling back to memory");

    // Clearing the obstruction lets the ring come back, and the report clears.
    std::filesystem::remove(blocker, error);
    buffer.configure(diskSettings, storageRoot, 8ull * 1024 * 1024);
    expect(buffer.storageFallbackReason().empty(),
        "a recovered ring kept reporting a storage fallback");

    buffer.clear();
    std::filesystem::remove_all(storageRoot, error);
}

// A packet larger than the whole ring can never fit and must be dropped
// without wedging the buffer.
void testRingRejectsPacketLargerThanCapacity() {
    const auto storageRoot = testOutputDir() / L"ring_oversized_packet";
    std::error_code error;
    std::filesystem::remove_all(storageRoot, error);
    std::filesystem::create_directories(storageRoot, error);

    ReplaySettings diskSettings = settings(30);
    diskSettings.diskBackedVideo = true;
    ReplayBuffer buffer;
    constexpr uint64_t kRingBytes = 8ull * 1024 * 1024;
    buffer.configure(diskSettings, storageRoot, kRingBytes);
    buffer.pushVideo(makeVideoPayload(0, kSecond100ns, true, 0x80, 1024));
    buffer.pushVideo(makeVideoPayload(
        kSecond100ns, kSecond100ns, false, 0x80, static_cast<size_t>(kRingBytes) + 4096));
    expect(buffer.videoPacketCount() == 1,
        "an oversized packet was not dropped, or dropped a buffered packet");
    expect(buffer.storageFallbackReason().empty(),
        "an oversized packet was misreported as a storage failure");

    buffer.clear();
    std::filesystem::remove_all(storageRoot, error);
}

// hasDts/dts100ns were false/zero in every payload, so the disk backend's
// metadata copy was never exercised with a real decode timestamp.
void testDiskBackendPreservesDecodeTimestamps() {
    const auto storageRoot = testOutputDir() / L"ring_dts";
    std::error_code error;
    std::filesystem::remove_all(storageRoot, error);
    std::filesystem::create_directories(storageRoot, error);

    std::vector<EncodedPacket> packets;
    for (int64_t pts = 0; pts < 6; ++pts) {
        EncodedPacket packet = makeVideoPayload(
            pts * kSecond100ns, kSecond100ns, pts == 0, static_cast<uint8_t>(0x90 + pts), 2048);
        // B-frame reordering: every second packet carries a real DTS.
        packet.hasDts = (pts % 2) == 1;
        packet.dts100ns = packet.hasDts ? packet.pts100ns - 500000 : 0;
        packets.push_back(packet);
    }
    const auto push = [&packets](ReplayBuffer& buffer) {
        for (const auto& packet : packets) {
            buffer.pushVideo(packet);
        }
    };

    ReplaySettings diskSettings = settings(30);
    diskSettings.diskBackedVideo = true;
    ReplayBuffer disk;
    disk.configure(diskSettings, storageRoot, 8ull * 1024 * 1024);
    push(disk);
    const auto diskOutput = testOutputDir() / L"dts_disk.mp4";
    expect(disk.saveTo(diskOutput, defaultVideoSettings()), "disk save failed");
    const auto diskSamples = backtrack::g_lastMuxedInputs.videoSamples;

    ReplayBuffer ram;
    ram.configure(settings(30));
    push(ram);
    const auto ramOutput = testOutputDir() / L"dts_ram.mp4";
    expect(ram.saveTo(ramOutput, defaultVideoSettings()), "RAM save failed");
    const auto ramSamples = backtrack::g_lastMuxedInputs.videoSamples;

    expect(diskSamples.size() == ramSamples.size(), "backends disagree on the sample count");
    bool sawDecodeTimestamp = false;
    for (size_t index = 0; index < diskSamples.size(); ++index) {
        const auto& disk = diskSamples[index];
        const auto& ramSample = ramSamples[index];
        expect(disk.hasDts == ramSample.hasDts && disk.dts100ns == ramSample.dts100ns,
            "the disk backend lost the decode timestamp");
        if (disk.hasDts) {
            sawDecodeTimestamp = true;
            expect(disk.dts100ns == disk.pts100ns - 500000,
                "the saved decode timestamp was not the pushed one");
        }
    }
    expect(sawDecodeTimestamp, "no sample carried a decode timestamp, so nothing was checked");

    std::filesystem::remove_all(storageRoot, error);
    std::filesystem::remove_all(testOutputDir(), error);
}

// Annex B start code followed by a single NAL header byte. Only the start
// codes and the header are inspected, so no slice payload is needed.
std::vector<uint8_t> annexBNalUnit(const std::vector<uint8_t>& startCode, uint8_t header) {
    std::vector<uint8_t> bytes(startCode);
    bytes.push_back(header);
    return bytes;
}

// A driver can emit an IDR with no SPS/PPS. The cut must still start at a
// frame a decoder can bootstrap from, on both backends.
void testParameterSetsArePrependedOnBothBackends() {
    const auto storageRoot = testOutputDir() / L"ring_parametersets";
    std::error_code error;
    std::filesystem::remove_all(storageRoot, error);
    std::filesystem::create_directories(storageRoot, error);

    // Packet 0 is the cut point and carries no parameter sets; packet 1 is a
    // keyframe that does, so its sets get cached for the prepend.
    forEachBackend(storageRoot, [](ReplayBuffer& buffer) {
        EncodedPacket headerless = makeVideoPayload(
            0, kSecond100ns, true, 0x41, 512);
        buffer.pushVideo(headerless);

        EncodedPacket withSets = makeVideoPayload(kSecond100ns, kSecond100ns, true, 0x41, 512);
        withSets.bytes = parameterSetPayload();
        buffer.pushVideo(withSets);

        buffer.pushVideo(makeVideoPayload(2 * kSecond100ns, kSecond100ns, false, 0x41, 512));
        buffer.pushVideo(makeVideoPayload(3 * kSecond100ns, kSecond100ns, false, 0x41, 512));

        const auto output = testOutputDir() / L"parametersets.mp4";
        std::wstring detail;
        expect(buffer.saveTo(output, defaultVideoSettings(), &detail),
            std::string("saveTo failed for a buffer needing prepended headers: ") + narrowForTest(detail));

        const auto& samples = backtrack::g_lastMuxedInputs.videoSamples;
        expect(samples.size() == 4, "parameter-set save lost packets");
        // Both NALs in the fixture are parameter sets, so a correct extractor
        // returns the whole payload rather than a shortened tail.
        std::vector<uint8_t> expectedHeaders;
        expect(backtrack::extractParameterSets(
                   VideoCodec::H264, parameterSetPayload(), expectedHeaders),
            "the crafted parameter-set fixture was not recognized");
        expect(expectedHeaders.size() == 512u,
            "the extractor did not return the fixture's full parameter sets");
        expect(samples.front().size == 512u + expectedHeaders.size(),
            "cached parameter sets were not merged into the first sample");
        expect(samples[1].size == 512u, "parameter sets leaked into a non-first sample");

        const auto& stream = backtrack::g_lastVideoStreamBytes;
        expect(stream.size() >= expectedHeaders.size(),
            "prepended headers are missing from the stream");
        expect(std::equal(
                   expectedHeaders.begin(), expectedHeaders.end(), stream.begin(),
                   [](uint8_t a, char b) { return a == static_cast<uint8_t>(b); }),
            "the prepended header bytes do not match the cached parameter sets");
        // The cut point itself must remain the headerless payload, so the
        // merged sample is decodable rather than two header sets back to back.
        expect(stream[expectedHeaders.size()] == static_cast<char>(0x41),
            "the headerless cut payload was not written after the prepended sets");
    });

    std::filesystem::remove_all(storageRoot, error);
}

// Replay cut safety rests on these classifiers rather than on encoder
// metadata, and a false positive here silently produces an undecodable clip
// start. Expected values are hand-derived from the two NAL header layouts -
// H.264 is nal_ref_idc 3 | type (0x65 = IDR, 0x61 = plain I), HEVC is
// (type << 1) with BLA..IDR at 16..20 and CRA at 21 - not from the classifier
// under test.
void testBitstreamClassifiersMatchIdrOnly() {
    const std::vector<uint8_t> longStart{0x00, 0x00, 0x00, 0x01};
    const std::vector<uint8_t> shortStart{0x00, 0x00, 0x01};

    expect(backtrack::bitstreamContainsKeyFrame(
               VideoCodec::H264, annexBNalUnit(longStart, 0x65)),
        "an H.264 IDR slice was not accepted as a cut point");
    expect(!backtrack::bitstreamContainsKeyFrame(
               VideoCodec::H264, annexBNalUnit(longStart, 0x61)),
        "an H.264 plain I slice was accepted as a cut point");

    for (uint8_t type = 0; type < 64; ++type) {
        for (const auto& start : {shortStart, longStart}) {
            const auto nal = annexBNalUnit(start, static_cast<uint8_t>(type << 1));
            expect(backtrack::bitstreamContainsKeyFrame(VideoCodec::Hevc, nal) == (type >= 16 && type <= 20),
                "HEVC cut classification disagreed with the closed-GOP BLA/IDR range");
            expect(backtrack::bitstreamContainsCraFrame(VideoCodec::Hevc, nal) == (type == 21),
                "HEVC CRA classification accepted another NAL type");
        }
    }

    // Both ends of the HEVC BLA..IDR range, plus CRA just past it.
    expect(backtrack::bitstreamContainsKeyFrame(
               VideoCodec::Hevc, annexBNalUnit(longStart, 0x20)),
        "an HEVC BLA at the bottom of the BLA/IDR range was not accepted");
    expect(backtrack::bitstreamContainsKeyFrame(
               VideoCodec::Hevc, annexBNalUnit(longStart, 0x26)),
        "an HEVC IDR was not accepted as a cut point");
    expect(backtrack::bitstreamContainsKeyFrame(
               VideoCodec::Hevc, annexBNalUnit(longStart, 0x28)),
        "an HEVC IDR at the top of the BLA/IDR range was not accepted");
    const std::vector<uint8_t> hevcCra = annexBNalUnit(longStart, 0x2A);
    expect(!backtrack::bitstreamContainsKeyFrame(VideoCodec::Hevc, hevcCra),
        "an HEVC CRA was accepted as a cut point");
    expect(backtrack::bitstreamContainsCraFrame(VideoCodec::Hevc, hevcCra),
        "an HEVC CRA was not reported as an open-GOP picture");
    expect(!backtrack::bitstreamContainsCraFrame(
               VideoCodec::H264, annexBNalUnit(longStart, 0x65)),
        "an H.264 IDR was misreported as an open-GOP picture");

    // A 3-byte start code carries the same header and must classify identically.
    expect(backtrack::bitstreamContainsKeyFrame(
               VideoCodec::H264, annexBNalUnit(shortStart, 0x65)),
        "a 3-byte start code hid an H.264 IDR");
    expect(!backtrack::bitstreamContainsKeyFrame(
               VideoCodec::Hevc, annexBNalUnit(shortStart, 0x2A)),
        "a 3-byte start code turned an HEVC CRA into a cut point");

    // Without a start code there is no NAL boundary. Both the bare header byte
    // and the trailing zero pair must stay unread: the scanner only commits to
    // a start code once a third byte proves one.
    const std::vector<uint8_t> noStartCode{0x65, 0x88, 0x84, 0x21, 0x00, 0x00};
    expect(!backtrack::bitstreamContainsKeyFrame(VideoCodec::H264, noStartCode),
        "an H.264 IDR header with no start code was accepted as a cut point");
    expect(!backtrack::bitstreamContainsCraFrame(VideoCodec::Hevc, noStartCode),
        "a byte sequence with no start code was reported as a CRA");
}

void testParameterSetClassifiersRequireEveryCodecHeader() {
    for (const auto codec : {VideoCodec::H264, VideoCodec::Hevc}) {
        const std::vector<uint8_t> headers = codec == VideoCodec::H264
            ? std::vector<uint8_t>{0x67, 0x68} : std::vector<uint8_t>{0x40, 0x42, 0x44};
        const std::vector<uint8_t> irrelevant = codec == VideoCodec::H264
            ? std::vector<uint8_t>{0x61, 0x66, 0x69, 0x7f}
            : std::vector<uint8_t>{0x02, 0x0e, 0x10, 0x2a, 0x7e};
        for (const auto& start : {std::vector<uint8_t>{0, 0, 1}, std::vector<uint8_t>{0, 0, 0, 1}}) {
            const unsigned complete = (1u << headers.size()) - 1;
            for (unsigned subset = 0; subset <= complete; ++subset) {
                std::vector<uint8_t> bytes;
                std::vector<uint8_t> expected;
                for (const uint8_t header : irrelevant) {
                    const auto nal = annexBNalUnit(start, header);
                    bytes.insert(bytes.end(), nal.begin(), nal.end());
                }
                for (size_t i = 0; i < headers.size(); ++i) {
                    if ((subset & (1u << i)) == 0) {
                        continue;
                    }
                    auto nal = annexBNalUnit(start, headers[i]);
                    nal.insert(nal.end(), {0xab, 0xcd, 0x80});
                    // duplicate sets cannot substitute for a missing type.
                    for (int repeat = 0; repeat < 2; ++repeat) {
                        bytes.insert(bytes.end(), nal.begin(), nal.end());
                        expected.insert(expected.end(), nal.begin(), nal.end());
                        const auto filler = annexBNalUnit(
                            start.size() == 3 ? std::vector<uint8_t>{0, 0, 0, 1} : std::vector<uint8_t>{0, 0, 1},
                            irrelevant.back());
                        bytes.insert(bytes.end(), filler.begin(), filler.end());
                    }
                }
                expect(backtrack::bitstreamContainsParameterSets(codec, bytes) == (subset == complete),
                    "parameter-set completeness did not match the codec's required types");
                expect(!backtrack::bitstreamContainsKeyFrame(codec, bytes),
                    "parameter sets or irrelevant NALs became a cut point");
                expect(backtrack::bitstreamContainsCraFrame(codec, bytes) == (codec == VideoCodec::Hevc),
                    "an interleaved CRA was lost or invented");
                std::vector<uint8_t> extracted{0xff};
                expect(backtrack::extractParameterSets(codec, bytes, extracted) == (subset != 0),
                    "extraction incorrectly required a complete parameter-set group");
                expect(extracted == expected, "extraction changed parameter sets or retained irrelevant NALs");
            }
        }
    }
}

void testBitstreamClassifiersHandleTruncatedAndTrailingInput() {
    const std::vector<std::vector<uint8_t>> truncated{
        {}, {0}, {0, 0}, {0, 0, 0}, {0, 0, 1}, {0, 0, 0, 1},
        {0x65, 0x2a, 0x67, 0x68, 0x40, 0x42, 0x44, 0, 0}};
    for (const auto codec : {VideoCodec::H264, VideoCodec::Hevc}) {
        for (const auto& bytes : truncated) {
            expect(!backtrack::bitstreamContainsKeyFrame(codec, bytes) &&
                   !backtrack::bitstreamContainsCraFrame(codec, bytes) &&
                   !backtrack::bitstreamContainsParameterSets(codec, bytes),
                "truncated or unframed input was classified as a NAL");
            std::vector<uint8_t> extracted{0xff};
            expect(!backtrack::extractParameterSets(codec, bytes, extracted) && extracted.empty(),
                "empty extraction retained bytes from truncated or previous input");
        }
        for (const auto& start : {std::vector<uint8_t>{0, 0, 1}, std::vector<uint8_t>{0, 0, 0, 1}}) {
            const uint8_t idr = codec == VideoCodec::H264 ? 0x65 : 0x28;
            auto bytes = annexBNalUnit(start, idr);
            bytes.insert(bytes.end(), start.begin(), start.end());
            expect(backtrack::bitstreamContainsKeyFrame(codec, bytes),
                "a trailing empty start code hid an IDR");
            expect(!backtrack::bitstreamContainsCraFrame(codec, bytes), "an IDR became a CRA");

            const std::vector<uint8_t> headers = codec == VideoCodec::H264
                ? std::vector<uint8_t>{0x67, 0x68} : std::vector<uint8_t>{0x40, 0x42, 0x44};
            bytes.clear();
            for (const auto header : headers) {
                const auto nal = annexBNalUnit(start, header);
                bytes.insert(bytes.end(), nal.begin(), nal.end());
            }
            // header-only NALs are recognized; this scanner does not parse payloads.
            expect(backtrack::bitstreamContainsParameterSets(codec, bytes),
                "header-only parameter sets were not recognized");
            std::vector<uint8_t> extracted;
            auto trailing = bytes;
            trailing.insert(trailing.end(), start.begin(), start.end());
            expect(backtrack::bitstreamContainsParameterSets(codec, trailing) &&
                   backtrack::extractParameterSets(codec, trailing, extracted) && extracted == bytes,
                "a trailing empty start code changed complete parameter sets");
            bytes.insert(bytes.end(), {0, 0});
            expect(backtrack::extractParameterSets(codec, bytes, extracted) && extracted == bytes,
                "a trailing partial start code truncated the last parameter set");
        }
    }
}

// The extractor ends a NAL where the next start code begins, and a start code
// needs a third byte to prove itself - so a NAL running to the last byte of
// the buffer used to be copied 2 bytes short. That short copy is cached as the
// replay's codec header and prepended to every cut, so a headers-only packet
// from a driver turned into an undecodable first frame with nothing reported
// anywhere. Both NALs in the fixture are parameter sets, so a correct extractor
// returns the whole payload.
void testExtractParameterSetsCopiesTrailingNalWhole() {
    const std::vector<uint8_t> payload = parameterSetPayload();
    std::vector<uint8_t> extracted;
    expect(backtrack::extractParameterSets(VideoCodec::H264, payload, extracted),
        "the headers-only fixture was not recognized");
    expect(extracted == payload,
        "a NAL ending at the last byte of the buffer was not copied whole");

    // The scan must also resume correctly after a NAL that consumed the
    // remaining bytes: a following slice must not be swallowed into the
    // parameter sets, and must not stop the scan short of a later set.
    std::vector<uint8_t> withSlice = payload;
    const uint8_t slice[] = {0x00, 0x00, 0x00, 0x01, 0x65, 0x41, 0x41, 0x41};
    withSlice.insert(withSlice.end(), slice, slice + sizeof(slice));
    std::vector<uint8_t> mixed;
    expect(backtrack::extractParameterSets(VideoCodec::H264, withSlice, mixed),
        "the headers-plus-slice fixture was not recognized");
    expect(mixed == payload,
        "a trailing slice NAL was wrongly included in the parameter sets");
}

// A muxing failure must fail the save, keep the elementary streams for
// recovery, and hand the user a reason.
void testMuxFailurePreservesStreamsAndReportsReason() {
    std::error_code error;
    std::filesystem::remove_all(testOutputDir(), error);

    ReplayBuffer buffer;
    buffer.configure(settings(30));
    for (int64_t pts = 0; pts < 6; ++pts) {
        buffer.pushVideo(makeVideoPayload(
            pts * kSecond100ns, kSecond100ns, pts == 0, static_cast<uint8_t>(0xA0 + pts), 1024));
    }

    const auto output = testOutputDir() / L"mux_failure" / L"clip.mp4";
    backtrack::g_muxShouldFail = true;
    backtrack::g_muxFailureDetail = L"simulated muxer failure";
    std::wstring detail;
    const bool saved = buffer.saveTo(output, defaultVideoSettings(), &detail);
    backtrack::g_muxShouldFail = false;
    backtrack::g_muxFailureDetail.clear();

    expect(!saved, "saveTo reported success when muxing failed");
    expect(!detail.empty(), "a muxing failure produced no user-facing reason");
    const auto staging = output.parent_path() / L".backtrack_tmp" / output.stem();
    expect(std::filesystem::exists(staging / backtrack::RecoveryStore::kRecoveryManifestName, error),
        "a failed mux dropped the elementary streams needed for recovery");

    buffer.clear();
    std::filesystem::remove_all(testOutputDir(), error);
}

void testSameNameRetryDoesNotTouchPreservedReplay() {
    std::error_code error;
    std::filesystem::remove_all(testOutputDir(), error);
    ReplayBuffer buffer;
    buffer.configure(settings(30));
    for (int64_t pts = 0; pts < 6; ++pts) {
        buffer.pushVideo(makeVideoPayload(
            pts * kSecond100ns, kSecond100ns, pts == 0, static_cast<uint8_t>(0xA0 + pts), 1024));
    }
    const auto directory = testOutputDir() / L"same_minute";
    std::filesystem::create_directories(directory, error);
    expect(!error, "could not create retry fixture");
    const auto output = directory / L"clip.mp4";
    const HANDLE reservation = CreateFileW(
        output.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr);
    expect(reservation != INVALID_HANDLE_VALUE, "could not reserve first output name");
    CloseHandle(reservation);

    backtrack::g_muxShouldFail = true;
    std::wstring detail;
    const bool saved = buffer.saveTo(output, defaultVideoSettings(), &detail);
    backtrack::g_muxShouldFail = false;
    expect(!saved, "first replay unexpectedly saved");
    expect(!std::filesystem::exists(output), "failed replay did not release its output reservation");
    const auto firstDir = directory / L".backtrack_tmp" / output.stem();
    const auto firstVideo = firstDir / L"replay.h264";
    const auto firstManifest = firstDir / backtrack::RecoveryStore::kRecoveryManifestName;
    std::ifstream videoBefore(firstVideo, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(videoBefore)), std::istreambuf_iterator<char>());
    std::ifstream manifestBefore(firstManifest, std::ios::binary);
    const std::string manifest((std::istreambuf_iterator<char>(manifestBefore)), std::istreambuf_iterator<char>());
    videoBefore.close();
    manifestBefore.close();
    expect(!bytes.empty() && !manifest.empty(), "first replay was not preserved");

    const HANDLE retryReservation = CreateFileW(
        output.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr);
    expect(retryReservation != INVALID_HANDLE_VALUE, "could not reserve retry output name");
    CloseHandle(retryReservation);
    expect(buffer.saveTo(output, defaultVideoSettings()), "same-name retry failed");
    expect(backtrack::g_lastMuxedInputs.videoPath.parent_path() != firstDir,
        "retry reused the preserved session directory");
    std::ifstream videoAfter(firstVideo, std::ios::binary);
    const std::string afterBytes((std::istreambuf_iterator<char>(videoAfter)), std::istreambuf_iterator<char>());
    std::ifstream manifestAfter(firstManifest, std::ios::binary);
    const std::string afterManifest((std::istreambuf_iterator<char>(manifestAfter)), std::istreambuf_iterator<char>());
    videoAfter.close();
    manifestAfter.close();
    expect(afterBytes == bytes && afterManifest == manifest, "retry altered the preserved replay");
    std::wstring recoveryDetail;
    expect(!backtrack::RecoveryStore::recoverLatestFailedRecording(directory, recoveryDetail).empty(),
        "preserved replay could not be recovered after retry");
    expect(std::string(backtrack::g_lastVideoStreamBytes.begin(), backtrack::g_lastVideoStreamBytes.end()) == bytes,
        "recovery did not receive the original replay bytes");
    buffer.clear();
    std::filesystem::remove_all(testOutputDir(), error);
}

// Audio is buffered through the RAM queues when diskBackedAudio is off, so
// only pushing it through the disk backend proves the save path stitches both
// timelines.
void testDiskBackendSavesAudio() {
    const auto storageRoot = testOutputDir() / L"ring_audio";
    std::error_code error;
    std::filesystem::remove_all(storageRoot, error);
    std::filesystem::create_directories(storageRoot, error);

    ReplaySettings diskSettings = settings(30);
    diskSettings.diskBackedVideo = true;
    ReplayBuffer buffer;
    buffer.configure(diskSettings, storageRoot, 8ull * 1024 * 1024);
    for (int64_t pts = 0; pts < 6; ++pts) {
        buffer.pushVideo(makeVideoPayload(
            pts * kSecond100ns, kSecond100ns, pts == 0, static_cast<uint8_t>(0xB0 + pts), 1024));
    }
    for (int64_t pts = 1; pts < 5; ++pts) {
        buffer.pushAudio(makeAudio(pts * kSecond100ns, 1));
    }

    const auto output = testOutputDir() / L"disk_audio.mp4";
    expect(buffer.saveTo(output, defaultVideoSettings()),
        "a disk-backed save with audio failed");
    expect(backtrack::g_lastSystemWav.has_value(),
        "the disk backend produced no system audio track");
    // Anchored to the video start (0s) and gap-filled up to the last audio
    // packet (5s), so the muxed tracks stay aligned.
    expect(backtrack::g_lastSystemWav->dataBytes == 5ull * 48000 * 4,
        "the disk backend wrote the wrong amount of audio");

    buffer.clear();
    std::filesystem::remove_all(storageRoot, error);
}

// A loopback device that drops out for 20 s is written out as the full 20 s of
// silence so the saved audio keeps the real gap duration and post-outage audio
// stays aligned with the unchanged video track. Compressing the gap would play
// it early. The exact byte count - not "at least" - is what pins the preserved
// duration.
void testReplayAudioGapPreservesDuration() {
    std::error_code error;
    std::filesystem::remove_all(testOutputDir(), error);

    ReplayBuffer buffer;
    buffer.configure(settings(60));
    // 30 s of video so the audio track spans the whole window; only the video
    // start anchors the timeline, so the gap below is measured from 0.
    for (int64_t pts = 0; pts < 30; ++pts) {
        buffer.pushVideo(makeVideoPayload(
            pts * kSecond100ns, kSecond100ns, pts % 5 == 0, static_cast<uint8_t>(0xE0 + (pts % 8)), 1024));
    }
    // One system-audio packet at 20 s: a 20 s hole.
    buffer.pushAudio(makeAudio(20 * kSecond100ns, 1));

    const auto output = testOutputDir() / L"gapcap.mp4";
    expect(buffer.saveTo(output, defaultVideoSettings()),
        "saving a replay with an audio gap failed");
    expect(backtrack::g_lastSystemWav.has_value(),
        "an audio gap produced no system audio WAV");

    // The full 20 s gap as silence plus the one-second packet, and nothing
    // else: the saved audio preserves the real gap duration instead of
    // compressing it to 10 s of silence plus the packet.
    const uint64_t expectedBytes = 21ull * 48000 * 4;
    expect(backtrack::g_lastSystemWav->dataBytes == expectedBytes,
        "the audio gap duration was not preserved (got " +
            std::to_string(backtrack::g_lastSystemWav->dataBytes) +
            " bytes, expected " + std::to_string(expectedBytes) + ")");

    buffer.clear();
    std::filesystem::remove_all(testOutputDir(), error);
}

// An antivirus scanner or the search indexer routinely holds the manifest open
// for a second or two. publishWithRetry exists so that transient handle does
// not cost the user a whole recording: by this point every elementary-stream
// write has already been verified, so the only thing left to do is wait it out.
// A false return must therefore not be reported as a write failure.
void testManifestPublishSurvivesSharingViolation() {
    RecoveryFixture fixture = RecoveryFixture::create(L"sharing");
    const auto videoPath = fixture.sessionDir / L"replay.h264";
    writeElementaryStream(videoPath, kPackets);
    const MuxedInputs inputs = makeMuxedInputs(videoPath, kPackets);

    const auto manifestPath = fixture.sessionDir / backtrack::RecoveryStore::kRecoveryManifestName;
    expect(backtrack::RecoveryStore::writeManifest(inputs, fixture.sessionDir),
        "could not create the manifest fixture to lock");

    // Hold recovery.tsv with no sharing at all: MOVEFILE_REPLACE_EXISTING fails
    // with ERROR_ACCESS_DENIED for as long as this handle is open, which is the
    // error publishWithRetry has to treat as transient.
    HANDLE hold = CreateFileW(
        manifestPath.c_str(), GENERIC_READ, 0 /* no FILE_SHARE_* */, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    expect(hold != INVALID_HANDLE_VALUE, "could not open the manifest fixture exclusively");

    // Release it while writeManifest is inside its backoff, the way a scanner
    // finishing its scan looks. The first retry backoff is 300 ms, so this lands
    // inside the window.
    const auto releaseAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    std::thread releaser([hold, releaseAt] {
        std::this_thread::sleep_until(releaseAt);
        CloseHandle(hold);
    });

    const bool published = backtrack::RecoveryStore::writeManifest(inputs, fixture.sessionDir);
    releaser.join();  // the releasing thread owns the close; nothing closes it twice

    expect(published, "the manifest publish gave up on a transient sharing violation");

    const auto stagingPath = fixture.sessionDir /
        (std::wstring(backtrack::RecoveryStore::kRecoveryManifestName) + L".tmp");
    expect(!std::filesystem::exists(stagingPath),
        "a retried manifest publish left its staging file behind");

    // The republished table must describe the same session, not be truncated by
    // the failed attempt: the retry re-writes the staging file from scratch.
    expectRecoveredSamples(fixture, kPackets, "publish after a sharing violation");
    fixture.dispose();
}

// a directory without timing data remains available for manual recovery.
void testDirectoryWithoutManifestIsNotRecovered() {
    RecoveryFixture fixture = RecoveryFixture::create(L"nomanifest");
    writeElementaryStream(fixture.sessionDir / L"replay.h264", kPackets);

    std::wstring detail;
    // The stub records what the muxer was handed and is process-global, so an
    // earlier case in this suite has already populated it. Clearing it is what
    // makes "the muxer was never reached" an assertion rather than a leftover.
    backtrack::g_lastMuxedInputs.videoSamples.clear();
    const auto recovered =
        backtrack::RecoveryStore::recoverLatestFailedRecording(fixture.clipsDir, detail);
    expect(recovered.empty(),
        "a recovery directory holding streams but no manifest was recovered anyway");
    expect(!detail.empty(), "an unrecoverable directory reported no reason");
    // Nothing was muxed, so the muxer must not have been reached at all.
    expect(backtrack::g_lastMuxedInputs.videoSamples.empty(),
        "the muxer was handed a sample table for a directory with no manifest");
    // The directory itself must survive: recovery failing is not a reason to
    // delete the user's data.
    expect(std::filesystem::exists(fixture.sessionDir / L"replay.h264"),
        "an unrecoverable recovery directory had its streams deleted");

    // A truncated manifest is just as unusable as a missing one.
    writeTextFile(fixture.sessionDir / backtrack::RecoveryStore::kRecoveryManifestName,
        "backtrack-recovery-v1\ncodec\th264\n");
    const auto recoveredTruncated =
        backtrack::RecoveryStore::recoverLatestFailedRecording(fixture.clipsDir, detail);
    expect(recoveredTruncated.empty(),
        "a header-only manifest was recovered as if it described a session");

    fixture.dispose();
}

// Sound has its own ring file (replay_audio.bin), independent of the video
// backend. These cases run video in RAM so every byte the save publishes for
// audio provably came back out of the audio ring.
void testDiskAudioRoundTrip() {
    const auto storageRoot = testOutputDir() / L"ring_audio_roundtrip";
    std::error_code error;
    std::filesystem::remove_all(storageRoot, error);
    std::filesystem::create_directories(storageRoot, error);

    ReplaySettings audioSettings = settings(30);
    audioSettings.diskBackedAudio = true;
    ReplayBuffer buffer;
    buffer.configure(audioSettings, storageRoot, 0, 1024ull * 1024);
    for (int64_t pts = 0; pts < 6; ++pts) {
        buffer.pushVideo(makeVideo(pts * kSecond100ns, kSecond100ns, pts == 0));
    }
    for (int64_t pts = 1; pts < 5; ++pts) {
        buffer.pushAudio(makeAudio(pts * kSecond100ns, 1));
    }
    AudioPacket mic = makeAudio(2 * kSecond100ns, 1);
    mic.track = AudioTrack::Microphone;
    buffer.pushAudio(std::move(mic));
    expect(std::filesystem::exists(storageRoot / L".backtrack_ring" / L"replay_audio.bin", error),
        "audio ring file was not created");
    expect(!std::filesystem::exists(storageRoot / L".backtrack_ring" / L"replay_video.bin", error),
        "audio-only disk backing created a video ring file");

    const auto output = testOutputDir() / L"disk_audio_roundtrip.mp4";
    expect(buffer.saveTo(output, defaultVideoSettings()),
        "a disk-audio save failed");
    expect(backtrack::g_lastSystemWav.has_value(),
        "the audio ring produced no system audio track");
    // Anchored to the video start (0s) and gap-filled up to the last audio
    // packet (5s): byte-identical to what the RAM backend publishes.
    expect(backtrack::g_lastSystemWav->dataBytes == 5ull * 48000 * 4,
        "the audio ring read back the wrong bytes");

    buffer.clear();
    std::filesystem::remove_all(storageRoot, error);
}

// More audio than the ring holds forces a wrap and oldest-first eviction;
// the surviving extents must still reassemble byte for byte.
void testDiskAudioRoundTripAcrossWrap() {
    const auto storageRoot = testOutputDir() / L"ring_audio_wrap";
    std::error_code error;
    std::filesystem::remove_all(storageRoot, error);
    std::filesystem::create_directories(storageRoot, error);

    ReplaySettings audioSettings = settings(30);
    audioSettings.diskBackedAudio = true;
    ReplayBuffer buffer;
    buffer.configure(audioSettings, storageRoot, 0, 1024ull * 1024);
    for (int64_t pts = 0; pts <= 6; ++pts) {
        buffer.pushVideo(makeVideo(pts * kSecond100ns, kSecond100ns, pts == 0));
    }
    // 8 one-second packets (~1.5 MiB) against a 1 MiB ring: only the newest 5
    // ([3,8)) survive. The WAV still spans the video window [0,7].
    for (int64_t pts = 0; pts < 8; ++pts) {
        buffer.pushAudio(makeAudio(pts * kSecond100ns, 1));
    }

    const auto output = testOutputDir() / L"disk_audio_wrap.mp4";
    expect(buffer.saveTo(output, defaultVideoSettings()),
        "a disk-audio save across a ring wrap failed");
    expect(backtrack::g_lastSystemWav.has_value(),
        "the wrapped audio ring produced no system audio track");
    expect(backtrack::g_lastSystemWav->dataBytes == 7ull * 48000 * 4,
        "the wrapped audio ring produced " + std::to_string(backtrack::g_lastSystemWav->dataBytes) +
            " audio bytes, expected a 7 s timeline");

    buffer.clear();
    std::filesystem::remove_all(storageRoot, error);
}

AudioPacket makeMarkedAudio(AudioTrack track, int64_t seconds, uint8_t marker) {
    AudioPacket packet = makeAudio(seconds * kSecond100ns, 1);
    packet.track = track;
    for (size_t i = 0; i < packet.bytes.size(); ++i) {
        packet.bytes[i] = static_cast<uint8_t>(i * 31 + i / 251 + marker * 17);
    }
    return packet;
}

std::vector<uint8_t> audioSeconds(std::initializer_list<const AudioPacket*> seconds) {
    std::vector<uint8_t> result;
    for (const AudioPacket* packet : seconds) {
        if (packet) {
            result.insert(result.end(), packet->bytes.begin(), packet->bytes.end());
        } else {
            result.insert(result.end(), 48000 * 4, 0);
        }
    }
    return result;
}

void expectAudioTracks(std::initializer_list<const AudioPacket*> system,
                       std::initializer_list<const AudioPacket*> microphone) {
    expect(backtrack::g_lastSystemWav.has_value() == (system.size() != 0),
        "system audio track presence changed");
    expect(backtrack::g_lastMicrophoneWav.has_value() == (microphone.size() != 0),
        "microphone audio track presence changed");
    expect(backtrack::g_lastSystemAudioBytes == audioSeconds(system),
        "saved system audio bytes differ");
    expect(backtrack::g_lastMicrophoneAudioBytes == audioSeconds(microphone),
        "saved microphone audio bytes differ");
}

void testDiskAudioInsertionOrderAcrossTrackDrains() {
    backtrack::g_captureAudioBytes = true;
    for (const bool diskVideo : {false, true}) {
        const auto storageRoot = testOutputDir() / (diskVideo ? L"audio_fifo_disk_video" : L"audio_fifo_ram_video");
        std::error_code error;
        std::filesystem::remove_all(storageRoot, error);
        {
            ReplaySettings replaySettings = settings(30);
            replaySettings.diskBackedAudio = true;
            replaySettings.diskBackedVideo = diskVideo;
            ReplayBuffer buffer;
            buffer.configure(replaySettings, storageRoot, 8ull * 1024 * 1024, 1024ull * 1024);
            for (int64_t pts = 0; pts < 12; ++pts) {
                buffer.pushVideo(makeVideo(pts * kSecond100ns, kSecond100ns, pts == 0));
            }
            std::vector<AudioPacket> packets;
            for (int64_t pts = 4; pts <= 6; ++pts) {
                packets.push_back(makeMarkedAudio(AudioTrack::System, pts, static_cast<uint8_t>(packets.size() + 1)));
            }
            for (int64_t pts = 1; pts <= 3; ++pts) {
                packets.push_back(makeMarkedAudio(AudioTrack::Microphone, pts, static_cast<uint8_t>(packets.size() + 1)));
            }
            for (int64_t pts = 7; pts <= 9; ++pts) {
                packets.push_back(makeMarkedAudio(AudioTrack::System, pts, static_cast<uint8_t>(packets.size() + 1)));
            }
            for (int64_t pts = 4; pts <= 6; ++pts) {
                packets.push_back(makeMarkedAudio(AudioTrack::Microphone, pts, static_cast<uint8_t>(packets.size() + 1)));
            }
            const auto output = storageRoot / L"drains.mp4";
            for (size_t i = 0; i < 3; ++i) {
                buffer.pushAudio(packets[i]);
            }
            expect(buffer.saveTo(output, defaultVideoSettings()), "save between track drains failed");
            expectAudioTracks({nullptr, nullptr, nullptr, nullptr, &packets[0], &packets[1], &packets[2]}, {});

            for (size_t i = 3; i < 6; ++i) {
                buffer.pushAudio(packets[i]);
            }
            expect(buffer.saveTo(output, defaultVideoSettings()), "save after reversed timestamps failed");
            expectAudioTracks({nullptr, nullptr, nullptr, nullptr, nullptr, &packets[1], &packets[2]},
                              {nullptr, &packets[3], &packets[4], &packets[5]});

            for (size_t i = 6; i < 9; ++i) {
                buffer.pushAudio(packets[i]);
            }
            expect(buffer.saveTo(output, defaultVideoSettings()), "second save between track drains failed");
            expectAudioTracks({nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                               &packets[6], &packets[7], &packets[8]},
                              {nullptr, nullptr, &packets[4], &packets[5]});

            for (size_t i = 9; i < 12; ++i) {
                buffer.pushAudio(packets[i]);
            }
            expect(buffer.saveTo(output, defaultVideoSettings()), "save after second wrap failed");
            expectAudioTracks({nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                               &packets[7], &packets[8]},
                              {nullptr, nullptr, nullptr, nullptr, &packets[9], &packets[10], &packets[11]});
        }
        std::filesystem::remove_all(storageRoot, error);
    }
    backtrack::g_captureAudioBytes = false;
}

void testDiskAudioAgeTrimKeepsPhysicalPrefix() {
    backtrack::g_captureAudioBytes = true;
    constexpr uint64_t kPacketBytes = 48000 * 4;
    for (const bool diskVideo : {false, true}) {
        const auto storageRoot = testOutputDir() / (diskVideo ? L"audio_age_disk_video" : L"audio_age_ram_video");
        std::error_code error;
        std::filesystem::remove_all(storageRoot, error);
        {
            ReplaySettings replaySettings = settings(3);
            replaySettings.diskBackedAudio = true;
            replaySettings.diskBackedVideo = diskVideo;
            ReplayBuffer buffer;
            buffer.configure(replaySettings, storageRoot, 8ull * 1024 * 1024, 1024ull * 1024);
            buffer.pushVideo(makeVideo(0, kSecond100ns, true));
            const auto a = makeMarkedAudio(AudioTrack::System, 2, 1);
            const auto b = makeMarkedAudio(AudioTrack::Microphone, 0, 2);
            const auto c = makeMarkedAudio(AudioTrack::System, 3, 3);
            const auto d = makeMarkedAudio(AudioTrack::Microphone, 1, 4);
            const auto e = makeMarkedAudio(AudioTrack::System, 4, 5);
            const auto f = makeMarkedAudio(AudioTrack::Microphone, 2, 6);
            for (const auto* packet : {&a, &b, &c, &d, &e}) {
                buffer.pushAudio(*packet);
            }
            for (int64_t pts = 1; pts <= 4; ++pts) {
                buffer.pushVideo(makeVideo(pts * kSecond100ns, kSecond100ns, pts != 2));
            }
            expect(backtrack::ReplayBufferTestAccess::diskAudioBytes(buffer) == 5 * kPacketBytes,
                "age trimming reclaimed bytes behind an unexpired packet");
            const auto output = storageRoot / L"age.mp4";
            expect(buffer.saveTo(output, defaultVideoSettings()), "save after age trimming failed");
            expectAudioTracks({nullptr, &a, &c, &e}, {&d});

            buffer.pushAudio(f);
            expect(backtrack::ReplayBufferTestAccess::diskAudioBytes(buffer) == 4 * kPacketBytes,
                "capacity eviction or subsequent age trimming removed the wrong prefix");
            expect(buffer.saveTo(output, defaultVideoSettings()), "save after age trim and wrap failed");
            expectAudioTracks({nullptr, nullptr, &c, &e}, {&d, &f});

            for (int64_t pts = 5; pts <= 7; ++pts) {
                buffer.pushVideo(makeVideo(pts * kSecond100ns, kSecond100ns, true));
            }
            expect(backtrack::ReplayBufferTestAccess::diskAudioBytes(buffer) == 2 * kPacketBytes,
                "age trimming did not stop at the first unexpired packet");
            expect(buffer.saveTo(output, defaultVideoSettings()), "save after prefix expired failed");
            expectAudioTracks({&e}, {});
            buffer.pushVideo(makeVideo(8 * kSecond100ns, kSecond100ns, true));
            expect(backtrack::ReplayBufferTestAccess::diskAudioBytes(buffer) == 0,
                "age trimming did not reclaim the remaining expired prefix");
        }
        std::filesystem::remove_all(storageRoot, error);
    }
    backtrack::g_captureAudioBytes = false;
}

// A capacity-only change must not drop buffered audio, mirroring the video
// ring: the recorded extents stay valid at their offsets.
void testDiskAudioSurvivesCapacityChange() {
    const auto storageRoot = testOutputDir() / L"ring_audio_capacity";
    std::error_code error;
    std::filesystem::remove_all(storageRoot, error);
    std::filesystem::create_directories(storageRoot, error);

    ReplaySettings audioSettings = settings(30);
    audioSettings.diskBackedAudio = true;
    ReplayBuffer buffer;
    buffer.configure(audioSettings, storageRoot, 0, 1024ull * 1024);
    for (int64_t pts = 0; pts < 3; ++pts) {
        buffer.pushVideo(makeVideo(pts * kSecond100ns, kSecond100ns, pts == 0));
    }
    for (int64_t pts = 1; pts < 3; ++pts) {
        buffer.pushAudio(makeAudio(pts * kSecond100ns, 1));
    }

    const auto ringPath = storageRoot / L".backtrack_ring" / L"replay_audio.bin";
    buffer.configure(audioSettings, storageRoot, 0, 2ull * 1024 * 1024);
    expect(buffer.storageFallbackReason().empty(),
        "an audio capacity-only change reported a storage fallback");
    expect(std::filesystem::file_size(ringPath, error) == 1024ull * 1024,
        "an audio capacity change re-laid out a ring that still held packets");

    expect(buffer.videoPacketCount() == 3,
        "an audio capacity change dropped RAM video");
    const auto output = testOutputDir() / L"disk_audio_capacity.mp4";
    expect(buffer.saveTo(output, defaultVideoSettings()),
        "saving after an audio capacity change failed");
    expect(backtrack::g_lastSystemWav.has_value(),
        "saving after an audio capacity change produced no audio");
    expect(backtrack::g_lastSystemWav->dataBytes == 3ull * 48000 * 4,
        "saving after an audio capacity change lost packets");

    buffer.clear();
    std::filesystem::remove_all(storageRoot, error);
}

// An audio ring read failure fails the save and degrades audio to RAM only:
// video buffering on its own backend is untouched, and the session saves
// again afterwards.
void testAudioRingReadFailureFailsOverAudioOnly() {
    const auto storageRoot = testOutputDir() / L"ring_audio_read_failure";
    std::error_code error;
    std::filesystem::remove_all(storageRoot, error);
    std::filesystem::create_directories(storageRoot, error);

    ReplaySettings audioSettings = settings(30);
    audioSettings.diskBackedAudio = true;
    ReplayBuffer buffer;
    buffer.configure(audioSettings, storageRoot, 0, 1024ull * 1024);
    for (int64_t pts = 0; pts < 6; ++pts) {
        buffer.pushVideo(makeVideo(pts * kSecond100ns, kSecond100ns, pts == 0));
    }
    for (int64_t pts = 1; pts < 5; ++pts) {
        buffer.pushAudio(makeAudio(pts * kSecond100ns, 1));
    }

    backtrack::g_lastMuxedInputs.videoSamples.clear();
    backtrack::ReplayBufferTestAccess::breakAudioRing(buffer);

    const auto output = testOutputDir() / L"ring_audio_read_failure.mp4";
    std::wstring detail;
    const bool saved = buffer.saveTo(output, defaultVideoSettings(), &detail);
    expect(!saved, "a save reported success after the audio ring stopped reading");
    expect(backtrack::g_lastMuxedInputs.videoSamples.empty(),
        "a save with unreadable audio reached the muxer");
    expect(detail.find(L"disk ring") != std::wstring::npos,
        "the audio failure did not name the ring: " + narrowForTest(detail));
    expect(!buffer.storageFallbackReason().empty(),
        "an audio ring read failure was not reported to the user");

    for (int64_t pts = 6; pts < 9; ++pts) {
        buffer.pushVideo(makeVideo(pts * kSecond100ns, kSecond100ns, false));
        buffer.pushAudio(makeAudio(pts * kSecond100ns, 1));
    }
    expect(buffer.saveTo(output, defaultVideoSettings()),
        "the session could not save again after an audio ring read failure");
    expect(backtrack::g_lastSystemWav.has_value(),
        "the recovery save produced no audio after audio failover");

    buffer.clear();
    std::filesystem::remove_all(storageRoot, error);
}

void testBackendAndDirectorySwitchesResetBuffer() {
    const auto firstRoot = testOutputDir() / L"switch_ring_a";
    const auto secondRoot = testOutputDir() / L"switch_ring_b";
    std::error_code error;
    std::filesystem::remove_all(firstRoot, error);
    std::filesystem::remove_all(secondRoot, error);
    std::filesystem::create_directories(firstRoot, error);
    std::filesystem::create_directories(secondRoot, error);

    ReplaySettings diskSettings = settings(30);
    diskSettings.diskBackedVideo = true;
    ReplayBuffer buffer;

    // RAM -> disk: no ring was open, so the buffer starts empty and works.
    buffer.configure(diskSettings, firstRoot, 8ull * 1024 * 1024);
    expect(buffer.videoPacketCount() == 0, "switching to disk started with buffered video");
    buffer.pushVideo(makeVideoPayload(0, kSecond100ns, true, 0xC0, 1024));
    expect(buffer.videoPacketCount() == 1, "the disk backend did not buffer after a RAM switch");
    expect(std::filesystem::exists(firstRoot / L".backtrack_ring" / L"replay_video.bin", error),
        "the ring file was not created in the requested folder");

    // A directory switch does drop the buffer: the old extents name the old ring.
    buffer.configure(diskSettings, secondRoot, 8ull * 1024 * 1024);
    expect(buffer.videoPacketCount() == 0, "a ring-directory switch kept unreadable video");
    expect(std::filesystem::exists(secondRoot / L".backtrack_ring" / L"replay_video.bin", error),
        "the ring file was not recreated in the new folder");
    expect(!std::filesystem::exists(firstRoot / L".backtrack_ring" / L"replay_video.bin", error),
        "the ring file in the old folder survived the switch");
    expect(buffer.storageFallbackReason().empty(), "a directory switch reported a fallback");

    // Disk -> RAM drops it too. The ring file is deleted by its own handle on
    // close; the folder stays, because it carries no ownership marker and can
    // hold a legacy ring or a game's files.
    buffer.pushVideo(makeVideoPayload(0, kSecond100ns, true, 0xC1, 1024));
    buffer.configure(settings(30), secondRoot, 8ull * 1024 * 1024);
    expect(buffer.videoPacketCount() == 0, "switching to RAM kept disk-resident video");
    expect(!std::filesystem::exists(secondRoot / L".backtrack_ring" / L"replay_video.bin", error),
        "switching to RAM left the ring file behind");

    std::filesystem::remove_all(firstRoot, error);
    std::filesystem::remove_all(secondRoot, error);
}

// A ring read that fails mid-save used to break out of the dump loop with the
// elementary stream still healthy, so the sole `if (!videoStream)` guard never
// fired and a short sample table was muxed into a structurally valid clip that
// reported success. It must fail instead, and must not leave the session unable
// to save anything afterwards.
void testRingReadFailureFailsSaveAndFailsOver() {
    const auto storageRoot = testOutputDir() / L"ring_read_failure";
    std::error_code error;
    std::filesystem::remove_all(storageRoot, error);
    std::filesystem::create_directories(storageRoot, error);

    ReplaySettings diskSettings = settings(30);
    diskSettings.diskBackedVideo = true;
    ReplayBuffer buffer;
    buffer.configure(diskSettings, storageRoot, 8ull * 1024 * 1024);
    for (int64_t pts = 0; pts < 6; ++pts) {
        buffer.pushVideo(makeVideoPayload(
            pts * kSecond100ns, kSecond100ns, pts == 0, static_cast<uint8_t>(0xD0 + pts), 1024));
    }
    expect(buffer.videoPacketCount() == 6, "the disk ring did not fill before the fault");

    // The stub records what the muxer was handed; clearing it makes "was the
    // muxer reached at all" a real assertion rather than a leftover value.
    backtrack::g_lastMuxedInputs.videoSamples.clear();
    backtrack::ReplayBufferTestAccess::breakRing(buffer);

    const auto output = testOutputDir() / L"ring_read_failure.mp4";
    std::wstring detail;
    const bool saved = buffer.saveTo(output, defaultVideoSettings(), &detail);

    expect(!saved, "a save reported success after the ring stopped reading");
    expect(backtrack::g_lastMuxedInputs.videoSamples.empty(),
        "a truncated sample table reached the muxer");
    expect(!std::filesystem::exists(output, error),
        "a clip was published from a ring that could not be read");
    expect(detail.find(L"disk ring") != std::wstring::npos,
        "the failure did not name the ring: " + narrowForTest(detail));

    // The bytes behind those extents are gone, so the ring is abandoned rather
    // than retried, and the user is told the buffer moved to memory.
    expect(!buffer.storageFallbackReason().empty(),
        "a ring read failure was not reported to the user");
    expect(buffer.videoPacketCount() == 0, "the dead ring kept its unreadable packets");

    // The rest of the session must still work: in memory, with no pin or ring
    // left in a state that blocks the next save.
    for (int64_t pts = 6; pts < 12; ++pts) {
        buffer.pushVideo(makeVideoPayload(
            pts * kSecond100ns, kSecond100ns, pts == 6, static_cast<uint8_t>(0xD0 + pts), 1024));
    }
    expect(buffer.saveTo(output, defaultVideoSettings()),
        "the session could not save again after a ring read failure");
    expect(backtrack::g_lastMuxedInputs.videoSamples.size() == 6,
        "the recovery save produced the wrong sample table");

    buffer.clear();
    std::filesystem::remove_all(storageRoot, error);
}

// A killed session can strand a pre-sized ring file. Neither the file nor its
// folder carries an ownership marker, so startup maintenance preserves both: a
// legacy ring, or a game's own files, can live under the same name. Only a ring
// file this process created is reclaimed, by its own delete-on-close handle.
void testOrphanedRingFolderIsPreserved() {
    const auto storageRoot = testOutputDir() / L"orphan_ring";
    std::error_code error;
    std::filesystem::remove_all(storageRoot, error);
    std::filesystem::create_directories(storageRoot / L".backtrack_ring", error);

    const auto orphan = storageRoot / L".backtrack_ring" / L"replay_video.bin";
    {
        std::ofstream file(orphan, std::ios::binary | std::ios::trunc);
        expect(file.is_open(), "could not create the orphaned ring file");
        file << "legacy ring bytes";
    }
    ReplayBuffer::removeOrphanedRing(storageRoot);
    expect(std::filesystem::exists(orphan, error),
        "startup maintenance deleted a ring file it has no ownership evidence for");
    {
        std::ifstream file(orphan, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        expect(bytes == "legacy ring bytes", "startup maintenance changed a ring file it does not own");
    }

    // Idempotent, and the empty directory is a no-op too.
    ReplayBuffer::removeOrphanedRing(storageRoot);
    ReplayBuffer::removeOrphanedRing({});
    std::filesystem::remove_all(storageRoot, error);
}

void testDiskBackedRingRoundTripAndWrap() {
    const auto storageRoot = testOutputDir() / L"disk_ring_storage";
    std::error_code error;
    std::filesystem::remove_all(storageRoot, error);
    std::filesystem::create_directories(storageRoot, error);

    const auto ringPath = storageRoot / L".backtrack_ring" / L"replay_video.bin";
    VideoSettings videoSettings;
    videoSettings.codec = VideoCodec::H264;
    videoSettings.fps = 60;
    videoSettings.width = 1920;
    videoSettings.height = 1080;

    {
        // 3-second window (3 MiB of packets) fits the 8 MiB ring, so the
        // final buffer is shaped purely by the time window; earlier pushes
        // already exercised multiple physical wraps.
        ReplaySettings diskSettings = settings(3);
        diskSettings.diskBackedVideo = true;
        ReplayBuffer buffer;
        buffer.configure(diskSettings, storageRoot, 8ull * 1024 * 1024);

        constexpr int64_t kTotalPackets = 40; // 40 MiB written through an 8 MiB ring
        for (int64_t pts = 0; pts < kTotalPackets; ++pts) {
            buffer.pushVideo(makeVideoPayload(
                pts * kSecond100ns, kSecond100ns, (pts % 2) == 0, static_cast<uint8_t>(0x40 + pts)));
        }
        expect(std::filesystem::exists(ringPath, error), "ring file was not created");

        expect(buffer.videoPacketCount() == 4, "disk ring did not keep the 3-second window");
        expect(buffer.videoKeyFrameCount() == 2, "disk ring lost keyframe accounting across wraps");
        expect(buffer.videoSpan100ns() == 4 * kSecond100ns, "disk ring span wrong after wraps");
        expect(buffer.extendLastVideoDuration((kTotalPackets - 1) * kSecond100ns, 2 * kSecond100ns),
            "disk ring rejected a live tail extension");

        const auto output = testOutputDir() / L"disk_ring.mp4";
        expect(buffer.saveTo(output, videoSettings), "saveTo failed for the disk-backed ring");

        const auto& samples = backtrack::g_lastMuxedInputs.videoSamples;
        expect(samples.size() == 4, "disk ring clip lost packets");
        expect(samples.front().keyFrame && samples.front().pts100ns == 36 * kSecond100ns,
            "disk ring clip starts at the wrong keyframe");
        expect(samples.back().duration100ns == 2 * kSecond100ns,
            "disk ring clip lost the live tail extension");

        // Payload integrity: every sample's bytes must match the fill byte of
        // the packet pushed with its PTS. The stub snapshotted the elementary
        // stream before saveTo cleaned its staging directory.
        const auto& streamBytes = backtrack::g_lastVideoStreamBytes;
        expect(!streamBytes.empty(), "elementary stream snapshot is empty");
        for (size_t index = 0; index < samples.size(); ++index) {
            const auto& sample = samples[index];
            expect(static_cast<size_t>(sample.offset) + sample.size <= streamBytes.size(),
                "disk ring dump truncated a sample");
            const auto expectedFill = static_cast<uint8_t>(0x40 + 36 + static_cast<int64_t>(index));
            expect(std::all_of(
                       streamBytes.begin() + static_cast<std::ptrdiff_t>(sample.offset),
                       streamBytes.begin() + static_cast<std::ptrdiff_t>(sample.offset + sample.size),
                       [expectedFill](char c) { return static_cast<uint8_t>(c) == expectedFill; }),
                "disk ring returned corrupted payload (wrap mismatch)");
        }

        std::filesystem::remove_all(output.parent_path(), error);
    }

    // Lifecycle: destroying the buffer removes the ring file.
    expect(!std::filesystem::exists(ringPath, error), "ring file survived buffer destruction");
    std::filesystem::remove_all(storageRoot, error);
}

void testDiskBackedMatchesRamBackend() {
    // Core acceptance: identical pushes through both backends must produce a
    // byte-identical elementary stream and an identical sample table.
    std::vector<EncodedPacket> packets;
    constexpr int64_t kCount = 12;
    for (int64_t pts = 0; pts < kCount; ++pts) {
        packets.push_back(makeVideoPayload(
            pts * kSecond100ns, kSecond100ns, (pts % 2) == 0,
            static_cast<uint8_t>(0x20 + pts), 4096));
    }

    const auto push = [&packets](ReplayBuffer& buffer) {
        for (const auto& packet : packets) {
            buffer.pushVideo(packet);
        }
        buffer.extendLastVideoDuration((kCount - 1) * kSecond100ns, 2 * kSecond100ns);
    };
    VideoSettings videoSettings;
    videoSettings.codec = VideoCodec::H264;
    videoSettings.fps = 60;
    videoSettings.width = 1920;
    videoSettings.height = 1080;

    ReplayBuffer ramBuffer;
    ramBuffer.configure(settings(10));
    push(ramBuffer);
    const auto ramOutput = testOutputDir() / L"parity_ram.mp4";
    expect(ramBuffer.saveTo(ramOutput, videoSettings), "RAM parity save failed");
    const auto ramSamples = backtrack::g_lastMuxedInputs.videoSamples;
    const auto ramBytes = backtrack::g_lastVideoStreamBytes;

    const auto storageRoot = testOutputDir() / L"disk_ring_parity";
    std::error_code error;
    std::filesystem::remove_all(storageRoot, error);
    std::filesystem::create_directories(storageRoot, error);
    ReplaySettings diskSettings = settings(10);
    diskSettings.diskBackedVideo = true;
    ReplayBuffer diskBuffer;
    diskBuffer.configure(diskSettings, storageRoot, 8ull * 1024 * 1024);
    push(diskBuffer);
    const auto diskOutput = testOutputDir() / L"parity_disk.mp4";
    expect(diskBuffer.saveTo(diskOutput, videoSettings), "disk parity save failed");
    const auto& diskSamples = backtrack::g_lastMuxedInputs.videoSamples;
    const auto& diskBytes = backtrack::g_lastVideoStreamBytes;

    expect(ramBytes == diskBytes, "elementary streams differ between backends");
    expect(ramSamples.size() == diskSamples.size(), "sample tables differ in length");
    for (size_t index = 0; index < ramSamples.size(); ++index) {
        const auto& ram = ramSamples[index];
        const auto& disk = diskSamples[index];
        if (!(ram.offset == disk.offset && ram.size == disk.size &&
              ram.pts100ns == disk.pts100ns && ram.dts100ns == disk.dts100ns &&
              ram.hasDts == disk.hasDts && ram.duration100ns == disk.duration100ns &&
              ram.keyFrame == disk.keyFrame)) {
            std::cerr << "parity mismatch at sample " << index
                      << ": offset(" << ram.offset << "/" << disk.offset << ")"
                      << " size(" << ram.size << "/" << disk.size << ")"
                      << " pts(" << ram.pts100ns << "/" << disk.pts100ns << ")"
                      << " dur(" << ram.duration100ns << "/" << disk.duration100ns << ")"
                      << " kf(" << ram.keyFrame << "/" << disk.keyFrame << ")\n";
        }
        expect(std::memcmp(&ram, &disk, sizeof(ram)) == 0 || (
                   ram.offset == disk.offset && ram.size == disk.size &&
                   ram.pts100ns == disk.pts100ns && ram.dts100ns == disk.dts100ns &&
                   ram.hasDts == disk.hasDts && ram.duration100ns == disk.duration100ns &&
                   ram.keyFrame == disk.keyFrame),
            "sample table entry differs between backends");
    }

    std::filesystem::remove_all(ramOutput.parent_path(), error);
    std::filesystem::remove_all(storageRoot, error);
}

void testNewestVideoPtsRam() {
    ReplayBuffer buffer;
    buffer.configure(settings(10));
    expect(buffer.newestVideoPts100ns() == 0, "empty RAM buffer reported a newest PTS");
    for (int64_t pts = 0; pts < 12; ++pts) {
        buffer.pushVideo(makeVideo(pts * kSecond100ns, kSecond100ns, (pts % 2) == 0));
    }
    expect(buffer.newestVideoPts100ns() == 11 * kSecond100ns,
        "newest PTS does not match the newest buffered RAM packet");
    // Live-tail extension mutates duration, not PTS.
    buffer.extendLastVideoDuration(11 * kSecond100ns, 3 * kSecond100ns);
    expect(buffer.newestVideoPts100ns() == 11 * kSecond100ns,
        "newest PTS changed on a live-tail extension");
    buffer.clear();
    expect(buffer.newestVideoPts100ns() == 0, "clear did not reset newest PTS");
}

void testNewestVideoPtsDiskAndBackendSwitch() {
    const auto storageRoot = testOutputDir() / L"newest_pts_disk";
    std::error_code error;
    std::filesystem::remove_all(storageRoot, error);
    std::filesystem::create_directories(storageRoot, error);

    ReplaySettings diskSettings = settings(10);
    diskSettings.diskBackedVideo = true;
    ReplayBuffer buffer;
    buffer.configure(diskSettings, storageRoot, 8ull * 1024 * 1024);
    expect(buffer.newestVideoPts100ns() == 0, "empty disk buffer reported a newest PTS");
    for (int64_t pts = 0; pts < 6; ++pts) {
        buffer.pushVideo(makeVideoPayload(pts * kSecond100ns, kSecond100ns, pts == 0, 0x60, 1024));
    }
    expect(buffer.newestVideoPts100ns() == 5 * kSecond100ns,
        "disk buffer newest PTS does not match the newest packet");

    // A backend switch to RAM clears the on-disk buffered video (its bytes are
    // unreadable in the new backend), so the newest-PTS getter reports empty.
    buffer.configure(settings(10), storageRoot, 8ull * 1024 * 1024);
    expect(buffer.newestVideoPts100ns() == 0,
        "backend switch did not clear the buffered video PTS");
    std::filesystem::remove_all(storageRoot, error);
}

void testRamVideoSurvivesReconfiguration() {
    struct Scenario {
        const wchar_t* name;
        uint32_t seconds;
        uint64_t videoCap;
        bool toggleAudio;
        int64_t firstPts;
    };
    constexpr uint64_t kPacketBytes = 1024;
    constexpr uint64_t kInitialCap = 32 * kPacketBytes;
    const Scenario scenarios[] = {
        {L"unchanged", 20, kInitialCap, false, 0},
        {L"longer", 40, kInitialCap, false, 0},
        {L"shorter", 4, kInitialCap, false, 4},
        {L"byte_cap", 20, 3 * kPacketBytes, false, 6},
        {L"audio_only", 20, kInitialCap, true, 0},
    };
    for (const auto& scenario : scenarios) {
        const auto root = testOutputDir() / L"ram_reconfigure" / scenario.name;
        std::error_code error;
        std::filesystem::remove_all(root, error);
        {
            ReplayBuffer buffer;
            ReplaySettings replaySettings = settings(20);
            buffer.configure(replaySettings, root, kInitialCap, 1024ull * 1024);
            for (int64_t pts = 0; pts < 8; ++pts) {
                buffer.pushVideo(makeVideoPayload(
                    pts * kSecond100ns, kSecond100ns, (pts % 2) == 0,
                    static_cast<uint8_t>(0x30 + pts), kPacketBytes));
            }
            replaySettings.seconds = scenario.seconds;
            buffer.configure(replaySettings, root, scenario.videoCap, 1024ull * 1024);
            if (scenario.toggleAudio) {
                replaySettings.diskBackedAudio = true;
                buffer.configure(replaySettings, root, scenario.videoCap, 1024ull * 1024);
                expect(std::filesystem::exists(root / L".backtrack_ring" / L"replay_audio.bin"),
                    "audio-only switch did not open the audio ring");
                replaySettings.diskBackedAudio = false;
                buffer.configure(replaySettings, root, scenario.videoCap, 1024ull * 1024);
            }
            const size_t expectedCount = static_cast<size_t>(8 - scenario.firstPts);
            expect(buffer.videoPacketCount() == expectedCount,
                "RAM reconfiguration lost video or failed to trim it");
            expect(buffer.videoKeyFrameCount() == expectedCount / 2,
                "RAM reconfiguration changed the retained keyframe count");
            expect(buffer.saveTo(root / L"clip.mp4", defaultVideoSettings()),
                "save after RAM reconfiguration failed");
            const auto& samples = backtrack::g_lastMuxedInputs.videoSamples;
            expect(samples.size() == expectedCount, "RAM reconfiguration saved the wrong packets");
            std::vector<char> expectedBytes;
            for (size_t i = 0; i < expectedCount; ++i) {
                const int64_t pts = scenario.firstPts + static_cast<int64_t>(i);
                expect(samples[i].pts100ns == pts * kSecond100ns &&
                       samples[i].offset == i * kPacketBytes && samples[i].size == kPacketBytes,
                    "RAM reconfiguration changed a saved sample");
                expectedBytes.insert(expectedBytes.end(), kPacketBytes, static_cast<char>(0x30 + pts));
            }
            expect(backtrack::g_lastVideoStreamBytes == expectedBytes,
                "RAM reconfiguration changed saved video bytes");
        }
        std::filesystem::remove_all(root, error);
    }
}

void testReorderedPresentationEndAndConsecutiveSaves() {
    backtrack::g_captureAudioBytes = true;
    for (const bool diskVideo : {false, true}) {
        for (const bool diskAudio : {false, true}) {
            const auto root = testOutputDir() /
                (diskVideo ? (diskAudio ? L"reordered_dd" : L"reordered_dr")
                           : (diskAudio ? L"reordered_rd" : L"reordered_rr"));
            std::error_code error;
            std::filesystem::remove_all(root, error);
            {
                ReplaySettings replaySettings = settings(30);
                replaySettings.diskBackedVideo = diskVideo;
                replaySettings.diskBackedAudio = diskAudio;
                ReplayBuffer buffer;
                buffer.configure(replaySettings, root, 8ull * 1024 * 1024, 4ull * 1024 * 1024);
                const int64_t presentationOrder[] = {0, 2, 1, 3, 5, 4, 6, 8, 7, 9, 10, 12, 11};
                for (size_t i = 0; i < std::size(presentationOrder); ++i) {
                    const int64_t pts = presentationOrder[i];
                    EncodedPacket packet = makeVideoPayload(
                        pts * kSecond100ns, kSecond100ns, pts == 0 || pts == 3 || pts == 6 || pts == 10,
                        static_cast<uint8_t>(0x20 + i), 16);
                    packet.hasDts = true;
                    packet.dts100ns = (static_cast<int64_t>(i) - 2) * kSecond100ns;
                    expect(packet.dts100ns <= packet.pts100ns, "invalid reordered DTS fixture");
                    buffer.pushVideo(std::move(packet));
                }
                std::vector<AudioPacket> audio;
                for (int64_t pts = 0; pts < 15; ++pts) {
                    audio.push_back(makeMarkedAudio(AudioTrack::System, pts, static_cast<uint8_t>(pts + 1)));
                    if (pts < 13) {
                        buffer.pushAudio(audio.back());
                    }
                }
                auto expectedAudio = [&](size_t begin, size_t end) {
                    std::vector<uint8_t> bytes;
                    for (size_t i = begin; i < end; ++i) {
                        bytes.insert(bytes.end(), audio[i].bytes.begin(), audio[i].bytes.end());
                    }
                    return bytes;
                };
                expect(buffer.videoSpan100ns() == 13 * kSecond100ns,
                    "reordered video span used the decode-order tail");
                const auto output = root / L"clip.mp4";
                expect(buffer.saveTo(output, defaultVideoSettings()), "reordered save failed");
                const auto& firstSamples = backtrack::g_lastMuxedInputs.videoSamples;
                expect(firstSamples.size() == std::size(presentationOrder), "reordered save lost video");
                for (size_t i = 0; i < firstSamples.size(); ++i) {
                    expect(firstSamples[i].pts100ns == presentationOrder[i] * kSecond100ns &&
                           firstSamples[i].hasDts &&
                           firstSamples[i].dts100ns == (static_cast<int64_t>(i) - 2) * kSecond100ns,
                        "reordered save changed PTS or DTS");
                }
                expect(backtrack::g_lastSystemAudioBytes == expectedAudio(0, 13),
                    "reordered save dropped audio before the latest presentation end");

                expect(buffer.saveTo(output, defaultVideoSettings()), "consecutive reordered save failed");
                expect(backtrack::g_lastMuxedInputs.videoSamples.front().pts100ns == 10 * kSecond100ns,
                    "consecutive save did not use the previous presentation end");
                expect(backtrack::g_lastSystemAudioBytes == expectedAudio(10, 13),
                    "consecutive reordered save cut audio at the decode-order tail");

                expect(buffer.extendLastVideoDuration(11 * kSecond100ns, 4 * kSecond100ns),
                    "reordered tail extension failed");
                buffer.pushAudio(audio[13]);
                buffer.pushAudio(audio[14]);
                expect(buffer.videoSpan100ns() == 15 * kSecond100ns,
                    "reordered tail extension did not update the presentation end");
                expect(buffer.saveTo(output, defaultVideoSettings()), "extended reordered save failed");
                expect(backtrack::g_lastSystemAudioBytes == expectedAudio(10, 15),
                    "extended reordered save lost continuous audio");
            }
            std::filesystem::remove_all(root, error);
        }
    }
    backtrack::g_captureAudioBytes = false;
}

void testPresentationEndAfterWindowCuts() {
    backtrack::g_captureAudioBytes = true;
    for (const bool disk : {false, true}) {
        for (const bool laterKeyframe : {false, true}) {
            const auto root = testOutputDir() /
                (disk ? (laterKeyframe ? L"cut_disk_key" : L"cut_disk_hold")
                      : (laterKeyframe ? L"cut_ram_key" : L"cut_ram_hold"));
            std::error_code error;
            std::filesystem::remove_all(root, error);
            {
                ReplaySettings replaySettings = settings(laterKeyframe ? 6 : 3);
                replaySettings.diskBackedVideo = disk;
                replaySettings.diskBackedAudio = disk;
                ReplayBuffer buffer;
                buffer.configure(replaySettings, root, 8ull * 1024 * 1024, 4ull * 1024 * 1024);
                const int64_t ptsWithKey[] = {0, 1, 2, 4, 3, 6};
                const int64_t ptsWithoutKey[] = {0, 1, 3, 2};
                const size_t count = laterKeyframe ? std::size(ptsWithKey) : std::size(ptsWithoutKey);
                for (size_t i = 0; i < count; ++i) {
                    const int64_t pts = laterKeyframe ? ptsWithKey[i] : ptsWithoutKey[i];
                    const bool key = pts == 0 || (laterKeyframe && (pts == 2 || pts == 6));
                    EncodedPacket packet = makeVideo(pts * kSecond100ns,
                        (pts == 1 ? 11 : 1) * kSecond100ns, key);
                    packet.hasDts = true;
                    packet.dts100ns = (static_cast<int64_t>(i) - 2) * kSecond100ns;
                    expect(packet.dts100ns <= packet.pts100ns, "invalid cut DTS fixture");
                    buffer.pushVideo(std::move(packet));
                }
                std::vector<AudioPacket> audio;
                for (int64_t pts = 0; pts < 12; ++pts) {
                    audio.push_back(makeMarkedAudio(AudioTrack::System, pts, static_cast<uint8_t>(pts + 1)));
                    buffer.pushAudio(audio.back());
                }
                std::vector<uint8_t> expectedAudio;
                const size_t begin = laterKeyframe ? 2 : 0;
                const size_t end = laterKeyframe ? 7 : 5;
                for (size_t i = begin; i < end; ++i) {
                    expectedAudio.insert(expectedAudio.end(), audio[i].bytes.begin(), audio[i].bytes.end());
                }
                expect(buffer.videoSpan100ns() == 5 * kSecond100ns,
                    "span did not recompute the presentation end after the window cut");
                const auto output = root / L"clip.mp4";
                expect(buffer.saveTo(output, defaultVideoSettings()), "window-cut save failed");
                const auto& samples = backtrack::g_lastMuxedInputs.videoSamples;
                expect(samples.front().pts100ns == static_cast<int64_t>(begin) * kSecond100ns,
                    "window cut chose the wrong keyframe");
                if (!laterKeyframe) {
                    expect(samples[1].duration100ns == 4 * kSecond100ns,
                        "window cut did not bound the non-tail hold");
                }
                expect(backtrack::g_lastSystemAudioBytes == expectedAudio,
                    "window cut used an end from removed or shortened video");
                if (laterKeyframe) {
                    expect(buffer.saveTo(output, defaultVideoSettings()), "save after window cut failed");
                    expect(backtrack::g_lastMuxedInputs.videoSamples.front().pts100ns == 2 * kSecond100ns,
                        "consecutive save used the removed packet's presentation end");
                    expect(backtrack::g_lastSystemAudioBytes == expectedAudio,
                        "consecutive save after window cut changed audio");
                }
            }
            std::filesystem::remove_all(root, error);
        }
    }
    backtrack::g_captureAudioBytes = false;
}

void testUncuttableFramesKeepTheirPresentationEnd() {
    for (const bool disk : {false, true}) {
        const auto root = testOutputDir() / (disk ? L"uncuttable_disk" : L"uncuttable_ram");
        std::error_code error;
        std::filesystem::remove_all(root, error);
        {
            ReplaySettings replaySettings = settings(30);
            replaySettings.diskBackedVideo = disk;
            ReplayBuffer buffer;
            buffer.configure(replaySettings, root, 8ull * 1024 * 1024);
            const int64_t pts[] = {0, 6, 1};
            for (size_t i = 0; i < std::size(pts); ++i) {
                EncodedPacket packet = makeVideo(pts[i] * kSecond100ns, kSecond100ns, i == 0);
                packet.hasDts = true;
                packet.dts100ns = (static_cast<int64_t>(i) - 1) * kSecond100ns;
                buffer.pushVideo(std::move(packet));
            }
            replaySettings.seconds = 3;
            buffer.configure(replaySettings, root, 8ull * 1024 * 1024);
            for (int64_t i = 0; i < 7; ++i) {
                buffer.pushAudio(makeAudio(i * kSecond100ns, 1));
            }
            expect(buffer.videoSpan100ns() == 7 * kSecond100ns,
                "uncuttable reordered video reported a shortened span");
            expect(buffer.saveTo(root / L"clip.mp4", defaultVideoSettings()), "uncuttable save failed");
            expect(backtrack::g_lastMuxedInputs.videoSamples[1].duration100ns == kSecond100ns,
                "window cut zeroed a frame beyond the limit");
            expect(backtrack::g_lastSystemWav.has_value() &&
                   backtrack::g_lastSystemWav->dataBytes == 7ull * 48000 * 4,
                "uncuttable video lost audio before its presentation end");
        }
        std::filesystem::remove_all(root, error);
    }
}

// A day-long stall (exclusive-fullscreen hold) must not stretch the replay
// past its window: extendLastVideoDuration clamps a rogue tail to 60 s and
// saveTo cuts the tail to the window plus one GOP of keyframe tolerance.
// Without both, the raw 86400 s duration publishes window + tail.
void testDayHoldStaysWithinWindow() {
    constexpr int64_t kDay100ns = 86400 * kSecond100ns;
    for (const uint32_t windowSeconds : {30u, 120u}) {
        ReplayBuffer buffer;
        buffer.configure(settings(windowSeconds));
        // Window worth of 1 s packets, keyframe every 2 s; the newest packet
        // is a P-frame so the tail-budget shrink (not keyframe realign) bounds it.
        for (int64_t pts = 0; pts < static_cast<int64_t>(windowSeconds); ++pts) {
            buffer.pushVideo(makeVideo(pts * kSecond100ns, kSecond100ns, (pts % 2) == 0));
        }
        const int64_t newestPts = (static_cast<int64_t>(windowSeconds) - 1) * kSecond100ns;
        // Healthy extends still apply: a 33 ms live-tail increment stretches the span.
        expect(buffer.extendLastVideoDuration(newestPts, kSecond100ns + 330000),
            "healthy tail extension was rejected");
        expect(buffer.videoSpan100ns() == static_cast<int64_t>(windowSeconds) * kSecond100ns + 330000,
            "healthy tail extension did not extend the video span");
        // Day-long hold: clamped to 60 s, then cut to window + one GOP (2 s default).
        expect(buffer.extendLastVideoDuration(newestPts, kDay100ns),
            "day-long tail extension was rejected");
        const int64_t bound = static_cast<int64_t>(windowSeconds) * kSecond100ns + 2 * kSecond100ns;
        expect(buffer.videoSpan100ns() <= bound,
            "day-long hold stretched the video span past window + GOP");
        const auto output = testOutputDir() / L"dayhold.mp4";
        std::error_code error;
        std::filesystem::create_directories(output.parent_path(), error);
        expect(buffer.saveTo(output, defaultVideoSettings()), "saveTo failed after a day-long hold");
        const auto& samples = backtrack::g_lastMuxedInputs.videoSamples;
        expect(!samples.empty(), "day-long hold saved no video");
        expect(samples.front().keyFrame, "day-long hold clip does not start at a keyframe");
        const int64_t savedSpan =
            samples.back().pts100ns + samples.back().duration100ns - samples.front().pts100ns;
        expect(savedSpan <= bound,
            "day-long hold published " + std::to_string(savedSpan) +
                " 100ns units, past window + GOP (" + std::to_string(bound) + ")");
        std::filesystem::remove_all(output.parent_path(), error);
    }
}

// A long window at a high bitrate must not grow RAM without bound: the passed
// capacities cap the memory backends too, video realigning to a keyframe and
// audio evicting oldest-first. The same cap survives a disk-to-RAM failover.
void testRamModeEnforcesByteCap() {
    constexpr uint64_t kVideoCap = 16ull * 1024;
    constexpr uint64_t kAudioCap = 256ull * 1024;
    constexpr int64_t kPackets = 100;

    ReplayBuffer buffer;
    buffer.configure(settings(3600), {}, kVideoCap, kAudioCap);
    for (int64_t pts = 0; pts < kPackets; ++pts) {
        buffer.pushVideo(makeVideoPayload(
            pts * kSecond100ns, kSecond100ns, (pts % 5) == 0, static_cast<uint8_t>(0x30 + pts), 1024));
    }
    expect(backtrack::ReplayBufferTestAccess::ramVideoBytes(buffer) <= kVideoCap,
        "RAM video grew past its byte cap");
    expect(buffer.videoPacketCount() <= kVideoCap / 1024,
        "RAM video kept more packets than fit the byte cap");
    expect(buffer.newestVideoPts100ns() == (kPackets - 1) * kSecond100ns,
        "RAM byte eviction dropped the newest packet");
    expect(buffer.videoPacketCount() > 0, "RAM byte eviction emptied the buffer");

    for (int64_t pts = 0; pts < 10; ++pts) {
        buffer.pushAudio(makeAudio(pts * kSecond100ns, 1));
    }
    expect(backtrack::ReplayBufferTestAccess::ramAudioBytes(buffer) <= kAudioCap,
        "RAM audio grew past its byte cap");

    // Failover path: a dead disk ring lands on the same RAM cap, not uncapped.
    const auto storageRoot = testOutputDir() / L"ram_cap_failover";
    std::error_code error;
    std::filesystem::remove_all(storageRoot, error);
    std::filesystem::create_directories(storageRoot, error);
    ReplaySettings diskSettings = settings(3600);
    diskSettings.diskBackedVideo = true;
    ReplayBuffer disk;
    constexpr uint64_t kDiskCap = 8ull * 1024 * 1024;
    disk.configure(diskSettings, storageRoot, kDiskCap);
    for (int64_t pts = 0; pts < 6; ++pts) {
        disk.pushVideo(makeVideoPayload(
            pts * kSecond100ns, kSecond100ns, pts == 0, static_cast<uint8_t>(0x40 + pts), 1024));
    }
    backtrack::ReplayBufferTestAccess::breakRing(disk);
    const auto output = testOutputDir() / L"ram_cap_failover.mp4";
    std::wstring detail;
    expect(!disk.saveTo(output, defaultVideoSettings(), &detail),
        "a save reported success after the ring stopped reading");
    for (int64_t pts = 6; pts < 106; ++pts) {
        disk.pushVideo(makeVideoPayload(
            pts * kSecond100ns, kSecond100ns, (pts % 5) == 0, static_cast<uint8_t>(0x40 + pts), 100 * 1024));
    }
    expect(backtrack::ReplayBufferTestAccess::ramVideoBytes(disk) <= kDiskCap,
        "post-failover RAM video grew past its byte cap");
    expect(disk.newestVideoPts100ns() == 105 * kSecond100ns,
        "post-failover eviction dropped the newest packet");
    expect(disk.saveTo(output, defaultVideoSettings()),
        "the session could not save again after failover under a byte cap");

    disk.clear();
    buffer.clear();
    std::filesystem::remove_all(storageRoot, error);
    std::filesystem::remove_all(output.parent_path(), error);
}

// A WAV finalization failure (header patch or final flush) used to be
// unreportable: the replay was published with shortened audio and its source
// session deleted. It must fail the save and preserve the session instead.
void testAudioFinalizeFailureKeepsSession() {
    std::error_code error;
    std::filesystem::remove_all(testOutputDir(), error);

    ReplayBuffer buffer;
    buffer.configure(settings(30));
    for (int64_t pts = 0; pts < 6; ++pts) {
        buffer.pushVideo(makeVideoPayload(
            pts * kSecond100ns, kSecond100ns, pts == 0, static_cast<uint8_t>(0xF0 + pts), 1024));
    }
    buffer.pushAudio(makeAudio(kSecond100ns, 1));

    // The stub records what the muxer was handed; clearing it makes "was the
    // muxer reached at all" a real assertion rather than a leftover value.
    backtrack::g_lastMuxedInputs.videoSamples.clear();
    backtrack::WavWriter::failFinalizeForTesting(true);
    const auto output = testOutputDir() / L"finalize_failure" / L"clip.mp4";
    std::wstring detail;
    const bool saved = buffer.saveTo(output, defaultVideoSettings(), &detail);
    backtrack::WavWriter::failFinalizeForTesting(false);

    expect(!saved, "saveTo published a replay whose audio could not be finalized");
    expect(!detail.empty(), "a failed audio finalization produced no reason");
    expect(backtrack::g_lastMuxedInputs.videoSamples.empty(),
        "a replay with unfinalized audio reached the muxer");
    expect(!std::filesystem::exists(output, error),
        "a replay with unfinalized audio was published");
    const auto staging = output.parent_path() / L".backtrack_tmp" / output.stem();
    expect(std::filesystem::exists(staging / backtrack::RecoveryStore::kRecoveryManifestName, error),
        "a failed audio finalization dropped the streams needed for recovery");

    buffer.clear();
    std::filesystem::remove_all(testOutputDir(), error);
}

} // namespace

int main() {
    try {
        testKeyframeCountingAndTrim();
        testTailExtension();
        testSaveToHonorsLiveTailAndKeyframeStart();
        testConsecutiveSaveOverlap();
        testConfiguredConsecutiveSaveOverlap();
        testAudioTimelineMatchesVideo();
        testSaveWithoutKeyframeFails();
        testParseWavRecoversUnpatchedCrashHeader();
        testRecoveryManifestRoundTripIsAtomic();
        testSessionDirectoriesAreExclusivelyCreated();
        testFailedManifestRewriteKeepsIncrementalPrefix();
        testFailedMuxWithoutManifestKeepsStreams();
        testStartupRecoversAllValidSessionsAndKeepsFailures();
        testRecoveryTrimsSamplesBeyondStreamEnd();
        testRecoveryKeepsPrefixOnTornFinalLine();
        testRecoveryRejectsPlausibleNonContiguousTail();
        testRecoveryAcceptsFewerLinesThanDeclared();
        testRecoveryStillRejectsDamagedMetadata();
        testDiskBackedRingRoundTripAndWrap();
        testDiskBackedMatchesRamBackend();
        testNewestVideoPtsRam();
        testNewestVideoPtsDiskAndBackendSwitch();
        testRamVideoSurvivesReconfiguration();
        testReorderedPresentationEndAndConsecutiveSaves();
        testPresentationEndAfterWindowCuts();
        testUncuttableFramesKeepTheirPresentationEnd();
        testDiskRingSurvivesCapacityChange();
        testRingOpenFailureReportsFallback();
        testRingRejectsPacketLargerThanCapacity();
        testDiskBackendPreservesDecodeTimestamps();
        testParameterSetsArePrependedOnBothBackends();
        testBitstreamClassifiersMatchIdrOnly();
        testParameterSetClassifiersRequireEveryCodecHeader();
        testBitstreamClassifiersHandleTruncatedAndTrailingInput();
        testExtractParameterSetsCopiesTrailingNalWhole();
        testDiskBackendSavesAudio();
        testDiskAudioRoundTrip();
        testDiskAudioRoundTripAcrossWrap();
        testDiskAudioInsertionOrderAcrossTrackDrains();
        testDiskAudioAgeTrimKeepsPhysicalPrefix();
        testDiskAudioSurvivesCapacityChange();
        testAudioRingReadFailureFailsOverAudioOnly();
        testReplayAudioGapPreservesDuration();
        testManifestPublishSurvivesSharingViolation();
        testDirectoryWithoutManifestIsNotRecovered();
        testBackendAndDirectorySwitchesResetBuffer();
        testRingReadFailureFailsSaveAndFailsOver();
        testMuxFailurePreservesStreamsAndReportsReason();
        testAudioFinalizeFailureKeepsSession();
        testSameNameRetryDoesNotTouchPreservedReplay();
        testOrphanedRingFolderIsPreserved();
        testDayHoldStaysWithinWindow();
        testRamModeEnforcesByteCap();
        std::cout << "ReplayBuffer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ReplayBuffer test failure: " << error.what() << '\n';
        return 1;
    }
}
