#include "mux/WavWriter.h"

#include "core/Logger.h"

#include <mmreg.h>

#include <array>
#include <algorithm>
#include <limits>
#include <vector>

namespace backtrack {

namespace {

void writeTag(std::ofstream& stream, const char tag[4]) {
    stream.write(tag, 4);
}

void writeU16(std::ofstream& stream, uint16_t value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void writeU32(std::ofstream& stream, uint32_t value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

WaveFormatBlob fallbackPcmFormat() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    format.cbSize = 0;

    WaveFormatBlob blob;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&format);
    blob.bytes.assign(bytes, bytes + sizeof(WAVEFORMATEX));
    return blob;
}

} // namespace

WavWriter::~WavWriter() {
    close();
}

bool WavWriter::open(const std::filesystem::path& path, const WaveFormatBlob& format) {
    close();
    path_ = path;
    dataBytes_ = 0;
    maxDataBytes_ = 0;
    dataSizePosition_ = {};
    blockAlign_ = 0;
    samplesPerSec_ = 0;
    format_ = WaveFormatBlob{};
    riffBoundaryLogged_ = false;
    const WaveFormatBlob effectiveFormat = format.bytes.empty() ? fallbackPcmFormat() : format;
    if (effectiveFormat.bytes.size() < sizeof(WAVEFORMATEX)) {
        Logger::instance().warning(L"mux", L"Rejected WAV stream with invalid format metadata: " + path_.wstring());
        return false;
    }
    const auto* waveFormat = reinterpret_cast<const WAVEFORMATEX*>(effectiveFormat.bytes.data());
    blockAlign_ = waveFormat->nBlockAlign;
    samplesPerSec_ = waveFormat->nSamplesPerSec;
    if (blockAlign_ == 0 || samplesPerSec_ == 0) {
        Logger::instance().warning(L"mux", L"Rejected WAV stream with unusable format metadata: " + path_.wstring());
        return false;
    }
    format_ = effectiveFormat;
    if (!path_.parent_path().empty()) {
        std::error_code error;
        std::filesystem::create_directories(path_.parent_path(), error);
        if (error) {
            return false;
        }
    }
    stream_.open(path_, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!stream_.is_open()) {
        return false;
    }
    writeHeader(effectiveFormat);
    return true;
}

bool WavWriter::write(const uint8_t* data, size_t size) {
    if (!stream_.is_open()) {
        return false;
    }
    if (!data || size == 0) {
        return true;
    }
    if (size > static_cast<size_t>((std::numeric_limits<std::streamsize>::max)())) {
        return true;
    }
    if (maxDataBytes_ > 0 && !riffBoundaryLogged_ && dataBytes_ + size > maxDataBytes_) {
        // Large-file form: writes keep going past the 4 GiB RIFF boundary and
        // the header clamps to the uint32 maximum on close (data runs to EOF).
        // parseWav and the mix reader recover the extent from the real file
        // size, so long recordings are not truncated on purpose anymore.
        riffBoundaryLogged_ = true;
        Logger::instance().warning(L"mux",
            L"WAV stream crossed the 4 GiB RIFF boundary; audio continues with a clamped header: " + path_.wstring());
    }
    stream_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!stream_) {
        Logger::instance().error(L"mux", L"Could not write WAV audio data: " + path_.wstring());
        return false;
    }
    dataBytes_ += size;
    return true;
}

bool WavWriter::writeSilenceFrames(uint64_t frames) {
    if (!stream_.is_open() || blockAlign_ == 0) {
        return false;
    }
    if (frames == 0) {
        return true;
    }
    constexpr size_t kChunkBytes = 64 * 1024;
    const size_t alignedChunkBytes = std::max<size_t>(
        blockAlign_,
        (kChunkBytes / blockAlign_) * blockAlign_);
    std::vector<uint8_t> silence(alignedChunkBytes, 0);
    const uint64_t maxFrames = (std::numeric_limits<uint64_t>::max)() / blockAlign_;
    uint64_t bytesRemaining = std::min<uint64_t>(frames, maxFrames) * blockAlign_;
    while (bytesRemaining > 0) {
        uint64_t bytesThisChunk = std::min<uint64_t>(bytesRemaining, silence.size());
        bytesThisChunk -= bytesThisChunk % blockAlign_;
        if (bytesThisChunk == 0) {
            break;
        }
        const size_t bytesToWrite = static_cast<size_t>(bytesThisChunk);
        if (!write(silence.data(), bytesToWrite)) {
            return false;
        }
        bytesRemaining -= bytesToWrite;
    }
    return true;
}

void WavWriter::close() {
    if (!stream_.is_open()) {
        return;
    }
    patchHeader();
    stream_.close();
}

void WavWriter::reset() {
    close();
    path_.clear();
    format_ = WaveFormatBlob{};
    dataSizePosition_ = {};
    dataBytes_ = 0;
    maxDataBytes_ = 0;
    blockAlign_ = 0;
    samplesPerSec_ = 0;
    riffBoundaryLogged_ = false;
}

void WavWriter::writeHeader(const WaveFormatBlob& format) {
    writeTag(stream_, "RIFF");
    writeU32(stream_, 0);
    writeTag(stream_, "WAVE");
    writeTag(stream_, "fmt ");
    writeU32(stream_, static_cast<uint32_t>(format.bytes.size()));
    stream_.write(reinterpret_cast<const char*>(format.bytes.data()), static_cast<std::streamsize>(format.bytes.size()));
    writeTag(stream_, "data");
    dataSizePosition_ = stream_.tellp();
    writeU32(stream_, 0);
    dataBytes_ = 0;

    const auto dataStart = stream_.tellp();
    const uint64_t riffOverhead = static_cast<uint64_t>(dataStart) - 8;
    maxDataBytes_ = riffOverhead < (std::numeric_limits<uint32_t>::max)()
        ? static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)()) - riffOverhead
        : 0;
}

void WavWriter::patchHeader() {
    const auto end = stream_.tellp();
    const uint64_t riffSize64 = static_cast<uint64_t>(end) - 8;
    const uint32_t riffSize = static_cast<uint32_t>(std::min<uint64_t>(
        riffSize64,
        (std::numeric_limits<uint32_t>::max)()));
    const uint32_t dataSize = static_cast<uint32_t>(std::min<uint64_t>(
        dataBytes_,
        (std::numeric_limits<uint32_t>::max)()));

    stream_.seekp(4, std::ios::beg);
    writeU32(stream_, riffSize);
    stream_.seekp(dataSizePosition_, std::ios::beg);
    writeU32(stream_, dataSize);
    stream_.seekp(end, std::ios::beg);
}

} // namespace backtrack
