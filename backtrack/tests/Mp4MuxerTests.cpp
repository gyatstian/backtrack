#include "mux/Mp4Muxer.h"
#include "mux/RecoveryStore.h"
#include "mux/WavReader.h"

#include <Windows.h>
#include <mmreg.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
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

// --- real mux round trip through Media Foundation ---

std::string hrText(HRESULT hr) {
    return std::to_string(static_cast<uint32_t>(hr));
}

std::string narrow(const std::wstring& text) {
    return std::string(text.begin(), text.end());
}

struct MfRuntime {
    MfRuntime() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        comOwned_ = SUCCEEDED(com);
        if (comOwned_ || com == RPC_E_CHANGED_MODE) {
            mfStarted_ = SUCCEEDED(MFStartup(MF_VERSION));
        }
    }

    ~MfRuntime() {
        if (mfStarted_) {
            MFShutdown();
        }
        if (comOwned_) {
            CoUninitialize();
        }
    }

    MfRuntime(const MfRuntime&) = delete;
    MfRuntime& operator=(const MfRuntime&) = delete;

    bool usable() const { return mfStarted_; }

private:
    bool comOwned_ = false;
    bool mfStarted_ = false;
};

std::shared_ptr<const backtrack::WaveFormatBlob> pcmFormat() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = 48000 * 4;
    auto blob = std::make_shared<backtrack::WaveFormatBlob>();
    const auto* bytes = reinterpret_cast<const uint8_t*>(&format);
    blob->bytes.assign(bytes, bytes + sizeof(WAVEFORMATEX));
    return blob;
}

backtrack::AudioPacket makeAudioPacket(uint32_t frames, int64_t pts100ns, uint8_t sample) {
    backtrack::AudioPacket packet;
    packet.track = backtrack::AudioTrack::System;
    packet.frameCount = frames;
    packet.pts100ns = pts100ns;
    packet.format = pcmFormat();
    packet.bytes.assign(static_cast<size_t>(frames) * 4, sample);
    return packet;
}

// H.264 SPS/PPS in Annex B form. The MPEG-4 sink builds avcC from the first
// sample's parameter sets, so a stream without them fails Finalize with
// MF_E_SINK_HEADERS_NOT_FOUND. From the OS encoder at 320x240; the synthetic
// slices around them only need a sample table, not a decodable picture.
const std::vector<uint8_t> kH264ParameterSets = {
    0, 0, 0, 1, 0x67, 0x4d, 0x40, 0x14, 0x96, 0x56, 0x0a, 0x0f, 0xd8, 0x08, 0x80, 0x00, 0x00,
    0x03, 0x00, 0x80, 0x00, 0x00, 0x1e, 0x06, 0xd0, 0x44, 0x22, 0x9c,
    0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80};

backtrack::EncodedPacket makeVideoPacket(
    int index,
    int64_t pts100ns,
    int64_t dts100ns,
    bool hasDts,
    int64_t duration100ns,
    bool keyFrame) {
    backtrack::EncodedPacket packet;
    if (keyFrame) {
        packet.bytes = kH264ParameterSets;
    }
    packet.bytes.insert(packet.bytes.end(),
        {0, 0, 0, 1, static_cast<uint8_t>(keyFrame ? 0x65 : 0x41), 0x88});
    for (int i = 0; i < 16; ++i) {
        packet.bytes.push_back(static_cast<uint8_t>((index * 31 + i) & 0xff));
    }
    packet.pts100ns = pts100ns;
    packet.dts100ns = dts100ns;
    packet.hasDts = hasDts;
    packet.duration100ns = duration100ns;
    packet.keyFrame = keyFrame;
    packet.codec = backtrack::VideoCodec::H264;
    return packet;
}

uint32_t fourcc(const char (&tag)[5]) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(tag[0])) << 24) |
        (static_cast<uint32_t>(static_cast<uint8_t>(tag[1])) << 16) |
        (static_cast<uint32_t>(static_cast<uint8_t>(tag[2])) << 8) |
        static_cast<uint32_t>(static_cast<uint8_t>(tag[3]));
}

uint32_t readBigEndian32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
        (static_cast<uint32_t>(data[1]) << 16) |
        (static_cast<uint32_t>(data[2]) << 8) |
        static_cast<uint32_t>(data[3]);
}

