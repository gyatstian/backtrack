// Audio persistence regressions (audit finding #14):
// - a source WAV that existed when the save started but cannot be read back
//   (read-denied WAV) fails the save instead of silently becoming "no audio"
// - a PTS gap longer than the old 10 s fill cap is preserved in the saved
//   audio (timed markers around a long outage)
// - WavWriter keeps accepting audio past the 4 GiB RIFF boundary (markers
//   beyond RIFF boundary) instead of dropping it while returning success

#include "audio/AudioMix.h"
#include "audio/WavReader.h"
#include "mux/AudioTimeline.h"
#include "mux/WavWriter.h"

#include <Windows.h>
#include <mmreg.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using backtrack::AudioPacket;
using backtrack::AudioTrack;
using backtrack::MuxedInputs;
using backtrack::WaveFormatBlob;
using backtrack::WavWriter;

constexpr int64_t kSecond100ns = backtrack::kHundredNanosecondsPerSecond;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path testOutputDir() {
    return std::filesystem::temp_directory_path() / L"backtrack_audio_persistence_test";
}

WaveFormatBlob pcm16Stereo() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    WaveFormatBlob blob;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&format);
    blob.bytes.assign(bytes, bytes + sizeof(format));
    return blob;
}

// --- read-denied WAV ---

void testReadDeniedExpectedWavFailsSave() {
    std::error_code error;
    const auto dir = testOutputDir() / L"readdenied";
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);

    // The track existed when the save started (it was just recorded) but reads
    // back as garbage: a failed read, not "no audio".
    const auto badWav = dir / L"system.wav";
    {
        const char garbage[] = "this is not a wav at all";
        std::ofstream out(badWav, std::ios::binary | std::ios::trunc);
        out.write(garbage, static_cast<std::streamsize>(sizeof(garbage) - 1));
    }
    MuxedInputs inputs;
    inputs.videoPath = dir / L"video.h264";
    inputs.systemAudioPath = badWav;
    std::filesystem::path mixedPath = dir / L"mixed_audio.wav";
    expect(!backtrack::buildMixedPcmAudio(inputs, dir / L"out.mp4", mixedPath),
        "a corrupt expected WAV did not fail the save");
    expect(mixedPath.empty(), "a failed mix left a mixed path behind");

    std::filesystem::remove_all(testOutputDir(), error);
}

void testReadDeniedLockedWavFailsSave() {
    std::error_code error;
    const auto dir = testOutputDir() / L"readdeniedlock";
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);

    // A genuine read denial: the file exists and is a valid WAV, but another
    // handle holds it with no sharing, so every open attempt fails.
    const auto lockedWav = dir / L"system.wav";
    {
        WavWriter writer;
        expect(writer.open(lockedWav, pcm16Stereo()), "open failed");
        std::vector<uint8_t> payload(48000 * 4, 0x80);
        expect(writer.write(payload.data(), payload.size()), "write failed");
        writer.close();
    }
    HANDLE lock = CreateFileW(lockedWav.c_str(), GENERIC_READ, 0 /* no FILE_SHARE_* */,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    expect(lock != INVALID_HANDLE_VALUE, "could not lock the WAV fixture exclusively");

    MuxedInputs inputs;
    inputs.videoPath = dir / L"video.h264";
    inputs.systemAudioPath = lockedWav;
    std::filesystem::path mixedPath = dir / L"mixed_audio.wav";
    expect(!backtrack::buildMixedPcmAudio(inputs, dir / L"out.mp4", mixedPath),
        "a read-denied expected WAV did not fail the save");
    expect(mixedPath.empty(), "a failed mix left a mixed path behind");

    CloseHandle(lock);
    std::filesystem::remove_all(testOutputDir(), error);
}

void testAbsentTrackStaysVideoOnly() {
    // No file on either track: the save must continue video-only, not fail.
    MuxedInputs inputs;
    inputs.videoPath = testOutputDir() / L"video.h264";
    inputs.microphoneAudioPath = testOutputDir() / L"missing.wav";
    std::filesystem::path mixedPath = testOutputDir() / L"mixed_audio.wav";
    expect(backtrack::buildMixedPcmAudio(inputs, testOutputDir() / L"out.mp4", mixedPath),
        "an absent audio track failed the save");
    expect(mixedPath.empty(), "absent audio produced a mixed stream");
}

void testValidEmptyWavIsSkippedNotFailed() {
    std::error_code error;
    const auto dir = testOutputDir() / L"empty";
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    {
        WavWriter writer;
        expect(writer.open(dir / L"empty.wav", pcm16Stereo()), "open failed");
        writer.close();  // valid header, zero samples
    }
    MuxedInputs inputs;
    inputs.videoPath = dir / L"video.h264";
    inputs.systemAudioPath = dir / L"empty.wav";
    std::filesystem::path mixedPath = dir / L"mixed_audio.wav";
    expect(backtrack::buildMixedPcmAudio(inputs, dir / L"out.mp4", mixedPath),
        "a valid empty WAV failed the save");
    expect(mixedPath.empty(), "an empty WAV produced a mixed stream");
    std::filesystem::remove_all(testOutputDir(), error);
}

