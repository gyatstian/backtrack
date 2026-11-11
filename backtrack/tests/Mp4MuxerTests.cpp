#include "mux/Mp4Muxer.h"
#include "mux/RecoveryStore.h"
#include "mux/WavReader.h"

#include <Windows.h>
#include <mmreg.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace backtrack {

struct Mp4MuxerTestAccess {
    static void failVideoWrite(Mp4Muxer& muxer) {
        muxer.videoStream_.flush();
        if (!muxer.videoStream_) {
            throw std::runtime_error("could not flush the video fixture");
        }
        muxer.videoStream_.setstate(std::ios::badbit);
    }
};

} // namespace backtrack

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    expect(stream.is_open(), "could not read the preserved file");
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

// --- WAV extent past the RIFF boundary ---

// WavWriter's large-file form: the data size field is clamped to the uint32
// maximum and the payload runs to EOF. The fixture shrinks the boundary so the
// shape is reachable without writing 4 GiB: the declared RIFF extent ends
// inside the data chunk while the file keeps the rest of the payload.
constexpr uint32_t kClampedDataSize = 0xFFFFFFFFu;
// RIFF(12) + fmt chunk(8 + sizeof(WAVEFORMATEX)) + data header(8).
constexpr uint32_t kWavDataOffset = 12 + 8 + static_cast<uint32_t>(sizeof(WAVEFORMATEX)) + 8;

void appendU32(std::vector<uint8_t>& bytes, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<uint8_t>(value >> shift));
    }
}

std::vector<uint8_t> wavFixture(
    uint32_t declaredRiffSize,
    uint32_t declaredDataSize,
    const std::vector<uint8_t>& payload,
    const std::vector<uint8_t>& trailingChunk) {
    std::vector<uint8_t> bytes;
    auto appendTag = [&bytes](const char* tag) { bytes.insert(bytes.end(), tag, tag + 4); };
    appendTag("RIFF");
    appendU32(bytes, declaredRiffSize);
    appendTag("WAVE");
    appendTag("fmt ");
    appendU32(bytes, static_cast<uint32_t>(sizeof(WAVEFORMATEX)));
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = 48000 * 4;
    const auto* formatBytes = reinterpret_cast<const uint8_t*>(&format);
    bytes.insert(bytes.end(), formatBytes, formatBytes + sizeof(WAVEFORMATEX));
    appendTag("data");
    appendU32(bytes, declaredDataSize);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    bytes.insert(bytes.end(), trailingChunk.begin(), trailingChunk.end());
    return bytes;
}

void writeFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    expect(stream.is_open(), "could not create the WAV fixture");
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    expect(static_cast<bool>(stream), "could not write the WAV fixture");
}

// The mux decides how many frames to write from the extent it reads back. A
// clamped header must not end the MP4 audio track at the boundary: the trailing
// payload has to stay part of the track, and the frame count has to cover it.
void testClampedWavExtentKeepsTrailingAudio() {
    const auto directory = std::filesystem::temp_directory_path() / L"backtrack_mux_wav_extent_test";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    expect(!error, "could not create the WAV fixture directory");

    const auto path = directory / L"mixed_audio.wav";
    constexpr uint32_t kBoundaryIntoPayload = 16;
    std::vector<uint8_t> payload(4096 * 4, 0x11);
    const std::vector<uint8_t> marker{0xde, 0xad, 0xbe, 0xef};
    std::copy(marker.begin(), marker.end(), payload.end() - 4);
    writeFile(path, wavFixture(
        kWavDataOffset + kBoundaryIntoPayload - 8, kClampedDataSize, payload, {}));

    backtrack::WavInfo capped;
    expect(backtrack::parseWav(path, capped), "the clamped-header WAV was rejected");
    expect(capped.dataBytes == kBoundaryIntoPayload,
        "the fixture does not exercise the RIFF boundary cap");

    backtrack::WavInfo info;
    expect(backtrack::parseMuxWav(path, info), "the clamped-header WAV extent was not recovered");
    expect(info.dataOffset == kWavDataOffset, "the recovered WAV data offset is wrong");
    expect(info.dataBytes == payload.size(),
        "the mux lost the audio past the RIFF boundary (got " +
            std::to_string(info.dataBytes) + " of " + std::to_string(payload.size()) + " bytes)");
    const uint64_t frames = info.dataBytes / info.blockAlign;
    expect(frames == payload.size() / 4, "the mux frame count does not cover the recovered extent");
    // 4096 frames at 48 kHz: the duration the MP4 track must report.
    expect(frames == 4096, "the mux audio duration is wrong");

    std::ifstream stream(path, std::ios::binary);
    expect(stream.is_open(), "could not read the WAV fixture back");
    std::vector<uint8_t> lastFrame(4, 0);
    stream.seekg(static_cast<std::streamoff>(info.dataOffset + (frames - 1) * info.blockAlign), std::ios::beg);
    stream.read(reinterpret_cast<char*>(lastFrame.data()), static_cast<std::streamsize>(lastFrame.size()));
    expect(stream && lastFrame == marker,
        "the trailing audio past the RIFF boundary is not readable at the recovered extent");

    std::filesystem::remove_all(directory, error);
}

