#pragma once

#include "mux/MuxedInputs.h"

#include <filesystem>
#include <ostream>
#include <string>

namespace backtrack {
namespace RecoveryStore {

// Shared so the muxer's incremental manifest and full recovery rewrite target
// the same file.
inline constexpr wchar_t kRecoveryManifestName[] = L"recovery.tsv";

// Forces a written file's cached bytes out to the medium before a rename
// publishes it. Shared with the muxer's staged MP4 publish; win32Error carries
// the Win32 reason so a sharing violation stays distinguishable from a full disk.
bool flushFileToDisk(const std::filesystem::path& path, DWORD& win32Error);

// creates and owns a new directory, adding a suffix when the preferred name exists.
std::filesystem::path createUniqueDirectory(
    const std::filesystem::path& preferredPath, std::error_code& error);

// Writes the complete manifest (header, sample count, all sample lines) used
// for post-failure recovery.
bool writeManifest(const MuxedInputs& inputs, const std::filesystem::path& tempDirectory);
// Writes everything except the "samples"/"sample" lines so an in-progress
// recording can append them incrementally and stay recoverable after a crash.
void writeManifestHeader(std::ostream& stream, const MuxedInputs& partialInputs);
// Single tab-separated "sample" line shared by full and incremental manifests.
std::string manifestSampleLine(const MuxedInputs::VideoSample& sample);
void writeReadme(const MuxedInputs& inputs, const std::filesystem::path& tempDirectory);
// Shared free-space preflight for both save paths (recording finalize via
// Mp4Muxer::muxToMp4, replay via ReplayBuffer::saveTo): fails fast with a
// readable reason instead of an opaque Media Foundation HRESULT or a truncated
// elementary stream. neededBytes is the caller's estimate (elementary streams
// plus slack below); the mux re-checks with real file sizes before writing.
inline constexpr uint64_t kSaveFreeSpaceSlackBytes = 256ull * 1024 * 1024;
bool ensureFreeDiskSpace(
    const std::filesystem::path& outputPath,
    uint64_t neededBytes,
    std::wstring& detail);
// Shared mux-publish tail for Mp4Muxer::finalize and ReplayBuffer::saveTo:
// writes manifest+readme, muxes with retry, then reports how the temp
// directory was left. Returns kPublished (caller deletes the temp directory),
// kPreserved (caller keeps the directory). errorDetail distinguishes
// automatic recovery from streams that may need manual recovery.
enum class MuxPublishResult {
    kPublished,
    kPreserved,
};
MuxPublishResult publishMuxedInputs(
    const MuxedInputs& inputs,
    const std::filesystem::path& outputPath,
    const std::filesystem::path& tempDirectory,
    std::wstring* errorDetail = nullptr);
// keeps existing timing data intact on write failure and creates a manifest
// only when none exists; the caller retains the directory in either case.
std::wstring preserveFailedRecording(
    const MuxedInputs& inputs,
    const std::filesystem::path& outputPath,
    const std::filesystem::path& tempDirectory,
    const std::wstring& reason);
bool muxToMp4WithRetry(
    const MuxedInputs& inputs,
    const std::filesystem::path& outputPath,
    std::wstring* errorDetail = nullptr);
// reason: specific cause ("MF Finalize failed 0x..."), may be empty.
std::wstring preservedRecordingDetail(
    const std::filesystem::path& outputPath,
    const std::filesystem::path& tempDirectory,
    const MuxedInputs& inputs,
    const std::wstring& reason = {});
bool hasRecoverableRecording(const std::filesystem::path& clipDirectory);
std::filesystem::path recoverLatestFailedRecording(
    const std::filesystem::path& clipDirectory,
    std::wstring& detail);
// startup maintenance: attempts each recoverable session once and preserves
// legacy sessions and ambiguous files, including old name reservations.
void sweepOrphanedRecoveryData(const std::filesystem::path& clipDirectory);

} // namespace RecoveryStore
} // namespace backtrack