uint64_t readBigEndian64(const uint8_t* data) {
    return (static_cast<uint64_t>(readBigEndian32(data)) << 32) | readBigEndian32(data + 4);
}

struct Mp4Box {
    uint32_t type = 0;
    size_t totalSize = 0;
    const uint8_t* payload = nullptr;
    size_t payloadSize = 0;
};

std::optional<Mp4Box> readBox(const uint8_t* data, size_t size, size_t& offset) {
    if (offset + 8 > size) {
        return std::nullopt;
    }
    uint64_t totalSize = readBigEndian32(data + offset);
    size_t headerSize = 8;
    if (totalSize == 1) {
        if (offset + 16 > size) {
            return std::nullopt;
        }
        totalSize = readBigEndian64(data + offset + 8);
        headerSize = 16;
    } else if (totalSize == 0) {
        totalSize = size - offset;
    }
    if (totalSize < headerSize || totalSize > size - offset) {
        return std::nullopt;
    }
    Mp4Box box;
    box.type = readBigEndian32(data + offset + 4);
    box.totalSize = static_cast<size_t>(totalSize);
    box.payload = data + offset + headerSize;
    box.payloadSize = box.totalSize - headerSize;
    offset += box.totalSize;
    return box;
}

std::optional<Mp4Box> findBox(const Mp4Box& parent, uint32_t type) {
    size_t offset = 0;
    for (;;) {
        const auto box = readBox(parent.payload, parent.payloadSize, offset);
        if (!box) {
            return std::nullopt;
        }
        if (box->type == type) {
            return box;
        }
    }
}

struct Mp4SampleTimeline {
    uint32_t timescale = 0;
    std::vector<int64_t> dts100ns;
    std::vector<int64_t> pts100ns;
    std::vector<int32_t> compositionOffsets;
};

// Reads the video track's stts/ctts sample tables. Source-reader sample times
// need a decoder, which a synthetic (container-only) stream cannot provide.
bool readVideoTimeline(const std::filesystem::path& path, Mp4SampleTimeline& timeline) {
    const auto bytes = readFile(path);
    const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
    const Mp4Box root{0, bytes.size(), data, bytes.size()};
    const auto moov = findBox(root, fourcc("moov"));
    if (!moov) {
        return false;
    }

    size_t trackOffset = 0;
    for (;;) {
        const auto trak = readBox(moov->payload, moov->payloadSize, trackOffset);
        if (!trak) {
            return false;
        }
        if (trak->type != fourcc("trak")) {
            continue;
        }
        const auto mdia = findBox(*trak, fourcc("mdia"));
        if (!mdia) {
            continue;
        }
        const auto hdlr = findBox(*mdia, fourcc("hdlr"));
        if (!hdlr || hdlr->payloadSize < 12 || readBigEndian32(hdlr->payload + 8) != fourcc("vide")) {
            continue;
        }
        const auto mdhd = findBox(*mdia, fourcc("mdhd"));
        const auto minf = findBox(*mdia, fourcc("minf"));
        if (!mdhd || mdhd->payloadSize < 4 || !minf) {
            return false;
        }
        const auto stbl = findBox(*minf, fourcc("stbl"));
        if (!stbl) {
            return false;
        }
        const auto stts = findBox(*stbl, fourcc("stts"));
        if (!stts || stts->payloadSize < 8) {
            return false;
        }
        // mdhd version 1 widens creation/modification times to 64 bit.
        const size_t timescaleOffset = mdhd->payload[0] == 1 ? 20 : 12;
        if (mdhd->payloadSize < timescaleOffset + 4) {
            return false;
        }
        timeline.timescale = readBigEndian32(mdhd->payload + timescaleOffset);
        if (timeline.timescale == 0) {
            return false;
        }

        const uint32_t durationsCount = readBigEndian32(stts->payload + 4);
        if (stts->payloadSize < 8 + static_cast<size_t>(durationsCount) * 8) {
            return false;
        }
        std::vector<std::pair<uint32_t, uint32_t>> durations;
        for (uint32_t index = 0; index < durationsCount; ++index) {
            durations.emplace_back(
                readBigEndian32(stts->payload + 8 + index * 8),
                readBigEndian32(stts->payload + 12 + index * 8));
        }

        std::vector<std::pair<uint32_t, int32_t>> offsets;
        if (const auto ctts = findBox(*stbl, fourcc("ctts"))) {
            if (ctts->payloadSize < 8) {
                return false;
            }
            const uint32_t offsetsCount = readBigEndian32(ctts->payload + 4);
            if (ctts->payloadSize < 8 + static_cast<size_t>(offsetsCount) * 8) {
                return false;
            }
            for (uint32_t index = 0; index < offsetsCount; ++index) {
                offsets.emplace_back(
                    readBigEndian32(ctts->payload + 8 + index * 8),
                    static_cast<int32_t>(readBigEndian32(ctts->payload + 12 + index * 8)));
            }
        }

        int64_t dts = 0;
        size_t offsetIndex = 0;
        uint32_t offsetRemaining = 0;
        int32_t compositionOffset = 0;
        for (const auto& [count, delta] : durations) {
            for (uint32_t sample = 0; sample < count; ++sample) {
                while (offsetRemaining == 0 && offsetIndex < offsets.size()) {
                    offsetRemaining = offsets[offsetIndex].first;
                    compositionOffset = offsets[offsetIndex].second;
                    ++offsetIndex;
                }
                int32_t useOffset = 0;
                if (offsetRemaining > 0) {
                    useOffset = compositionOffset;
                    --offsetRemaining;
                }
                timeline.dts100ns.push_back(dts);
                timeline.compositionOffsets.push_back(useOffset);
                timeline.pts100ns.push_back(dts + useOffset);
                dts += delta;
            }
        }
        return !timeline.dts100ns.empty();
    }
}

