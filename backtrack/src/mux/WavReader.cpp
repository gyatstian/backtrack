#include "mux/WavReader.h"

#include <cstdint>
#include <fstream>
#include <limits>

namespace backtrack {
namespace {

// The clamped data size field is the marker for "data runs to EOF": WavWriter
// clamps both RIFF and data sizes to the uint32 maximum on close. A smaller
// declared data size means real trailing chunks, which are not audio.
bool extendClampedDataExtent(const std::filesystem::path& path, WavInfo& info) {
    std::error_code fileError;
    const uintmax_t fileBytesRaw = std::filesystem::file_size(path, fileError);
    if (fileError) {
        return false;
    }
    const uint64_t fileBytes = static_cast<uint64_t>(fileBytesRaw);
    if (info.dataOffset < 8 || info.dataOffset + info.dataBytes >= fileBytes) {
        return true; // Already covers everything the file holds.
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return false;
    }
    // The data chunk's size field sits 4 bytes before its payload.
    stream.seekg(static_cast<std::streamoff>(info.dataOffset - 4), std::ios::beg);
    uint32_t declaredDataSize = 0;
    stream.read(reinterpret_cast<char*>(&declaredDataSize), sizeof(declaredDataSize));
    if (!stream) {
        return false;
    }
    if (declaredDataSize == (std::numeric_limits<uint32_t>::max)()) {
        info.dataBytes = fileBytes - info.dataOffset;
    }
    return true;
}

} // namespace

bool parseMuxWav(const std::filesystem::path& path, WavInfo& info) {
    if (!parseWav(path, info)) {
        return false;
    }
    return extendClampedDataExtent(path, info);
}

} // namespace backtrack
