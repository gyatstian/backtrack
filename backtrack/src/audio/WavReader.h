#pragma once

#include "core/Types.h"

#include <cstdint>
#include <filesystem>

namespace backtrack {

enum class SampleEncoding {
    Pcm,
    Float,
    Unsupported,
};

struct WavInfo {
    std::filesystem::path path;
    WaveFormatBlob format;
    uint64_t dataOffset = 0;
    uint64_t dataBytes = 0;
    uint16_t channels = 0;
    uint32_t samplesPerSec = 0;
    uint16_t bitsPerSample = 0;
    uint16_t blockAlign = 0;
    SampleEncoding encoding = SampleEncoding::Unsupported;
};

bool parseWav(const std::filesystem::path& path, WavInfo& info);

} // namespace backtrack