void expectTimeline(
    const Mp4SampleTimeline& timeline,
    const std::vector<int64_t>& inputPts100ns,
    const std::string& label) {
    expect(timeline.timescale > 0, label + " has no video timescale");
    expect(timeline.dts100ns.size() == inputPts100ns.size(),
        label + " sample count does not match the written packets (got " +
            std::to_string(timeline.dts100ns.size()) + " of " +
            std::to_string(inputPts100ns.size()) + ")");
    const int64_t tick100ns = static_cast<int64_t>(10000000ull / timeline.timescale) + 1;
    int64_t previousDts = -1;
    for (size_t index = 0; index < timeline.dts100ns.size(); ++index) {
        expect(timeline.dts100ns[index] > previousDts,
            label + " decode timestamps are not strictly increasing");
        previousDts = timeline.dts100ns[index];
        expect(timeline.pts100ns[index] >= 0, label + " presentation timestamp is negative");
        expect(timeline.compositionOffsets[index] >= 0, label + " composition offset is negative");
        const int64_t expectedDelta = inputPts100ns[index] - inputPts100ns.front();
        const int64_t actualDelta =
            (timeline.pts100ns[index] - timeline.pts100ns.front()) * 10000000ll / timeline.timescale;
        const int64_t difference = actualDelta - expectedDelta;
        expect(difference <= tick100ns && difference >= -tick100ns,
            label + " presentation timeline does not match the written packet order");
    }
}

struct Mp4Probe {
    bool opened = false;
    bool hasVideo = false;
    bool hasAudio = false;
    GUID videoSubtype{};
    UINT32 width = 0;
    UINT32 height = 0;
    UINT64 duration100ns = 0;
};

Mp4Probe probeMp4(const std::filesystem::path& path) {
    Mp4Probe probe;
    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(path.wstring().c_str(), nullptr, &reader))) {
        return probe;
    }
    probe.opened = true;
    PROPVARIANT duration;
    PropVariantInit(&duration);
    if (SUCCEEDED(reader->GetPresentationAttribute(
            MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration))) {
        probe.duration100ns = duration.uhVal.QuadPart;
    }
    PropVariantClear(&duration);
    Microsoft::WRL::ComPtr<IMFMediaType> videoType;
    if (SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &videoType))) {
        probe.hasVideo = true;
        videoType->GetGUID(MF_MT_SUBTYPE, &probe.videoSubtype);
        MFGetAttributeSize(videoType.Get(), MF_MT_FRAME_SIZE, &probe.width, &probe.height);
    }
    Microsoft::WRL::ComPtr<IMFMediaType> audioType;
    probe.hasAudio = SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &audioType));
    return probe;
}

