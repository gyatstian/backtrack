#pragma once

#include "mux/Mp4Muxer.h"

#include <filesystem>
#include <string>

namespace backtrack {
namespace RecoveryStore {

bool writeManifest(const MuxedInputs& inputs, const std::filesystem::path& tempDirectory);
void writeReadme(const MuxedInputs& inputs, const std::filesystem::path& tempDirectory);
bool muxToMp4WithRetry(const MuxedInputs& inputs, const std::filesystem::path& outputPath);
std::wstring preservedRecordingDetail(
    const std::filesystem::path& outputPath,
    const std::filesystem::path& tempDirectory,
    const MuxedInputs& inputs);
std::filesystem::path recoverLatestFailedRecording(
    const std::filesystem::path& clipDirectory,
    std::wstring& detail);

} // namespace RecoveryStore
} // namespace backtrack