// A well-formed WAV must keep its exact extent, and a real trailing chunk after
// the data must never be counted as audio.
void testDeclaredExtentIsTrustedWhenNotClamped() {
    const auto directory = std::filesystem::temp_directory_path() / L"backtrack_mux_wav_extent_trusted_test";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    expect(!error, "could not create the WAV fixture directory");

    std::vector<uint8_t> payload(48000 * 4, 0x22);
    const auto wellFormed = directory / L"well_formed.wav";
    writeFile(wellFormed, wavFixture(
        static_cast<uint32_t>(kWavDataOffset + payload.size() - 8),
        static_cast<uint32_t>(payload.size()), payload, {}));
    backtrack::WavInfo info;
    expect(backtrack::parseMuxWav(wellFormed, info), "the well-formed WAV was rejected");
    expect(info.dataBytes == payload.size(), "a well-formed WAV extent changed");

    const auto withTrailingChunk = directory / L"trailing_chunk.wav";
    std::vector<uint8_t> trailing{'L', 'I', 'S', 'T', 8, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<uint8_t> shortPayload(32, 0x33);
    writeFile(withTrailingChunk, wavFixture(
        static_cast<uint32_t>(kWavDataOffset + shortPayload.size() + trailing.size() - 8),
        static_cast<uint32_t>(shortPayload.size()), shortPayload, trailing));
    expect(backtrack::parseMuxWav(withTrailingChunk, info), "the trailing-chunk WAV was rejected");
    expect(info.dataBytes == shortPayload.size(),
        "a real trailing chunk was counted as audio (got " +
            std::to_string(info.dataBytes) + " bytes)");

    std::filesystem::remove_all(directory, error);
}

void testRecoveryNumbersRequireCompleteInRangeTokens() {
    const auto directory = std::filesystem::temp_directory_path() /
        (L"backtrack_manifest_numbers_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto video = directory / L"video.h264";
    writeFile(video, std::vector<uint8_t>(12, 0x11));
    const auto manifest = directory / backtrack::RecoveryStore::kRecoveryManifestName;
    const std::string header = "backtrack-recovery-v1\nvideo\tvideo.h264\n";
    const std::string first = "sample\t0\t4\t-1\t333333\t1\t-2\t1\n";
    const auto retained = [&](const std::string& text, unsigned count) {
        {
            std::ofstream stream(manifest, std::ios::binary | std::ios::trunc);
            stream << text;
            expect(static_cast<bool>(stream), "could not write numeric manifest fixture");
        }
        const auto detail = backtrack::RecoveryStore::preserveFailedRecording(
            {}, directory / L"clip.mp4", directory, L"test failure");
        if (count == 0) {
            expect(detail.find(L"manual recovery") != std::wstring::npos,
                "damaged recovery metadata was accepted");
        } else {
            expect(detail.find(L"Preserved " + std::to_wstring(count) + L" video packets") != std::wstring::npos,
                "strict numeric parsing lost the valid prefix or accepted a damaged tail");
        }
    };
    retained(header + "fps\t4294967295\nsamples\t0001\n" + first, 1);
    retained(header + "sample\t0\t4\t-9223372036854775808\t9223372036854775807\t4294967295\t0\t4294967295\n", 1);
    for (const std::string value : {"", "-1", "+1", " 1", "1 ", "1x", "4294967296", "18446744073709551616"}) {
        retained(header + "fps\t" + value + "\n" + first, 0);
    }
    for (const std::string value : {"", "-1", "+1", " 1", "1 ", "1x", "18446744073709551616"}) {
        retained(header + "samples\t" + value + "\n" + first, 0);
    }
    const std::vector<std::vector<std::string>> invalidColumns = {
        {"", "-4", "+4", " 4", "4 ", "4x", "18446744073709551616"},
        {"", "-4", "+4", " 4", "4 ", "4x", "4294967296"},
        {"", "+1", " 1", "1 ", "1x", "9223372036854775808", "-9223372036854775809"},
        {"", "+1", " 1", "1 ", "1x", "9223372036854775808", "-9223372036854775809"},
        {"", "-1", "+1", "1x", "4294967296"},
        {"", "+1", "1x", "9223372036854775808", "-9223372036854775809"},
        {"", "-1", "+1", "1x", "4294967296"},
    };
    for (size_t column = 0; column < invalidColumns.size(); ++column) {
        for (const auto& value : invalidColumns[column]) {
            std::vector<std::string> columns = {"4", "4", "1", "333333", "0", "0", "1"};
            columns[column] = value;
            std::string line = "sample";
            for (const auto& part : columns) line += '\t' + part;
            retained(header + first + line + "\nsample\t4\t4\t1\t333333\t0\t0\t1\n", 1);
        }
    }
    std::filesystem::remove_all(directory);
}

void testFailedManifestPublishRemovesStagingOnly() {
    const auto directory = std::filesystem::temp_directory_path() /
        (L"backtrack_manifest_publish_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    backtrack::MuxedInputs inputs;
    inputs.videoPath = directory / L"video.h264";
    backtrack::MuxedInputs::VideoSample sample;
    sample.size = 4;
    sample.pts100ns = -1;
    sample.dts100ns = -2;
    sample.hasDts = true;
    sample.duration100ns = 333333;
    sample.keyFrame = true;
    inputs.videoSamples.push_back(sample);
    writeFile(inputs.videoPath, std::vector<uint8_t>(4, 0x11));
    expect(backtrack::RecoveryStore::writeManifest(inputs, directory), "could not publish the baseline manifest");
    const auto manifest = directory / backtrack::RecoveryStore::kRecoveryManifestName;
    const auto before = readFile(manifest);
    const auto detail = backtrack::RecoveryStore::preserveFailedRecording(
        inputs, directory / L"clip.mp4", directory, L"test failure");
    expect(detail.find(L"Preserved 1 video packets") != std::wstring::npos,
        "signed sample timestamps did not round trip through the manifest");
    const HANDLE hold = CreateFileW(manifest.c_str(), GENERIC_READ, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    expect(hold != INVALID_HANDLE_VALUE, "could not lock the baseline manifest");
    inputs.fps = 144;
    const bool published = backtrack::RecoveryStore::writeManifest(inputs, directory);
    CloseHandle(hold);
    expect(!published && readFile(manifest) == before, "failed publish changed the previous manifest");
    expect(!std::filesystem::exists(directory / L"recovery.tsv.tmp"), "failed publish left its staging file");
    expect(backtrack::RecoveryStore::writeManifest(inputs, directory), "manifest publish could not retry after cleanup");
    // binary reads retain the CRLF emitted by the Windows text-mode writer.
    expect(readFile(manifest).find("fps\t144\r\n") != std::string::npos, "retried manifest did not contain the new settings");
    std::filesystem::remove_all(directory);
}

void testWriteFailurePreservesIncrementalRecording() {
    const auto directory = std::filesystem::temp_directory_path() / L"backtrack_mux_write_failure_test";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    expect(!error, "could not create the recording fixture");
    const auto output = directory / L"clip.mp4";
    const HANDLE reservation = CreateFileW(
        output.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr);
    expect(reservation != INVALID_HANDLE_VALUE, "could not reserve the recording output");
    CloseHandle(reservation);

    backtrack::Mp4Muxer muxer;
    backtrack::VideoSettings settings;
    settings.codec = backtrack::VideoCodec::H264;
    expect(muxer.startRecording(output, settings), "could not start the recording");
    backtrack::EncodedPacket packet;
    packet.bytes = {0, 0, 0, 1, 0x65, 0x11};
    packet.keyFrame = true;
    packet.duration100ns = 333333;
    muxer.writeVideoPacket(packet);
    packet.keyFrame = false;
    packet.pts100ns = 333333;
    muxer.writeVideoPacket(packet);

    const auto firstDir = directory / L".backtrack_tmp" / L"clip";
    const auto video = firstDir / L"video.h264";
    const auto manifest = firstDir / backtrack::RecoveryStore::kRecoveryManifestName;
    backtrack::Mp4MuxerTestAccess::failVideoWrite(muxer);
    packet.pts100ns = 666666;
    muxer.writeVideoPacket(packet);
    expect(muxer.finalize().empty(), "write failure published a recording");
    expect(!std::filesystem::exists(output), "failed recording kept its output reservation");
    expect(muxer.lastError().find(L"Recover Failed") != std::wstring::npos,
        "valid incremental manifest was not reported as recoverable");
    const auto videoBytes = readFile(video);
    const auto manifestBytes = readFile(manifest);
    expect(videoBytes.size() == packet.bytes.size() * 2, "recorded video prefix was lost");
    expect(manifestBytes.find("sample\t") != std::string::npos, "incremental samples were lost");

    muxer.abort();
    expect(readFile(video) == videoBytes && readFile(manifest) == manifestBytes,
        "abort deleted or changed the finalized failed recording");
    expect(muxer.startRecording(output, settings), "same-name retry could not start");
    muxer.writeVideoPacket(packet);
    expect(readFile(video) == videoBytes && readFile(manifest) == manifestBytes,
        "same-name recording retry changed the preserved session");
    muxer.abort();
    expect(readFile(video) == videoBytes && readFile(manifest) == manifestBytes,
        "aborting the retry changed the preserved session");
    std::filesystem::remove_all(directory, error);
    expect(!error, "could not remove the recording fixture");
}

} // namespace

int main() {
    try {
        testWriteFailurePreservesIncrementalRecording();
        testClampedWavExtentKeepsTrailingAudio();
        testDeclaredExtentIsTrustedWhenNotClamped();
        testRecoveryNumbersRequireCompleteInRangeTokens();
        testFailedManifestPublishRemovesStagingOnly();
        std::cout << "Mp4Muxer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Mp4Muxer test failure: " << error.what() << '\n';
        return 1;
    }
}