// Uses the OS H.264 encoder MFT to get an elementary stream that is actually
// decodable, not just container-shaped. Returns nullopt with a reason when the
// machine has no usable encoder, so the caller can skip instead of failing.
std::optional<std::vector<backtrack::EncodedPacket>> encodeH264Frames(
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    uint32_t frameCount,
    std::string& skipReason) {
    IMFActivate** activates = nullptr;
    UINT32 activateCount = 0;
    const HRESULT enumResult = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr,
        nullptr,
        &activates,
        &activateCount);
    if (FAILED(enumResult) || activateCount == 0) {
        skipReason = "OS H.264 encoder MFT absent (" + hrText(enumResult) + ")";
        CoTaskMemFree(activates);
        return std::nullopt;
    }
    Microsoft::WRL::ComPtr<IMFTransform> encoder;
    HRESULT activationResult = E_FAIL;
    for (UINT32 index = 0; index < activateCount; ++index) {
        GUID clsid{};
        if (SUCCEEDED(activates[index]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid)) &&
            clsid == CLSID_MSH264EncoderMFT) {
            activationResult = activates[index]->ActivateObject(IID_PPV_ARGS(encoder.GetAddressOf()));
        }
        activates[index]->Release();
    }
    CoTaskMemFree(activates);
    if (!encoder) {
        skipReason = "the Microsoft H.264 encoder MFT could not be activated (" +
            hrText(activationResult) + ")";
        return std::nullopt;
    }

    Microsoft::WRL::ComPtr<IMFAttributes> transformAttributes;
    UINT32 async = FALSE;
    if (SUCCEEDED(encoder->GetAttributes(&transformAttributes)) && transformAttributes) {
        transformAttributes->GetUINT32(MF_TRANSFORM_ASYNC, &async);
        if (async) {
            transformAttributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        }
    }

    Microsoft::WRL::ComPtr<IMFMediaType> outputType;
    if (FAILED(MFCreateMediaType(&outputType))) {
        skipReason = "could not create the encoder output type";
        return std::nullopt;
    }
    outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outputType->SetUINT32(MF_MT_AVG_BITRATE, 1000000);
    outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    HRESULT hr = encoder->SetOutputType(0, outputType.Get(), 0);
    if (FAILED(hr)) {
        skipReason = "the encoder rejected the H.264 output type (" + hrText(hr) + ")";
        return std::nullopt;
    }

    Microsoft::WRL::ComPtr<IMFMediaType> inputType;
    for (DWORD index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IMFMediaType> candidate;
        if (FAILED(encoder->GetInputAvailableType(0, index, &candidate))) {
            break;
        }
        GUID subtype{};
        if (SUCCEEDED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFVideoFormat_NV12) {
            inputType = candidate;
            break;
        }
    }
    if (!inputType) {
        skipReason = "the encoder exposes no NV12 input type";
        return std::nullopt;
    }
    MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    inputType->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
    hr = encoder->SetInputType(0, inputType.Get(), 0);
    if (FAILED(hr)) {
        skipReason = "the encoder rejected the NV12 input type (" + hrText(hr) + ")";
        return std::nullopt;
    }

    MFT_OUTPUT_STREAM_INFO streamInfo{};
    if (FAILED(encoder->GetOutputStreamInfo(0, &streamInfo))) {
        skipReason = "the encoder exposes no output stream info";
        return std::nullopt;
    }
    const DWORD outputBufferSize = streamInfo.cbSize > 0 ? streamInfo.cbSize : 1u << 20;
    std::vector<backtrack::EncodedPacket> packets;
    auto harvest = [&]() -> bool {
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER output{};
            if ((streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
                Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
                Microsoft::WRL::ComPtr<IMFSample> sample;
                if (FAILED(MFCreateMemoryBuffer(outputBufferSize, &buffer)) ||
                    FAILED(MFCreateSample(&sample)) ||
                    FAILED(sample->AddBuffer(buffer.Get()))) {
                    skipReason = "could not allocate the encoder output sample";
                    return false;
                }
                output.pSample = sample.Detach();
            }
            DWORD status = 0;
            const HRESULT outputResult = encoder->ProcessOutput(0, 1, &output, &status);
            if (outputResult == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                if (output.pSample) {
                    output.pSample->Release();
                }
                return true;
            }
            if (FAILED(outputResult)) {
                if (output.pSample) {
                    output.pSample->Release();
                }
                skipReason = "encoder ProcessOutput failed (" + hrText(outputResult) + ")";
                return false;
            }
            Microsoft::WRL::ComPtr<IMFSample> produced;
            produced.Attach(output.pSample);
            Microsoft::WRL::ComPtr<IMFMediaBuffer> contiguous;
            BYTE* data = nullptr;
            DWORD length = 0;
            if (FAILED(produced->ConvertToContiguousBuffer(&contiguous)) ||
                FAILED(contiguous->Lock(&data, nullptr, &length))) {
                skipReason = "the encoder output buffer was unreadable";
                return false;
            }
            backtrack::EncodedPacket packet;
            packet.bytes.assign(data, data + length);
            contiguous->Unlock();
            LONGLONG timestamp = 0;
            LONGLONG duration = 0;
            produced->GetSampleTime(&timestamp);
            produced->GetSampleDuration(&duration);
            packet.pts100ns = timestamp;
            packet.duration100ns = duration > 0 ? duration : 10000000ll / fps;
            packet.codec = backtrack::VideoCodec::H264;
            packet.keyFrame = packets.empty();
            packets.push_back(std::move(packet));
        }
    };

    const int64_t frameDuration100ns = 10000000ll / fps;
    const size_t yPlane = static_cast<size_t>(width) * height;
    std::vector<uint8_t> frame(yPlane + yPlane / 2, 128);
    encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    for (uint32_t index = 0; index < frameCount; ++index) {
        std::fill(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(yPlane),
            static_cast<uint8_t>(16 + index * 24));
        Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
        Microsoft::WRL::ComPtr<IMFSample> sample;
        if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(frame.size()), &buffer)) ||
            FAILED(MFCreateSample(&sample)) ||
            FAILED(sample->AddBuffer(buffer.Get()))) {
            skipReason = "could not allocate the encoder input sample";
            return std::nullopt;
        }
        BYTE* destination = nullptr;
        DWORD maxLength = 0;
        DWORD currentLength = 0;
        if (FAILED(buffer->Lock(&destination, &maxLength, &currentLength))) {
            skipReason = "the encoder input buffer was unreadable";
            return std::nullopt;
        }
        std::memcpy(destination, frame.data(), frame.size());
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(frame.size()));
        sample->SetSampleTime(static_cast<LONGLONG>(index) * frameDuration100ns);
        sample->SetSampleDuration(frameDuration100ns);
        const HRESULT inputResult = encoder->ProcessInput(0, sample.Get(), 0);
        if (FAILED(inputResult)) {
            skipReason = "the encoder rejected an NV12 frame (" + hrText(inputResult) + ")";
            return std::nullopt;
        }
        if (!harvest()) {
            return std::nullopt;
        }
    }
    encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (!harvest()) {
        return std::nullopt;
    }
    if (packets.empty()) {
        skipReason = "the encoder produced no packets";
        return std::nullopt;
    }

    // Output is in decode order, so a B-frame stream has regressing PTS. A
    // decode timestamp at or before the PTS keeps the muxer's contract valid;
    // the smallest shift that satisfies every frame preserves presentation
    // deltas without needing the encoder's reorder depth.
    int64_t maxShift = 0;
    for (size_t index = 0; index < packets.size(); ++index) {
        const int64_t shift = static_cast<int64_t>(index) - packets[index].pts100ns / frameDuration100ns;
        maxShift = std::max(maxShift, shift);
    }
    for (size_t index = 0; index < packets.size(); ++index) {
        packets[index].dts100ns = (static_cast<int64_t>(index) - maxShift) * frameDuration100ns;
        packets[index].hasDts = maxShift > 0;
    }
    return packets;
}

