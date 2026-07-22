#include "audio/WavReader.h"

#include <mmreg.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <string_view>
#include <vector>

namespace backtrack {
namespace {

constexpr uint32_t kMaxWaveFormatBytes = 4096;

bool isWaveExtensibleSubFormat(const GUID& guid, uint16_t tag) {
    static constexpr std::array<uint8_t, 8> kWaveSubFormatTail = {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
    return guid.Data1 == tag &&
           guid.Data2 == 0 &&
           guid.Data3 == 0x0010 &&
           std::equal(std::begin(guid.Data4), std::end(guid.Data4), kWaveSubFormatTail.begin());
}

SampleEncoding encodingFromFormat(const std::vector<uint8_t>& formatBytes) {
    if (formatBytes.size() < sizeof(WAVEFORMATEX)) {
        return SampleEncoding::Unsupported;
    }

    const auto* format = reinterpret_cast<const WAVEFORMATEX*>(formatBytes.data());
    if (format->wFormatTag == WAVE_FORMAT_PCM) {
        return SampleEncoding::Pcm;
    }
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return SampleEncoding::Float;
    }
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && formatBytes.size() >= sizeof(WAVEFORMATEXTENSIBLE)) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(formatBytes.data());
        if (isWaveExtensibleSubFormat(extensible->SubFormat, WAVE_FORMAT_PCM)) {
            return SampleEncoding::Pcm;
        }
        if (isWaveExtensibleSubFormat(extensible->SubFormat, WAVE_FORMAT_IEEE_FLOAT)) {
            return SampleEncoding::Float;
        }
    }
    return SampleEncoding::Unsupported;
}

} // namespace

bool parseWav(const std::filesystem::path& path, WavInfo& info) {
    std::error_code fileError;
    const uintmax_t fileBytesRaw = std::filesystem::file_size(path, fileError);
    if (fileError ||
        fileBytesRaw < 12 ||
        fileBytesRaw > static_cast<uintmax_t>((std::numeric_limits<std::streamoff>::max)())) {
        return false;
    }
    const uint64_t fileBytes = static_cast<uint64_t>(fileBytesRaw);

    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return false;
    }

    char riff[4]{};
    uint32_t riffSize = 0;
    char wave[4]{};
    stream.read(riff, sizeof(riff));
    stream.read(reinterpret_cast<char*>(&riffSize), sizeof(riffSize));
    stream.read(wave, sizeof(wave));
    if (!stream ||
        std::string_view(riff, 4) != "RIFF" ||
        std::string_view(wave, 4) != "WAVE" ||
        riffSize < 4 ||
        static_cast<uint64_t>(riffSize) > fileBytes - 8) {
        return false;
    }

    const uint64_t riffEnd = 8ull + static_cast<uint64_t>(riffSize);
    bool foundFormat = false;
    bool foundData = false;
    while (stream && (!foundFormat || !foundData)) {
        const std::streamoff chunkHeaderOffset = static_cast<std::streamoff>(stream.tellg());
        if (chunkHeaderOffset < 0 || static_cast<uint64_t>(chunkHeaderOffset) + 8 > riffEnd) {
            break;
        }

        char id[4]{};
        uint32_t size = 0;
        stream.read(id, sizeof(id));
        stream.read(reinterpret_cast<char*>(&size), sizeof(size));
        if (!stream) {
            break;
        }

        const std::streamoff chunkDataOffset = static_cast<std::streamoff>(stream.tellg());
        if (chunkDataOffset < 0) {
            return false;
        }
        const uint64_t chunkData = static_cast<uint64_t>(chunkDataOffset);
        if (static_cast<uint64_t>(size) > riffEnd - chunkData) {
            return false;
        }
        const uint64_t nextChunk = chunkData + static_cast<uint64_t>(size) + (size & 1u);
        if (nextChunk > riffEnd + 1 || nextChunk > fileBytes) {
            return false;
        }

        if (std::string_view(id, 4) == "fmt ") {
            if (size < sizeof(WAVEFORMATEX) || size > kMaxWaveFormatBytes) {
                return false;
            }
            info.format.bytes.resize(size);
            stream.read(reinterpret_cast<char*>(info.format.bytes.data()), size);
            if (!stream) {
                return false;
            }
            const auto* format = reinterpret_cast<const WAVEFORMATEX*>(info.format.bytes.data());
            info.channels = format->nChannels;
            info.samplesPerSec = format->nSamplesPerSec;
            info.bitsPerSample = format->wBitsPerSample;
            info.blockAlign = format->nBlockAlign;
            info.encoding = encodingFromFormat(info.format.bytes);
            foundFormat = true;
        } else if (std::string_view(id, 4) == "data") {
            info.dataOffset = chunkData;
            info.dataBytes = size;
            foundData = true;
        }

        stream.seekg(static_cast<std::streamoff>(nextChunk), std::ios::beg);
    }

    const uint16_t bytesPerSample = info.bitsPerSample / 8;
    const uint32_t minimumBlockAlign = static_cast<uint32_t>(info.channels) * bytesPerSample;
    info.path = path;
    return foundFormat &&
           foundData &&
           info.channels > 0 &&
           info.samplesPerSec > 0 &&
           info.bitsPerSample > 0 &&
           info.bitsPerSample % 8 == 0 &&
           bytesPerSample > 0 &&
           info.blockAlign >= minimumBlockAlign &&
           info.encoding != SampleEncoding::Unsupported;
}

} // namespace backtrack
