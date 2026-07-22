#pragma once

#include "mux/Mp4Muxer.h"

#include <cstdint>
#include <filesystem>

namespace backtrack {

inline constexpr uint32_t kMuxAudioSampleRate = 48000;
inline constexpr uint16_t kMuxAudioChannels = 2;
inline constexpr uint16_t kMuxAudioBitsPerSample = 16;
inline constexpr uint32_t kMuxAudioAverageBytesPerSecond = 24000;

bool buildMixedPcmAudio(
    const MuxedInputs& inputs,
    const std::filesystem::path& outputPath,
    std::filesystem::path& mixedPath);

} // namespace backtrack