bool h264DecoderAvailable() {
    const MFT_REGISTER_TYPE_INFO inputType{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activates = nullptr;
    UINT32 activateCount = 0;
    const HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_DECODER,
        MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER,
        &inputType,
        nullptr,
        &activates,
        &activateCount);
    for (UINT32 index = 0; index < activateCount; ++index) {
        activates[index]->Release();
    }
    CoTaskMemFree(activates);
    return SUCCEEDED(hr) && activateCount > 0;
}

struct DecodedVideo {
    bool usable = false;
    uint32_t frames = 0;
    std::vector<int64_t> timestamps;
};

DecodedVideo decodeVideoFrames(const std::filesystem::path& path, uint32_t wantedFrames) {
    DecodedVideo decoded;
    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(path.wstring().c_str(), nullptr, &reader))) {
        return decoded;
    }
    reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    Microsoft::WRL::ComPtr<IMFMediaType> outputType;
    if (FAILED(MFCreateMediaType(&outputType))) {
        return decoded;
    }
    outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (FAILED(reader->SetCurrentMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, outputType.Get()))) {
        return decoded;
    }
    decoded.usable = true;
    for (int attempt = 0; attempt < 200 && decoded.frames < wantedFrames; ++attempt) {
        DWORD actualStream = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        Microsoft::WRL::ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &actualStream, &flags, &timestamp, &sample))) {
            return decoded;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            break;
        }
        if (!sample) {
            continue;
        }
        Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
        DWORD length = 0;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) ||
            FAILED(buffer->GetCurrentLength(&length)) || length == 0) {
            return decoded;
        }
        decoded.timestamps.push_back(timestamp);
        ++decoded.frames;
    }
    return decoded;
}

