#pragma once

#include "core/Types.h"

#include <filesystem>
#include <fstream>

namespace backtrack {

class WavWriter {
public:
    WavWriter() = default;
    ~WavWriter();

    bool open(const std::filesystem::path& path, const WaveFormatBlob& format);
    void write(const uint8_t* data, size_t size);
    void writeSilenceFrames(uint64_t frames);
    void close();
    void reset();
    bool isOpen() const { return stream_.is_open(); }
    uint64_t dataBytes() const { return dataBytes_; }
    const std::filesystem::path& path() const { return path_; }
    uint16_t blockAlign() const { return blockAlign_; }
    uint32_t samplesPerSec() const { return samplesPerSec_; }

private:
    void writeHeader(const WaveFormatBlob& format);
    void patchHeader();

    std::filesystem::path path_;
    std::ofstream stream_;
    std::streampos dataSizePosition_{};
    uint64_t dataBytes_ = 0;
    uint64_t maxDataBytes_ = 0;
    uint16_t blockAlign_ = 0;
    uint32_t samplesPerSec_ = 0;
    bool sizeLimitLogged_ = false;
};

} // namespace backtrack
