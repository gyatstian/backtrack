#pragma once

#include "mux/Mp4Muxer.h"

#include <chrono>
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

// Writes the complete manifest (header, sample count, all sample lines) used
// for post-failure recovery.
bool writeManifest(const MuxedInputs& inputs, const std::filesystem::path& tempDirectory);
// Writes everything except the "samples"/"sample" lines so an in-progress
// recording can append them incrementally and stay recoverable after a crash.
void writeManifestHeader(std::ostream& stream, const MuxedInputs& partialInputs);
// Single tab-separated "sample" line shared by full and incremental manifests.
std::string manifestSampleLine(const MuxedInputs::VideoSample& sample);
void writeReadme(const MuxedInputs& inputs, const std::filesystem::path& tempDirectory);
// Removes a recovery manifest left by a previous failed save before its
// elementary streams are truncated: a crash mid-attempt must never leave a
// manifest describing bytes the streams no longer contain. Only paths that
// truncate streams (the replay save) call this; the recording path only grows
// its streams and rewrites the manifest atomically, so deleting first would
// trade a complete incremental manifest for a crash window with none.
void clearStaleManifest(const std::filesystem::path& tempDirectory);
// Shared mux-publish tail for Mp4Muxer::finalize and ReplayBuffer::saveTo:
// writes manifest+readme, muxes with retry, then reports how the temp
// directory was left. Returns kPublished (caller deletes the temp directory),
// kPreservedForRecovery (manifest describes the streams on disk; caller keeps
// the directory), or kFailed (no manifest, so nothing is recoverable; caller
// deletes the directory). errorDetail carries the user-presentable reason on
// either failure.
enum class MuxPublishResult {
    kPublished,
    kPreservedForRecovery,
    kFailed,
};
MuxPublishResult publishMuxedInputs(
    const MuxedInputs& inputs,
    const std::filesystem::path& outputPath,
    const std::filesystem::path& tempDirectory,
    std::wstring* errorDetail = nullptr);
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
std::filesystem::path recoverLatestFailedRecording(
    const std::filesystem::path& clipDirectory,
    std::wstring& detail);
// Startup maintenance: first gives the newest valid recovery manifest a chance
// to be muxed into a clip (a crash mid-recording/mid-save still leaves all
// data on disk), then removes leftover crash-recovery data — temporary
// recording directories, stale staging files, and empty output-name
// reservations — older than maxAge, so crashed sessions cannot leak disk
// space forever. Younger leftovers stay available for manual recovery.
void sweepOrphanedRecoveryData(
    const std::filesystem::path& clipDirectory,
    std::chrono::hours maxAge = std::chrono::hours(24));

} // namespace RecoveryStore
} // namespace backtrack