void testSuccessfulMuxRoundTrip() {
    const auto directory = std::filesystem::temp_directory_path() / L"backtrack_mux_round_trip_test";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    expect(!error, "could not create the mux round-trip fixture");
    const auto output = directory / L"clip.mp4";

    backtrack::VideoSettings settings;
    settings.codec = backtrack::VideoCodec::H264;
    settings.width = 320;
    settings.height = 240;
    settings.fps = 30;
    settings.bitrateKbps = 1000;

    backtrack::Mp4Muxer muxer;
    expect(muxer.startRecording(output, settings), "could not start the round-trip recording");
    std::vector<int64_t> inputPts100ns;
    constexpr int64_t kFrameDuration100ns = 333333;
    constexpr uint32_t kVideoPackets = 30;
    for (uint32_t index = 0; index < kVideoPackets; ++index) {
        const int64_t pts = static_cast<int64_t>(index) * kFrameDuration100ns;
        inputPts100ns.push_back(pts);
        muxer.writeVideoPacket(
            makeVideoPacket(static_cast<int>(index), pts, pts, false, kFrameDuration100ns, index == 0));
    }
    // 1 s of 48 kHz stereo PCM in 20 ms packets, so the audio track spans the video timeline.
    for (uint32_t index = 0; index < 50; ++index) {
        muxer.writeAudioPacket(makeAudioPacket(960, static_cast<int64_t>(index) * 200000,
            static_cast<uint8_t>(index)));
    }

    const auto published = muxer.finalize();
    expect(published == output, "a valid recording did not publish an MP4: " + narrow(muxer.lastError()));
    expect(std::filesystem::exists(output), "the published MP4 does not exist");
    expect(std::filesystem::file_size(output) > 1024, "the published MP4 is too small to hold its streams");

    MfRuntime runtime;
    expect(runtime.usable(), "Media Foundation is unavailable for the read-back");
    const auto probe = probeMp4(output);
    expect(probe.opened, "the published MP4 could not be opened by Media Foundation");
    expect(probe.hasVideo, "the published MP4 has no video stream");
    expect(probe.videoSubtype == MFVideoFormat_H264, "the published MP4 video is not H.264");
    expect(probe.width == settings.width && probe.height == settings.height,
        "the published MP4 video size does not match the recording settings");
    expect(probe.hasAudio, "the published MP4 lost the muxed audio track");
    constexpr uint64_t kExpectedDuration100ns = 10000000ull;
    constexpr uint64_t kDurationTolerance100ns = 500000ull;
    const uint64_t durationDelta = probe.duration100ns > kExpectedDuration100ns
        ? probe.duration100ns - kExpectedDuration100ns
        : kExpectedDuration100ns - probe.duration100ns;
    expect(durationDelta <= kDurationTolerance100ns,
        "the published MP4 duration " + std::to_string(probe.duration100ns) +
            " does not match the input timeline");

    Mp4SampleTimeline timeline;
    expect(readVideoTimeline(output, timeline), "could not read the MP4 video sample tables");
    expectTimeline(timeline, inputPts100ns, "muxed recording");

    std::filesystem::remove_all(directory, error);
    expect(!error, "could not remove the mux round-trip fixture");
}