void testValidWavMixes() {
    std::error_code error;
    const auto dir = testOutputDir() / L"mix";
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    const auto systemWav = dir / L"system.wav";
    {
        WavWriter writer;
        expect(writer.open(systemWav, pcm16Stereo()), "open failed");
        std::vector<uint8_t> payload(48000 * 4, 0x80);  // 1 s of 16-bit PCM
        expect(writer.write(payload.data(), payload.size()), "write failed");
        writer.close();
    }
    MuxedInputs inputs;
    inputs.videoPath = dir / L"video.h264";
    inputs.systemAudioPath = systemWav;
    std::filesystem::path mixedPath;
    expect(backtrack::buildMixedPcmAudio(inputs, dir / L"out.mp4", mixedPath),
        "a valid WAV failed to mix");
    expect(!mixedPath.empty() && std::filesystem::exists(mixedPath),
        "mixing a valid WAV produced no output");
    backtrack::WavInfo info;
    expect(backtrack::parseWav(mixedPath, info), "the mixed WAV is unreadable");
    expect(info.dataBytes == 48000 * 4, "the mixed WAV lost audio");
    std::filesystem::remove_all(testOutputDir(), error);
}

// --- timed markers around a long outage ---

void testLongOutagePreservesGapDuration() {
    std::error_code error;
    const auto dir = testOutputDir() / L"outage";
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    const auto path = dir / L"timeline.wav";

    WavWriter writer;
    expect(writer.open(path, pcm16Stereo()), "open failed");
    int64_t nextPts = 0;
    backtrack::AudioGapStats gapStats;

    // Marker at 0 s: one second of audio on the timeline.
    AudioPacket first;
    first.track = AudioTrack::System;
    first.frameCount = 48000;
    first.pts100ns = 0;
    first.bytes.assign(48000 * 4, 0x80);
    expect(backtrack::paceAudioPacket(
               writer, nextPts, first,
               (std::numeric_limits<int64_t>::max)(), L"test", L"timeline.wav", gapStats),
        "pacing the first marker failed");

    // 40 s of silence, then a resume marker at 40 s: the saved audio must span
    // the full 40 s so the resumed audio lands at its true absolute time.
    AudioPacket resumed;
    resumed.track = AudioTrack::System;
    resumed.frameCount = 48000;
    resumed.pts100ns = 40 * kSecond100ns;
    resumed.bytes.assign(48000 * 4, 0x7f);
    expect(backtrack::paceAudioPacket(
               writer, nextPts, resumed,
               (std::numeric_limits<int64_t>::max)(), L"test", L"timeline.wav", gapStats),
        "pacing the resumed marker failed");
    writer.close();

    expect(gapStats.fillEvents == 1 && gapStats.fillBytes == 39ull * 48000 * 4 &&
            gapStats.maxGapMs == 39000,
        "the full-gap fill did not update the gap statistics");

    backtrack::WavInfo info;
    expect(backtrack::parseWav(path, info), "the outage WAV is unreadable");
    // 1 s packet + 39 s of gap silence + 1 s packet = 41 s on the timeline.
    // Compressing the gap to 10 s would leave only 12 s.
    expect(info.dataBytes == 41ull * 48000 * 4,
        "the long outage did not preserve its gap duration in the saved audio (got " +
            std::to_string(info.dataBytes) + " bytes)");
    std::filesystem::remove_all(testOutputDir(), error);
}

// --- markers beyond the RIFF boundary ---

void testWritesPastRiffBoundaryAreKept() {
    std::error_code error;
    const auto dir = testOutputDir() / L"riff";
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    const auto path = dir / L"long.wav";

    WavWriter writer;
    expect(writer.open(path, pcm16Stereo()), "open failed");
    // Shrink the RIFF boundary so the large-file path is reachable without
    // writing 4 GiB (production keeps the real ~4 GiB boundary).
    writer.setMaxDataBytesForTesting(16);

    std::vector<uint8_t> payload(32, 0x7f);
    expect(writer.write(payload.data(), payload.size()),
        "a write past the RIFF boundary failed");
    expect(writer.dataBytes() == 32,
        "bytes past the RIFF boundary were not counted");
    writer.close();

    const auto fileBytes = std::filesystem::file_size(path, error);
    expect(!error && fileBytes == 46 + 32,
        "bytes past the RIFF boundary were not stored on disk");
    std::filesystem::remove_all(testOutputDir(), error);
}

} // namespace

int main() {
    try {
        testReadDeniedExpectedWavFailsSave();
        testReadDeniedLockedWavFailsSave();
        testAbsentTrackStaysVideoOnly();
        testValidEmptyWavIsSkippedNotFailed();
        testValidWavMixes();
        testLongOutagePreservesGapDuration();
        testWritesPastRiffBoundaryAreKept();
        std::cout << "AudioPersistence tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AudioPersistence test failure: " << error.what() << '\n';
        return 1;
    }
}
