#include "mux/RecoveryStore.h"

#include "core/Logger.h"

#include <charconv>
#include <chrono>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace backtrack {
namespace RecoveryStore {
namespace {

constexpr wchar_t kRecoveryReadmeName[] = L"RECOVERY.txt";

struct RecoveryManifest {
    MuxedInputs inputs;
    std::filesystem::path tempDirectory;
};

std::string codecText(VideoCodec codec) {
    return codec == VideoCodec::Hevc ? "hevc" : "h264";
}

VideoCodec codecFromText(const std::string& text) {
    return text == "hevc" ? VideoCodec::Hevc : VideoCodec::H264;
}

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        const size_t tab = line.find('\t', start);
        if (tab == std::string::npos) {
            parts.push_back(line.substr(start));
            return parts;
        }
        parts.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
}

std::string narrowFileName(const std::filesystem::path& path) {
    return path.filename().string();
}

std::filesystem::path recoveryRoot(const std::filesystem::path& clipDirectory) {
    return clipDirectory / L".backtrack_tmp";
}

std::filesystem::path manifestPathFor(const std::filesystem::path& tempDirectory) {
    return tempDirectory / kRecoveryManifestName;
}

std::filesystem::path manifestTempPathFor(const std::filesystem::path& tempDirectory) {
    return tempDirectory / (std::wstring(kRecoveryManifestName) + L".tmp");
}

// Full-token numeric parsing. std::stoull happily converts the "123" prefix of
// a line torn mid-number into a plausible-but-wrong offset, which would make
// the muxer read wrong bytes; requiring full consumption rejects those.
bool parseUInt64Strict(const std::string& text, uint64_t& value) {
    const char* const first = text.data();
    const char* const last = first + text.size();
    const auto result = std::from_chars(first, last, value);
    return result.ec == std::errc() && result.ptr == last;
}

bool parseInt64Strict(const std::string& text, int64_t& value) {
    const char* const first = text.data();
    const char* const last = first + text.size();
    const auto result = std::from_chars(first, last, value);
    return result.ec == std::errc() && result.ptr == last;
}

bool parseUInt32Strict(const std::string& text, uint32_t& value) {
    uint64_t wide = 0;
    if (!parseUInt64Strict(text, wide) || wide > (std::numeric_limits<uint32_t>::max)()) {
        return false;
    }
    value = static_cast<uint32_t>(wide);
    return true;
}

// Bounded retry budget for the two publishes that can lose a race with
// antivirus or the search indexer: the staged MP4 and the recovery manifest.
// Both files are written and flushed to disk by then, so a sharing violation is
// transient and waiting out the scanner's handle is the whole fix. One quick
// retry was too thin against it: four attempts over roughly two seconds with
// growing backoff.
constexpr int kPublishAttempts = 4;
constexpr std::chrono::milliseconds kPublishBackoffMs[] = {
    std::chrono::milliseconds(300),
    std::chrono::milliseconds(600),
    std::chrono::milliseconds(1200)};

// Retrying the manifest publish is what keeps a scanner from costing the user a
// complete recording: saveTo has already verified every elementary-stream write
// by this point, so a lost race over recovery.tsv must not be reported as a
// write failure. Any other error is left alone - a full disk or a denied folder
// will not clear, and the caller needs to see it as a genuine write failure.
bool publishWithRetry(const std::filesystem::path& source, const std::filesystem::path& destination) {
    for (int attempt = 1;; ++attempt) {
        if (MoveFileExW(
                source.c_str(),
                destination.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        const DWORD error = GetLastError();
        // ERROR_ACCESS_DENIED is the one a held-open destination actually
        // produces: MoveFileExW reports 5 here, not ERROR_SHARING_VIOLATION, and
        // only with FILE_SHARE_DELETE absent on the holder. Retrying without it
        // meant this budget never engaged for the scanner race it exists for.
        // A genuinely denied folder also returns 5 and now costs the full
        // backoff before failing, which is the price of not special-casing it.
        if (attempt >= kPublishAttempts ||
            (error != ERROR_SHARING_VIOLATION &&
             error != ERROR_LOCK_VIOLATION &&
             error != ERROR_ACCESS_DENIED)) {
            return false;
        }
        std::this_thread::sleep_for(kPublishBackoffMs[attempt - 1]);
    }
}

std::filesystem::path uniqueRecoveredOutputPath(const std::filesystem::path& clipDirectory, const std::wstring& stem) {
    // Tries to claim candidate exclusively by creating an empty placeholder
    // (fail-if-exists), so nothing else can take the name while recovery muxes.
    // Falls back to existence probing when file creation is not possible.
    auto tryClaim = [&clipDirectory](const std::wstring& fileName) -> std::optional<std::filesystem::path> {
        const std::filesystem::path candidate = clipDirectory / fileName;
        const HANDLE handle = CreateFileW(
            candidate.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            return candidate;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            std::error_code probeError;
            if (!std::filesystem::exists(candidate, probeError) || probeError) {
                return candidate;
            }
        }
        return std::nullopt;
    };

    if (auto claimed = tryClaim(stem + L".mp4")) {
        return *claimed;
    }
    if (auto claimed = tryClaim(stem + L"_recovered.mp4")) {
        return *claimed;
    }
    for (uint32_t suffix = 2; suffix < 1000; ++suffix) {
        if (auto claimed = tryClaim(stem + L"_recovered_" + std::to_wstring(suffix) + L".mp4")) {
            return *claimed;
        }
    }
    const std::filesystem::path tickCandidate =
        clipDirectory / (stem + L"_recovered_" + std::to_wstring(GetTickCount64()) + L".mp4");
    return tryClaim(tickCandidate.filename().wstring()).value_or(tickCandidate);
}

// Parses one "sample" line's columns into sample. All numbers must be
// complete tokens; partial values fail and close the table instead of parsing
// into plausible-but-wrong offsets.
bool parseSampleColumns(const std::vector<std::string>& parts, MuxedInputs::VideoSample& sample) {
    if (parts.size() < 6) {
        return false;
    }
    uint32_t keyFrameFlag = 0;
    if (!(parseUInt64Strict(parts[1], sample.offset) &&
          parseUInt32Strict(parts[2], sample.size) &&
          parseInt64Strict(parts[3], sample.pts100ns) &&
          parseInt64Strict(parts[4], sample.duration100ns) &&
          parseUInt32Strict(parts[5], keyFrameFlag))) {
        return false;
    }
    sample.keyFrame = keyFrameFlag != 0;

    // Column added with B-frame support; older manifests fall back to "no
    // decode timestamp", which matches their streams.
    int64_t dts100ns = 0;
    if ((parts.size() >= 7 && !parseInt64Strict(parts[6], dts100ns))) {
        return false;
    }
    sample.dts100ns = dts100ns;
    // Legacy manifests carry no validity flag; their writers stored the
    // presentation time whenever a decode timestamp was invalid, so
    // inequality implies validity.
    sample.hasDts = parts.size() >= 7 && sample.dts100ns != sample.pts100ns;

    // Column added with the explicit DTS-validity flag; older manifests keep
    // the inferred interpretation above.
    uint32_t hasDtsFlag = 0;
    if (parts.size() >= 8) {
        if (!parseUInt32Strict(parts[7], hasDtsFlag)) {
            return false;
        }
        sample.hasDts = hasDtsFlag != 0;
    }
    return true;
}

bool readRecoveryManifest(const std::filesystem::path& tempDirectory, RecoveryManifest& manifest) {
    const std::filesystem::path manifestPath = manifestPathFor(tempDirectory);
    std::ifstream stream(manifestPath);
    if (!stream.is_open()) {
        return false;
    }

    std::string line;
    if (!std::getline(stream, line) || line != "backtrack-recovery-v1") {
        return false;
    }

    manifest = {};
    manifest.tempDirectory = tempDirectory;
    size_t expectedSamples = 0;
    // Samples tile the stream contiguously from offset 0 — exactly what both
    // writers produce. The first damaged or non-contiguous line therefore
    // marks a torn tail: keep the valid prefix, ignore later sample lines,
    // but keep scanning because audio-track registrations can follow samples.
    uint64_t nextSampleOffset = 0;
    bool sampleTableClosed = false;
    while (std::getline(stream, line)) {
        if (line.empty()) {
            continue;
        }

        const auto parts = splitTabs(line);
        if (parts.empty()) {
            continue;
        }

        if (parts[0] == "sample") {
            if (sampleTableClosed) {
                continue;
            }
            MuxedInputs::VideoSample sample;
            if (parseSampleColumns(parts, sample) && sample.offset == nextSampleOffset &&
                sample.offset <= (std::numeric_limits<uint64_t>::max)() - sample.size) {
                nextSampleOffset = sample.offset + sample.size;
                manifest.inputs.videoSamples.push_back(sample);
            } else {
                sampleTableClosed = true;
                Logger::instance().warning(L"mux",
                    L"Ignoring damaged recovery-manifest sample tail in " + manifestPath.wstring());
            }
            continue;
        }

        bool damagedMetadata = false;
        if (parts[0] == "codec" && parts.size() >= 2) {
            manifest.inputs.codec = codecFromText(parts[1]);
        } else if (parts[0] == "fps" && parts.size() >= 2) {
            damagedMetadata = !parseUInt32Strict(parts[1], manifest.inputs.fps);
        } else if (parts[0] == "width" && parts.size() >= 2) {
            damagedMetadata = !parseUInt32Strict(parts[1], manifest.inputs.width);
        } else if (parts[0] == "height" && parts.size() >= 2) {
            damagedMetadata = !parseUInt32Strict(parts[1], manifest.inputs.height);
        } else if (parts[0] == "bitrateKbps" && parts.size() >= 2) {
            damagedMetadata = !parseUInt32Strict(parts[1], manifest.inputs.bitrateKbps);
        } else if (parts[0] == "video" && parts.size() >= 2) {
            manifest.inputs.videoPath = tempDirectory / std::filesystem::path(parts[1]);
        } else if (parts[0] == "systemAudio" && parts.size() >= 2) {
            manifest.inputs.systemAudioPath = tempDirectory / std::filesystem::path(parts[1]);
        } else if (parts[0] == "microphoneAudio" && parts.size() >= 2) {
            manifest.inputs.microphoneAudioPath = tempDirectory / std::filesystem::path(parts[1]);
        } else if (parts[0] == "samples" && parts.size() >= 2) {
            uint64_t count = 0;
            if (parseUInt64Strict(parts[1], count)) {
                constexpr uint64_t kMaxReservedSamples = 10'000'000;
                expectedSamples = static_cast<size_t>(count);
                manifest.inputs.videoSamples.reserve(
                    static_cast<size_t>(std::min<uint64_t>(count, kMaxReservedSamples)));
            } else {
                damagedMetadata = true;
            }
        } else {
            continue; // Unknown keys are ignored, as in earlier versions.
        }

        // Metadata lines are written and flushed before recording starts, so
        // a damaged one means the whole file is untrustworthy.
        if (damagedMetadata) {
            return false;
        }
    }

    if (manifest.inputs.videoPath.empty()) {
        manifest.inputs.videoPath = tempDirectory / (manifest.inputs.codec == VideoCodec::H264 ? L"video.h264" : L"video.hevc");
    }
    // error_code overloads: this parser runs on the UI thread (manual
    // recovery) and during startup; a throwing std::filesystem call on an
    // unreadable file would terminate the process.
    std::error_code audioError;
    if (!manifest.inputs.systemAudioPath.empty() &&
        !std::filesystem::exists(manifest.inputs.systemAudioPath, audioError)) {
        manifest.inputs.systemAudioPath.clear();
    }
    std::error_code microphoneError;
    if (!manifest.inputs.microphoneAudioPath.empty() &&
        !std::filesystem::exists(manifest.inputs.microphoneAudioPath, microphoneError)) {
        manifest.inputs.microphoneAudioPath.clear();
    }

    std::error_code videoError;
    if (!std::filesystem::exists(manifest.inputs.videoPath, videoError) || videoError) {
        return false;
    }

    // Flush-desync guard: the incremental manifest is flushed at GOP
    // boundaries while the elementary stream's own write buffer can lag
    // behind, so after a crash the last recorded samples may reference bytes
    // that never reached disk. Trim that tail instead of failing an otherwise
    // fully recoverable session.
    std::error_code sizeError;
    const uint64_t streamBytes = std::filesystem::file_size(manifest.inputs.videoPath, sizeError);
    if (!sizeError) {
        auto& samples = manifest.inputs.videoSamples;
        while (!samples.empty()) {
            const MuxedInputs::VideoSample& last = samples.back();
            if (last.offset <= streamBytes &&
                static_cast<uint64_t>(last.size) <= streamBytes - last.offset) {
                break;
            }
            samples.pop_back();
        }
    }

    // A full rewrite interrupted mid-write could leave fewer lines than the
    // declared count even with atomic publish as last line of defense.
    return !manifest.inputs.videoSamples.empty() &&
           (expectedSamples == 0 || manifest.inputs.videoSamples.size() <= expectedSamples);
}

} // namespace

// Forces cached file data out to the medium so a rename published right after
// cannot point at bytes still sitting in volatile cache during a power loss.
// win32Error distinguishes a sharing violation (a scanner holds the file) from
// a full disk or a denied write; callers retry the first and surface the rest.
bool flushFileToDisk(const std::filesystem::path& path, DWORD& win32Error) {
    const HANDLE handle = CreateFileW(
        path.c_str(), FILE_WRITE_DATA, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        win32Error = GetLastError();
        return false;
    }
    const bool flushed = FlushFileBuffers(handle);
    if (!flushed) {
        win32Error = GetLastError();
    }
    CloseHandle(handle);
    return flushed;
}

void writeManifestHeader(std::ostream& stream, const MuxedInputs& partialInputs) {
    stream << "backtrack-recovery-v1\n";
    stream << "codec\t" << codecText(partialInputs.codec) << "\n";
    stream << "fps\t" << partialInputs.fps << "\n";
    stream << "width\t" << partialInputs.width << "\n";
    stream << "height\t" << partialInputs.height << "\n";
    stream << "bitrateKbps\t" << partialInputs.bitrateKbps << "\n";
    if (!partialInputs.videoPath.empty()) {
        stream << "video\t" << narrowFileName(partialInputs.videoPath) << "\n";
    }
    if (!partialInputs.systemAudioPath.empty()) {
        stream << "systemAudio\t" << narrowFileName(partialInputs.systemAudioPath) << "\n";
    }
    if (!partialInputs.microphoneAudioPath.empty()) {
        stream << "microphoneAudio\t" << narrowFileName(partialInputs.microphoneAudioPath) << "\n";
    }
}

std::string manifestSampleLine(const MuxedInputs::VideoSample& sample) {
    std::string line = "sample\t";
    line += std::to_string(sample.offset);
    line += '\t';
    line += std::to_string(sample.size);
    line += '\t';
    line += std::to_string(sample.pts100ns);
    line += '\t';
    line += std::to_string(sample.duration100ns);
    line += '\t';
    line += sample.keyFrame ? '1' : '0';
    line += '\t';
    line += std::to_string(sample.dts100ns);
    line += '\t';
    // Column added with the explicit DTS-validity flag; decode timestamps can
    // legitimately be zero or negative, so validity cannot be inferred.
    line += sample.hasDts ? '1' : '0';
    line += '\n';
    return line;
}

bool writeManifest(const MuxedInputs& inputs, const std::filesystem::path& tempDirectory) {
    // Publish atomically: stage the rewrite in a sibling temp file and move it
    // over recovery.tsv only once fully written and flushed. Truncating the
    // live manifest in place meant a crash or power loss mid-rewrite left a
    // header claiming N samples with fewer lines on disk — rejecting an
    // otherwise fully recoverable session.
    const std::filesystem::path manifestPath = manifestPathFor(tempDirectory);
    const std::filesystem::path tempPath = manifestTempPathFor(tempDirectory);
    {
        std::ofstream stream(tempPath, std::ios::out | std::ios::trunc);
        if (!stream.is_open()) {
            Logger::instance().warning(L"mux", L"Could not write recording recovery manifest: " + manifestPath.wstring());
            return false;
        }

        writeManifestHeader(stream, inputs);
        stream << "samples\t" << inputs.videoSamples.size() << "\n";
        for (const auto& sample : inputs.videoSamples) {
            stream << manifestSampleLine(sample);
        }
        stream.flush();
        if (!stream.good()) {
            Logger::instance().warning(L"mux", L"Could not write recording recovery manifest: " + manifestPath.wstring());
            stream.close();
            std::error_code removeError;
            std::filesystem::remove(tempPath, removeError);
            return false;
        }
    }

    DWORD flushError = ERROR_SUCCESS;
    if (!flushFileToDisk(tempPath, flushError)) {
        Logger::instance().warning(L"mux",
            L"Could not flush recording recovery manifest: " + tempPath.wstring() +
            L" (Win32 error " + std::to_wstring(flushError) + L")");
        std::error_code removeError;
        std::filesystem::remove(tempPath, removeError);
        return false;
    }
    if (!publishWithRetry(tempPath, manifestPath)) {
        Logger::instance().warning(L"mux",
            L"Could not publish recording recovery manifest: " + manifestPath.wstring() +
            L" (Win32 error " + std::to_wstring(GetLastError()) + L")");
        std::error_code removeError;
        std::filesystem::remove(tempPath, removeError);
        return false;
    }
    return true;
}

void writeReadme(const MuxedInputs& inputs, const std::filesystem::path& tempDirectory) {
    std::ofstream stream(tempDirectory / kRecoveryReadmeName, std::ios::out | std::ios::trunc);
    if (!stream.is_open()) {
        return;
    }

    stream << "Backtrack preserved this recording because native MP4 muxing failed.\n\n";
    stream << "Use the Recover Failed action in Backtrack to retry muxing this folder.\n";
    stream << "The elementary streams are also available for manual recovery:\n";
    stream << "- Video: " << narrowFileName(inputs.videoPath) << "\n";
    if (!inputs.systemAudioPath.empty()) {
        stream << "- System audio: " << narrowFileName(inputs.systemAudioPath) << "\n";
    }
    if (!inputs.microphoneAudioPath.empty()) {
        stream << "- Microphone audio: " << narrowFileName(inputs.microphoneAudioPath) << "\n";
    }
    stream << "\nThe recovery.tsv file contains packet timing used by Backtrack's native muxer.\n";
}

std::wstring preservedRecordingDetail(
    const std::filesystem::path& outputPath,
    const std::filesystem::path& tempDirectory,
    const MuxedInputs& inputs,
    const std::wstring& reason) {
    std::wstring detail;
    if (!reason.empty()) {
        detail = reason + L". ";
    }
    return detail +
           L"Preserved " + std::to_wstring(inputs.videoSamples.size()) +
           L" video packets and elementary streams in " + tempDirectory.wstring() +
           L". Use Recover Failed to retry, or recover manually from the preserved video/audio files.";
}

bool muxToMp4WithRetry(
    const MuxedInputs& inputs,
    const std::filesystem::path& outputPath,
    std::wstring* errorDetail) {
    // Same budget as the manifest publish (kPublishAttempts): a scanner holding
    // the staged name routinely releases it within a second or two.
    std::wstring lastDetail;
    unsigned stagingVariant = 0;
    for (int attempt = 1;; ++attempt) {
        DWORD win32Error = ERROR_SUCCESS;
        if (Mp4Muxer::muxToMp4(inputs, outputPath, &lastDetail, stagingVariant, &win32Error)) {
            return true;
        }
        if (attempt >= kPublishAttempts) {
            break;
        }

        // A sharing violation means another process still holds the staged or
        // published name. Rotate to an alternate staging name
        // (<stem>.publish.<n>.mp4) so the next attempt writes to a fresh file
        // instead of colliding with the same handle again, and back off while
        // waiting for it to be released.
        if (win32Error == ERROR_SHARING_VIOLATION || win32Error == ERROR_LOCK_VIOLATION) {
            ++stagingVariant;
        }
        Logger::instance().warning(L"mux",
            L"Native MP4 mux failed on attempt " + std::to_wstring(attempt) + L"/" +
            std::to_wstring(kPublishAttempts) + L"; retrying in " +
            std::to_wstring(kPublishBackoffMs[attempt - 1].count()) + L"ms: " +
            outputPath.wstring());
        std::this_thread::sleep_for(kPublishBackoffMs[attempt - 1]);
    }

    if (errorDetail) {
        *errorDetail = std::move(lastDetail);
    }
    return false;
}

std::filesystem::path recoverLatestFailedRecording(
    const std::filesystem::path& clipDirectory,
    std::wstring& detail) {
    detail.clear();
    const std::filesystem::path root = recoveryRoot(clipDirectory);
    std::error_code error;
    if (clipDirectory.empty() || !std::filesystem::exists(root, error)) {
        detail = L"No failed recording recovery data was found";
        return {};
    }

    std::optional<RecoveryManifest> selected;
    std::filesystem::file_time_type selectedTime{};
    std::filesystem::directory_iterator iterator(root, error);
    const std::filesystem::directory_iterator end;
    if (error) {
        detail = L"Could not enumerate failed recording recovery data in " + root.wstring();
        return {};
    }

    for (; iterator != end; iterator.increment(error)) {
        const auto& entry = *iterator;
        if (error) {
            error = {};
            continue;
        }
        if (!entry.is_directory(error) || error) {
            error = {};
            continue;
        }

        RecoveryManifest manifest;
        if (!readRecoveryManifest(entry.path(), manifest)) {
            continue;
        }

        const auto manifestPath = manifestPathFor(entry.path());
        const auto modified = std::filesystem::last_write_time(manifestPath, error);
        if (error) {
            error = {};
        }
        if (!selected || modified > selectedTime) {
            selected = std::move(manifest);
            selectedTime = modified;
        }
    }

    if (!selected) {
        detail = L"No recoverable failed recording manifest was found in " + root.wstring();
        return {};
    }

    const std::wstring stem = selected->tempDirectory.filename().wstring();
    const std::filesystem::path outputPath = uniqueRecoveredOutputPath(clipDirectory, stem);
    std::wstring muxError;
    if (!muxToMp4WithRetry(selected->inputs, outputPath, &muxError)) {
        // Never delete real content on a failed recovery; the muxer publishes
        // atomically, so only an empty name reservation could be ours.
        removeEmptyOutputReservation(outputPath);
        detail = preservedRecordingDetail(outputPath, selected->tempDirectory, selected->inputs, muxError);
        return {};
    }

    std::error_code cleanupError;
    std::filesystem::remove_all(selected->tempDirectory, cleanupError);
    if (cleanupError) {
        Logger::instance().warning(L"mux", L"Recovered recording but could not remove recovery directory: " + selected->tempDirectory.wstring());
    }

    detail = L"Recovered failed recording to " + outputPath.filename().wstring();
    Logger::instance().info(L"mux", L"Recovered failed recording: " + outputPath.wstring());
    return outputPath;
}

void sweepOrphanedRecoveryData(const std::filesystem::path& clipDirectory, std::chrono::hours maxAge) {
    if (clipDirectory.empty()) {
        return;
    }
    const std::filesystem::path root = recoveryRoot(clipDirectory);
    std::error_code error;
    if (!std::filesystem::exists(root, error) || error) {
        return; // Nothing was ever staged: no crash leftovers to clean.
    }

    // Recover the newest valid manifest before any cleanup runs. This is the
    // only automatic chance to turn a crashed session's preserved streams into
    // a clip; recoverLatestFailedRecording removes its directory on success
    // and preserves the newest one on failure (mux error), leaving it for the
    // manual "Recover Failed" action or for age-based deletion below.
    std::wstring detail;
    const auto recovered = recoverLatestFailedRecording(clipDirectory, detail);
    if (!recovered.empty()) {
        Logger::instance().info(L"mux", L"Startup sweep recovered interrupted recording: " + recovered.wstring());
    } else if (!detail.empty()) {
        Logger::instance().info(L"mux", L"Startup sweep found nothing recoverable: " + detail);
    }

    // Everything remaining in the recovery root older than the threshold is
    // leaked from an old crashed session; delete it. Younger leftovers stay so
    // a recent failure can still be recovered manually.
    const auto cutoff = std::filesystem::file_time_type::clock::now() - maxAge;
    std::size_t removedEntries = 0;
    std::filesystem::directory_iterator iterator(root, error);
    const std::filesystem::directory_iterator end;
    if (error) {
        Logger::instance().warning(L"mux",
            L"Startup sweep could not enumerate recovery data in " + root.wstring() +
            L" (" + utf8ToWide(error.message()) + L")");
        return;
    }
    for (; iterator != end; iterator.increment(error)) {
        if (error) {
            error = {};
            continue;
        }
        const auto& entry = *iterator;
        std::error_code entryError;
        const auto modified = entry.last_write_time(entryError);
        if (entryError || modified >= cutoff) {
            continue; // Missing timestamp or still fresh: leave it alone.
        }
        const auto path = entry.path();
        std::error_code removeError;
        if (entry.is_directory(entryError) && !entryError) {
            std::filesystem::remove_all(path, removeError);
        } else {
            std::filesystem::remove(path, removeError);
        }
        if (removeError) {
            Logger::instance().warning(L"mux", L"Startup sweep could not remove orphaned recovery data: " + path.wstring());
        } else {
            ++removedEntries;
            Logger::instance().info(L"mux", L"Startup sweep removed orphaned recovery data: " + path.wstring());
        }
    }
    if (removedEntries > 0) {
        Logger::instance().info(L"mux",
            L"Startup sweep removed " + std::to_wstring(removedEntries) +
            L" orphaned recovery entries from " + root.wstring());
    }

    // Stale output-name reservations: empty hidden .mp4 placeholders left in
    // the clips folder when a session died between claiming a name and
    // publishing. Identified precisely by the hidden attribute plus zero size,
    // so real content is never touched.
    auto isHiddenFile = [](const std::filesystem::path& path) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_HIDDEN) != 0;
    };
    std::filesystem::directory_iterator clipIterator(clipDirectory, error);
    if (error) {
        error = {};
        return;
    }
    std::size_t removedReservations = 0;
    for (; clipIterator != end; clipIterator.increment(error)) {
        if (error) {
            error = {};
            continue;
        }
        const auto& entry = *clipIterator;
        std::error_code entryError;
        if (!entry.is_regular_file(entryError) || entryError) {
            continue;
        }
        const auto path = entry.path();
        if (!isHiddenFile(path)) {
            continue;
        }
        std::error_code sizeError;
        if (std::filesystem::file_size(path, sizeError) != 0 || sizeError) {
            continue;
        }
        const auto modified = entry.last_write_time(entryError);
        if (entryError || modified >= cutoff) {
            continue;
        }
        std::error_code removeError;
        std::filesystem::remove(path, removeError);
        if (removeError) {
            Logger::instance().warning(L"mux", L"Startup sweep could not remove stale clip name reservation: " + path.wstring());
        } else {
            ++removedReservations;
            Logger::instance().info(L"mux", L"Startup sweep removed stale clip name reservation: " + path.wstring());
        }
    }
    if (removedReservations > 0) {
        Logger::instance().info(L"mux",
            L"Startup sweep removed " + std::to_wstring(removedReservations) + L" stale clip name reservation(s)");
    }
}

} // namespace RecoveryStore
} // namespace backtrack