// Encoded packets arrive in decode order with regressing PTS when B-frames
// reorder. Without a decode timestamp on every sample the sink cannot build stts
// and drops the regressing frames, so the stored PTS sequence must stay exactly
// the one that was written.
void testBFrameMuxKeepsDecodeOrder() {
    const auto directory = std::filesystem::temp_directory_path() / L"backtrack_mux_b_frame_test";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    expect(!error, "could not create the B-frame fixture");
    const auto output = directory / L"clip.mp4";

    backtrack::VideoSettings settings;
    settings.codec = backtrack::VideoCodec::H264;
    settings.width = 320;
    settings.height = 240;
    settings.fps = 30;
    settings.bitrateKbps = 1000;

    constexpr int64_t kFrameDuration100ns = 333333;
    struct Timestamp {
        int64_t pts100ns;
        int64_t dts100ns;
        bool keyFrame;
    };
    std::vector<Timestamp> timestamps;
    for (int gop = 0; gop < 2; ++gop) {
        const int64_t pts = static_cast<int64_t>(gop) * 4 * kFrameDuration100ns;
        const int64_t dts = pts - 2 * kFrameDuration100ns;
        timestamps.push_back({pts, dts, true});
        timestamps.push_back({pts + 3 * kFrameDuration100ns, dts + kFrameDuration100ns, false});
        timestamps.push_back({pts + kFrameDuration100ns, dts + 2 * kFrameDuration100ns, false});
        timestamps.push_back({pts + 2 * kFrameDuration100ns, dts + 3 * kFrameDuration100ns, false});
    }

    backtrack::Mp4Muxer muxer;
    expect(muxer.startRecording(output, settings), "could not start the B-frame recording");
    std::vector<int64_t> inputPts100ns;
    for (size_t index = 0; index < timestamps.size(); ++index) {
        inputPts100ns.push_back(timestamps[index].pts100ns);
        muxer.writeVideoPacket(makeVideoPacket(static_cast<int>(index), timestamps[index].pts100ns,
            timestamps[index].dts100ns, true, kFrameDuration100ns, timestamps[index].keyFrame));
    }
    expect(muxer.finalize() == output,
        "the muxer rejected a decode-ordered B-frame stream: " + narrow(muxer.lastError()));

    MfRuntime runtime;
    expect(runtime.usable(), "Media Foundation is unavailable for the read-back");
    const auto probe = probeMp4(output);
    expect(probe.opened && probe.hasVideo, "the B-frame MP4 did not open with a video stream");

    Mp4SampleTimeline timeline;
    expect(readVideoTimeline(output, timeline), "could not read the B-frame MP4 sample tables");
    expectTimeline(timeline, inputPts100ns, "B-frame recording");

    std::filesystem::remove_all(directory, error);
    expect(!error, "could not remove the B-frame fixture");
}

// Stretch leg: encode real frames with the OS H.264 encoder, mux them through
// the production path, and decode at least one frame back. Skipped, not failed,
// on a machine without a usable encoder or decoder MFT.
void testOsH264StreamDecodes() {
    MfRuntime runtime;
    if (!runtime.usable()) {
        std::cout << "skip: Media Foundation unavailable; decodable-stream leg skipped\n";
        return;
    }
    constexpr uint32_t kWidth = 64;
    constexpr uint32_t kHeight = 64;
    constexpr uint32_t kFps = 30;
    constexpr uint32_t kFrames = 6;
    std::string skipReason;
    const auto packets = encodeH264Frames(kWidth, kHeight, kFps, kFrames, skipReason);
    if (!packets) {
        std::cout << "skip: " << skipReason << "; decodable-stream leg skipped\n";
        return;
    }
    if (!h264DecoderAvailable()) {
        std::cout << "skip: OS H.264 decoder MFT absent; decodable-stream leg skipped\n";
        return;
    }

    const auto directory = std::filesystem::temp_directory_path() / L"backtrack_mux_decodable_stream_test";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    expect(!error, "could not create the decodable-stream fixture");
    const auto output = directory / L"clip.mp4";

    backtrack::VideoSettings settings;
    settings.codec = backtrack::VideoCodec::H264;
    settings.width = kWidth;
    settings.height = kHeight;
    settings.fps = kFps;
    settings.bitrateKbps = 500;
    backtrack::Mp4Muxer muxer;
    expect(muxer.startRecording(output, settings), "could not start the decodable-stream recording");
    for (const auto& packet : *packets) {
        muxer.writeVideoPacket(packet);
    }
    expect(muxer.finalize() == output,
        "the muxer rejected a real H.264 elementary stream: " + narrow(muxer.lastError()));

    const auto decoded = decodeVideoFrames(output, 1);
    expect(decoded.usable, "Media Foundation could not configure the H.264 decoder for the muxed MP4");
    expect(decoded.frames > 0, "the muxed MP4 produced no decodable video frame");
    int64_t previousTimestamp = -1;
    for (const auto timestamp : decoded.timestamps) {
        expect(timestamp >= 0, "a decoded frame has a negative timestamp");
        expect(timestamp >= previousTimestamp, "decoded frame timestamps regress");
        previousTimestamp = timestamp;
    }

    std::filesystem::remove_all(directory, error);
    expect(!error, "could not remove the decodable-stream fixture");
}

