#pragma once

#include "mux/MuxedInputs.h"

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

// Test-only: fail every source payload chunk read after the first one, which is
// the shape of a storage error found after the header parsed. No public path
// can provoke it (a short but valid file is salvaged on purpose). The app never
// calls it.
void setAudioMixReadFailureForTesting(bool fail);

} // namespace backtrack
