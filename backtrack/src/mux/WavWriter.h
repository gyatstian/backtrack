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
    // Returns true when the full payload was handed to the stream without
    // error. False means a write failure (typically disk full); the stream
    // stays open so close() can still finalize what was written, but callers
    // must stop feeding packets - every further write fails too. Writes past
    // the 4 GiB RIFF boundary keep going (clamped header, data runs to EOF)
    // instead of dropping audio.
    bool write(const uint8_t* data, size_t size);
    bool writeSilenceFrames(uint64_t frames);
    // Flushes, patches the RIFF/data sizes and closes. False means a header
    // step or the final flush failed, so the file's extent on disk is not what
    // the header declares; callers that publish audio must fail their save
    // instead. Destructor and reset() report nothing, they only log.
    bool close();
    void reset();
    bool isOpen() const { return stream_.is_open(); }
    uint64_t dataBytes() const { return dataBytes_; }
    const std::filesystem::path& path() const { return path_; }
    uint16_t blockAlign() const { return blockAlign_; }
    uint32_t samplesPerSec() const { return samplesPerSec_; }
    // Format blob the header was written with; empty before open().
    const WaveFormatBlob& format() const { return format_; }
    // Test-only: shrink the RIFF boundary so the large-file path is reachable
    // without writing 4 GiB. The app never calls it.
    void setMaxDataBytesForTesting(uint64_t maxDataBytes) { maxDataBytes_ = maxDataBytes; }
    // Test-only: make close() report a finalization failure. No path can
    // provoke a real flush/patch error deterministically, and this failure used
    // to be unreportable at all. Process-wide because callers such as
    // ReplayBuffer::saveTo own their writers. The app never calls it.
    static void failFinalizeForTesting(bool fail);

private:
    void writeHeader(const WaveFormatBlob& format);
    bool patchHeader();

    std::filesystem::path path_;
    std::ofstream stream_;
    WaveFormatBlob format_;
    std::streampos dataSizePosition_{};
    uint64_t dataBytes_ = 0;
    uint64_t maxDataBytes_ = 0;
    uint16_t blockAlign_ = 0;
    uint32_t samplesPerSec_ = 0;
    bool riffBoundaryLogged_ = false;
};

} // namespace backtrack