// A finalization failure while finalize() is closing the recording's WAV track
// must fail the save and keep the session recoverable, not publish video with a
// truncated audio track. The switch is process-wide, so the guard resets it
// before any expectation can abort.
struct FailWavFinalizeGuard {
    FailWavFinalizeGuard() { backtrack::WavWriter::failFinalizeForTesting(true); }
    ~FailWavFinalizeGuard() { backtrack::WavWriter::failFinalizeForTesting(false); }
    FailWavFinalizeGuard(const FailWavFinalizeGuard&) = delete;
    FailWavFinalizeGuard& operator=(const FailWavFinalizeGuard&) = delete;
};

void testFinalizeFailurePreservesRecordingAudio() {
    const auto directory = std::filesystem::temp_directory_path() / L"backtrack_mux_wav_finalize_failure_test";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    expect(!error, "could not create the finalize-failure fixture");
    const auto output = directory / L"clip.mp4";
    const HANDLE reservation = CreateFileW(
        output.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr);
    expect(reservation != INVALID_HANDLE_VALUE, "could not reserve the finalize-failure output");
    CloseHandle(reservation);

    backtrack::Mp4Muxer muxer;
    backtrack::VideoSettings settings;
    settings.codec = backtrack::VideoCodec::H264;
    settings.width = 320;
    settings.height = 240;
    settings.fps = 30;
    expect(muxer.startRecording(output, settings), "could not start the finalize-failure recording");
    const auto videoPacket = makeVideoPacket(0, 0, 0, false, 333333, true);
    muxer.writeVideoPacket(videoPacket);
    muxer.writeAudioPacket(makeAudioPacket(960, 0, 0x10));

    const auto session = directory / L".backtrack_tmp" / L"clip";
    const auto videoPath = session / L"video.h264";
    const auto audioPath = session / L"system.wav";
    const auto manifest = session / backtrack::RecoveryStore::kRecoveryManifestName;
    expect(std::filesystem::exists(audioPath), "the recording audio track was not opened");

    {
        FailWavFinalizeGuard guard;
        expect(muxer.finalize().empty(), "a WAV finalization failure published a recording");
    }

    expect(!std::filesystem::exists(output), "failed finalize kept its output reservation");
    expect(std::filesystem::exists(audioPath), "failed finalize deleted the recording audio track");
    expect(std::filesystem::file_size(audioPath) > 44, "failed finalize truncated the recording audio track");
    const std::string expectedVideo(videoPacket.bytes.begin(), videoPacket.bytes.end());
    expect(readFile(videoPath) == expectedVideo, "failed finalize changed the recorded video prefix");
    expect(std::filesystem::exists(manifest), "failed finalize deleted the recovery manifest");
    expect(readFile(manifest).find("sample\t") != std::string::npos,
        "failed finalize deleted the incremental samples");
    expect(muxer.lastError().find(L"Recover Failed") != std::wstring::npos,
        "the recoverable session was not reported in the error");
    expect(muxer.lastError().find(L"Preserved 1 video packets") != std::wstring::npos,
        "the error did not name the preserved packets");

    std::filesystem::remove_all(directory, error);
    expect(!error, "could not remove the finalize-failure fixture");
}

} // namespace

int main() {
    try {
        testWriteFailurePreservesIncrementalRecording();
        testSuccessfulMuxRoundTrip();
        testBFrameMuxKeepsDecodeOrder();
        testOsH264StreamDecodes();
        testFinalizeFailurePreservesRecordingAudio();
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
