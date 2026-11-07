#pragma once

#include "audio/WavReader.h"

#include <filesystem>

namespace backtrack {

// parseWav plus the large-file extent recovery the mux needs: a WAV written by
// WavWriter past the 4 GiB RIFF boundary declares the uint32 maximum as its
// data size (data runs to EOF), which parseWav caps at the boundary. Extending
// that clamped extent to the real file size is what keeps the muxed audio track
// complete instead of ending after ~6h12m of 16-bit stereo 48 kHz.
// False means the clamped header could not be re-read; the caller must fail
// rather than publish audio truncated at the boundary.
bool parseMuxWav(const std::filesystem::path& path, WavInfo& info);

} // namespace backtrack
