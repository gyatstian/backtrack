#include "mux/Mp4Muxer.h"
#include "mux/RecoveryStore.h"

#include "clips/ClipStorage.h"
#include "core/Logger.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
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

std::filesystem::path normalizedDirectory(std::filesystem::path path) {
    path = path.lexically_normal();
    while (path.has_relative_path() && path.filename().empty()) {
        path = path.parent_path();
    }
    return path;
}

bool equalPathsInsensitive(const std::filesystem::path& left, const std::filesystem::path& right) {
    const auto& a = left.native();
    const auto& b = right.native();
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const wchar_t ca = a[i] == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(a[i]));
        const wchar_t cb = b[i] == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(b[i]));
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

bool isValidOutputSubdir(const std::filesystem::path& name) {
    const auto& text = name.native();
    if (text.empty() || name != name.filename() || name == L"." || name == L".." ||
        name.has_root_path() || text.find(L':') != std::wstring::npos ||
        text.find(L'/') != std::wstring::npos || text.find(L'\\') != std::wstring::npos) {
        return false;
    }
    return std::none_of(text.begin(), text.end(), [](wchar_t ch) {
        return ch < 0x20;
    });
}

std::filesystem::path manifestPathFor(const std::filesystem::path& tempDirectory) {
    return tempDirectory / kRecoveryManifestName;
}

std::filesystem::path manifestTempPathFor(const std::filesystem::path& tempDirectory) {
    return tempDirectory / (std::wstring(kRecoveryManifestName) + L".tmp");
}

double bytesToGiB(uint64_t bytes) {
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
    return static_cast<double>(bytes) / kGiB;
}

std::wstring formatGiB(uint64_t bytes) {
    std::wstringstream stream;
    stream << std::fixed << std::setprecision(1) << bytesToGiB(bytes) << L" GB";
    return stream.str();
}

// Full-token numeric parsing. std::stoull happily converts the "123" prefix of
// a line torn mid-number into a plausible-but-wrong offset, which would make
// the muxer read wrong bytes; requiring full consumption rejects those.
template <typename T>
bool parseStrict(const std::string& text, T& value) {
    const char* const first = text.data();
    const char* const last = first + text.size();
    const auto result = std::from_chars(first, last, value);
    return result.ec == std::errc() && result.ptr == last;
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
            if (attempt > 1) {
                Logger::instance().info(L"mux",
                    L"Recovery manifest publish recovered on attempt " +
                    std::to_wstring(attempt) + L"/" + std::to_wstring(kPublishAttempts) +
                    L" after " + std::to_wstring(kPublishBackoffMs[attempt - 2].count()) +
                    L"ms retry delay");
            }
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
    // A recovery output must be claimed exclusively before muxing.
    DWORD lastError = ERROR_SUCCESS;
    auto tryClaim = [&clipDirectory, &lastError](const std::wstring& fileName) -> std::optional<std::filesystem::path> {
        const std::filesystem::path candidate = clipDirectory / fileName;
        lastError = ClipStorage::tryReserveClipOutput(candidate);
        if (lastError == ERROR_SUCCESS) return candidate;
        return std::nullopt;
    };
    const auto failed = [&lastError] {
        return lastError != ERROR_FILE_EXISTS && lastError != ERROR_ALREADY_EXISTS;
    };

    if (auto claimed = tryClaim(stem + L".mp4")) {
        return *claimed;
    }
    if (failed()) return {};
    if (auto claimed = tryClaim(stem + L"_recovered.mp4")) {
        return *claimed;
    }
    if (failed()) return {};
    for (uint32_t suffix = 2; suffix < 1000; ++suffix) {
        if (auto claimed = tryClaim(stem + L"_recovered_" + std::to_wstring(suffix) + L".mp4")) {
            return *claimed;
        }
        if (failed()) return {};
    }
    const std::filesystem::path tickCandidate =
        clipDirectory / (stem + L"_recovered_" + std::to_wstring(GetTickCount64()) + L".mp4");
    return tryClaim(tickCandidate.filename().wstring()).value_or(std::filesystem::path{});
}

// Parses one "sample" line's columns into sample. All numbers must be
// complete tokens; partial values fail and close the table instead of parsing
// into plausible-but-wrong offsets.
bool parseSampleColumns(const std::vector<std::string>& parts, MuxedInputs::VideoSample& sample) {
    if (parts.size() < 6) {
        return false;
    }
    uint32_t keyFrameFlag = 0;
    if (!(parseStrict(parts[1], sample.offset) &&
          parseStrict(parts[2], sample.size) &&
          parseStrict(parts[3], sample.pts100ns) &&
          parseStrict(parts[4], sample.duration100ns) &&
          parseStrict(parts[5], keyFrameFlag))) {
        return false;
    }
    sample.keyFrame = keyFrameFlag != 0;

    // Column added with B-frame support; older manifests fall back to "no
    // decode timestamp", which matches their streams.
    int64_t dts100ns = 0;
    if ((parts.size() >= 7 && !parseStrict(parts[6], dts100ns))) {
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
        if (!parseStrict(parts[7], hasDtsFlag)) {
            return false;
        }
        sample.hasDts = hasDtsFlag != 0;
    }

    // Every column above is an individually valid int64, but the mux and
    // effectiveDts then compute pts - dts from them, and a pair whose
    // difference is unrepresentable wraps into the opposite sign there. pts 0
    // with dts INT64_MIN passes parsing and the order checks and then overflows
    // every downstream sum. Only the non-negative direction is ever subtracted
    // (both effectiveDts and the composition offset use a decode timestamp only
    // when it does not exceed the presentation time), so only that one is
    // bounded. The difference is taken in uint64 - exact for a non-negative
    // result - so the check itself cannot overflow.
    constexpr uint64_t kInt64Max = static_cast<uint64_t>((std::numeric_limits<int64_t>::max)());
    if (sample.dts100ns <= sample.pts100ns &&
        static_cast<uint64_t>(sample.pts100ns) - static_cast<uint64_t>(sample.dts100ns) > kInt64Max) {
        return false;
    }
    // Same bound for the sample end: pts + duration must be representable.
    const int64_t duration100ns = sample.duration100ns;
    if ((duration100ns > 0 &&
         sample.pts100ns > (std::numeric_limits<int64_t>::max)() - duration100ns) ||
        (duration100ns < 0 &&
         sample.pts100ns < (std::numeric_limits<int64_t>::min)() - duration100ns)) {
        return false;
    }
    return true;
}

bool isRecoveryInputFile(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
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
            damagedMetadata = !parseStrict(parts[1], manifest.inputs.fps);
        } else if (parts[0] == "width" && parts.size() >= 2) {
            damagedMetadata = !parseStrict(parts[1], manifest.inputs.width);
        } else if (parts[0] == "height" && parts.size() >= 2) {
            damagedMetadata = !parseStrict(parts[1], manifest.inputs.height);
        } else if (parts[0] == "bitrateKbps" && parts.size() >= 2) {
            damagedMetadata = !parseStrict(parts[1], manifest.inputs.bitrateKbps);
        } else if ((parts[0] == "video" || parts[0] == "systemAudio" || parts[0] == "microphoneAudio") && parts.size() >= 2) {
            const std::filesystem::path name(parts[1]);
            damagedMetadata = name.empty() || name != name.filename() || name == L"." || name == L".." ||
                              name.has_root_path() || name.native().find(L':') != std::wstring::npos;
            if (!damagedMetadata) {
                auto& destination = parts[0] == "video" ? manifest.inputs.videoPath :
                    (parts[0] == "systemAudio" ? manifest.inputs.systemAudioPath : manifest.inputs.microphoneAudioPath);
                destination = tempDirectory / name;
            }
        } else if (parts[0] == "outputSubdir") {
            if (parts.size() != 2) {
                damagedMetadata = true;
            } else {
                const std::filesystem::path name(utf8ToWide(parts[1]));
                damagedMetadata = !isValidOutputSubdir(name);
                if (!damagedMetadata) {
                    manifest.inputs.outputSubdir = name;
                }
            }
        } else if (parts[0] == "samples" && parts.size() >= 2) {
            uint64_t count = 0;
            if (parseStrict(parts[1], count)) {
                // The declared count is only the shortfall check below. It must
                // not size the vector: a manifest claiming 10,000,000 samples
                // used to reserve ~534 MiB before a single media byte was
                // validated, and the startup sweep keeps several manifests
                // alive at once. Growth follows the sample lines actually
                // parsed instead.
                expectedSamples = static_cast<size_t>(count);
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
    // Missing audio is optional, but a listed stream that cannot be staged
    // as a regular file must not make the session a recovery candidate.
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

    if (!isRecoveryInputFile(manifest.inputs.videoPath) ||
        (!manifest.inputs.systemAudioPath.empty() && !isRecoveryInputFile(manifest.inputs.systemAudioPath)) ||
        (!manifest.inputs.microphoneAudioPath.empty() && !isRecoveryInputFile(manifest.inputs.microphoneAudioPath))) {
        return false;
    }

    // The mux writes every sample time as (time - timeBase). A manifest can
    // carry individually valid timestamps whose span does not fit int64, so
    // keep the prefix that does and treat the rest as the damaged tail.
    if (!manifest.inputs.videoSamples.empty()) {
        const int64_t timeBase = muxTimeBase100ns(manifest.inputs);
        auto& samples = manifest.inputs.videoSamples;
        for (size_t index = 0; index < samples.size(); ++index) {
            const bool fits = int64DifferenceFits(samples[index].pts100ns, timeBase) &&
                (!samples[index].hasDts || int64DifferenceFits(samples[index].dts100ns, timeBase));
            if (!fits) {
                samples.resize(index);
                Logger::instance().warning(L"mux",
                    L"Ignoring recovery-manifest samples after an unrepresentable timestamp span in " +
                    manifestPath.wstring());
                break;
            }
        }
    }

    // Flush-desync guard: the incremental manifest is flushed at GOP
    // boundaries while the elementary stream's own write buffer can lag
    // behind, so after a crash the last recorded samples may reference bytes
    // that never reached disk. Trim that tail instead of failing an otherwise
    // fully recoverable session.
    std::error_code sizeError;
    const uint64_t streamBytes = std::filesystem::file_size(manifest.inputs.videoPath, sizeError);
    if (sizeError) {
        return false;
    }
    auto& samples = manifest.inputs.videoSamples;
    while (!samples.empty()) {
        const MuxedInputs::VideoSample& last = samples.back();
        if (last.offset <= streamBytes &&
            static_cast<uint64_t>(last.size) <= streamBytes - last.offset) {
            break;
        }
        samples.pop_back();
    }

    // A full rewrite interrupted mid-write could leave fewer lines than the
    // declared count even with atomic publish as last line of defense.
    return (expectedSamples == 0 || samples.size() <= expectedSamples) &&
           std::any_of(samples.begin(), samples.end(), [](const auto& sample) { return sample.size != 0; });
}

} // namespace

TempSessionTarget resolveTempSessionTarget(
    const std::filesystem::path& outputPath,
    const std::filesystem::path& clipRoot) {
    const auto outputDir = outputPath.parent_path();
    if (clipRoot.empty() || outputDir.empty()) {
        return {outputDir, {}};
    }
    const auto normRoot = normalizedDirectory(clipRoot);
    const auto normOutput = normalizedDirectory(outputDir);
    if (equalPathsInsensitive(normOutput, normRoot)) {
        return {normRoot, {}};
    }
    // Only hoist staging into clipRoot/.backtrack_tmp when outputDir is an
    // immediate non-reparse child of clipRoot: MoveFileExW requires staging
    // and final output on the same volume, and recovery/library scans only
    // cover one game-folder level below the root.
    const auto parentOfOutput = normalizedDirectory(normOutput.parent_path());
    const auto subdir = normOutput.filename();
    if (equalPathsInsensitive(parentOfOutput, normRoot) &&
        isValidOutputSubdir(subdir) &&
        ClipStorage::isDirectoryWithoutReparsePoint(normRoot) &&
        ClipStorage::isDirectoryWithoutReparsePoint(normOutput)) {
        return {normRoot, subdir};
    }
    return {outputDir, {}};
}

std::filesystem::path createUniqueDirectory(
    const std::filesystem::path& preferredPath, std::error_code& error) {
    std::filesystem::create_directories(preferredPath.parent_path(), error);
    if (error) {
        return {};
    }
    for (uint64_t suffix = 0;; ++suffix) {
        auto candidate = preferredPath;
        if (suffix != 0) {
            candidate += L"_" + std::to_wstring(suffix);
        }
        error.clear();
        if (std::filesystem::create_directory(candidate, error)) {
            return candidate;
        }
        if (error && error != std::errc::file_exists) {
            return {};
        }
        if (suffix == (std::numeric_limits<uint64_t>::max)()) {
            error = std::make_error_code(std::errc::file_exists);
            return {};
        }
    }
}

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
    if (!partialInputs.outputSubdir.empty()) {
        stream << "outputSubdir\t" << wideToUtf8(partialInputs.outputSubdir.wstring()) << "\n";
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
    const auto fail = [&tempPath](const std::wstring& reason) {
        Logger::instance().warning(L"mux", reason);
        std::error_code removeError;
        std::filesystem::remove(tempPath, removeError);
        return false;
    };
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
            stream.close();
            return fail(L"Could not write recording recovery manifest: " + manifestPath.wstring());
        }
    }

    DWORD flushError = ERROR_SUCCESS;
    if (!flushFileToDisk(tempPath, flushError)) {
        return fail(L"Could not flush recording recovery manifest: " + tempPath.wstring() +
            L" (Win32 error " + std::to_wstring(flushError) + L")");
    }
    if (!publishWithRetry(tempPath, manifestPath)) {
        const DWORD publishError = GetLastError();
        return fail(L"Could not publish recording recovery manifest: " + manifestPath.wstring() +
            L" (Win32 error " + std::to_wstring(publishError) + L")");
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

// Estimate covers the caller's byte count plus unmeasured container overhead.
// A failed volume query is not a failure: let the writers surface any real
// problem instead of blocking a save on a transient query error.
bool ensureFreeDiskSpace(
    const std::filesystem::path& outputPath,
    uint64_t neededBytes,
    std::wstring& detail) {
    ULARGE_INTEGER freeToCaller{};
    ULARGE_INTEGER totalBytes{};
    ULARGE_INTEGER totalFree{};
    const std::filesystem::path volumeRoot = outputPath.parent_path().empty()
        ? std::filesystem::path(L".")
        : outputPath.parent_path();
    if (!GetDiskFreeSpaceExW(volumeRoot.c_str(), &freeToCaller, &totalBytes, &totalFree)) {
        return true;
    }
    if (freeToCaller.QuadPart >= neededBytes) {
        return true;
    }

    detail = L"not enough free disk space (need about " + formatGiB(neededBytes) +
             L", have " + formatGiB(freeToCaller.QuadPart) + L")";
    return false;
}

MuxPublishResult publishMuxedInputs(
    const MuxedInputs& inputs,
    const std::filesystem::path& outputPath,
    const std::filesystem::path& tempDirectory,
    std::wstring* errorDetail) {
    // a failed rewrite must not remove a recording's incremental manifest.
    if (writeManifest(inputs, tempDirectory)) {
        writeReadme(inputs, tempDirectory);
    }

    std::wstring muxErrorDetail;
    // Same retry wrapper for both paths: a transient Media Foundation
    // init/finalize failure should not fail a save that one retry would pass.
    if (muxToMp4WithRetry(inputs, outputPath, &muxErrorDetail)) {
        return MuxPublishResult::kPublished;
    }

    // Only an empty output-name reservation may be removed; a real file that
    // appeared at the target in the meantime must survive.
    removeEmptyOutputReservation(outputPath);
    const auto detail = preserveFailedRecording(
        inputs, outputPath, tempDirectory,
        muxErrorDetail.empty() ? L"MP4 muxing failed; see log for details" : muxErrorDetail);
    if (errorDetail) {
        *errorDetail = detail;
    }
    Logger::instance().warning(L"mux", detail);
    return MuxPublishResult::kPreserved;
}

std::wstring preserveFailedRecording(
    const MuxedInputs& inputs,
    const std::filesystem::path& outputPath,
    const std::filesystem::path& tempDirectory,
    const std::wstring& reason) {
    std::error_code error;
    if (!std::filesystem::exists(manifestPathFor(tempDirectory), error) && !error &&
        !inputs.videoSamples.empty()) {
        writeManifest(inputs, tempDirectory);
    }
    RecoveryManifest manifest;
    if (readRecoveryManifest(tempDirectory, manifest)) {
        return preservedRecordingDetail(outputPath, tempDirectory, manifest.inputs, reason);
    }
    return reason + L". Preserved temporary streams in " + tempDirectory.wstring() +
        L". Automatic recovery could not verify timing data; the streams may need manual recovery.";
}

std::wstring preservedRecordingDetail(
    const std::filesystem::path& outputPath,
    const std::filesystem::path& tempDirectory,
    const MuxedInputs& inputs,
    const std::wstring& reason) {
    const auto tempParent = tempDirectory.parent_path();
    const auto libraryDirectory =
        (tempParent.filename() == L".backtrack_tmp" && !tempParent.parent_path().empty())
            ? tempParent.parent_path()
            : (outputPath.parent_path().empty() ? std::filesystem::path(L".") : outputPath.parent_path());
    std::wstring detail;
    if (!reason.empty()) {
        detail = reason + L". ";
    }
    return detail +
           L"Preserved " + std::to_wstring(inputs.videoSamples.size()) +
           L" video packets and elementary streams in " + tempDirectory.wstring() +
           L". Use Recover Failed in the clip library, or retry with " + libraryDirectory.wstring() +
           L" as the library. The video/audio files can also be recovered manually.";
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
            if (attempt > 1) {
                Logger::instance().info(L"mux",
                    L"Native MP4 mux recovered on attempt " +
                    std::to_wstring(attempt) + L"/" + std::to_wstring(kPublishAttempts) +
                    L" after " + std::to_wstring(kPublishBackoffMs[attempt - 2].count()) +
                    L"ms retry delay");
            }
            return true;
        }
        if (attempt >= kPublishAttempts) {
            break;
        }

        // rotate the preferred staging directory after a sharing violation.
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

namespace {

constexpr wchar_t kRecoveredMarkerName[] = L".backtrack_recovered";

struct LocatedManifest {
    std::filesystem::path outputDirectory;
    RecoveryManifest manifest;
    std::filesystem::file_time_type modified{};
};

bool outputIdentity(const std::filesystem::path& path, std::array<DWORD, 7>& identity) {
    const HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(file, &info) != FALSE;
    CloseHandle(file);
    if (!ok || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        (info.nFileSizeHigh == 0 && info.nFileSizeLow == 0)) return false;
    identity = {info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow,
                info.nFileSizeHigh, info.nFileSizeLow,
                info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime};
    return true;
}

bool alreadyRecovered(const LocatedManifest& located) {
    std::error_code error;
    std::filesystem::directory_iterator it(located.manifest.tempDirectory, error), end;
    for (; !error && it != end; it.increment(error)) {
        const std::wstring name = it->path().filename().wstring();
        const std::wstring prefix = kRecoveredMarkerName;
        if (!name.starts_with(prefix)) continue;
        const auto suffix = name.substr(prefix.size());
        if (!suffix.empty() && (suffix.size() < 2 || suffix.front() != L'_' ||
            !std::all_of(suffix.begin() + 1, suffix.end(), [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; }))) continue;
        std::error_code entryError;
        const auto markerTime = it->last_write_time(entryError);
        if (entryError || markerTime < located.modified) continue;
        std::ifstream stream(it->path(), std::ios::binary);
        std::string magic, outputName;
        if (!std::getline(stream, magic) || magic != "backtrack-recovered-v2" || !std::getline(stream, outputName)) continue;
        const std::filesystem::path fileName(utf8ToWide(outputName));
        if (fileName.empty() || fileName != fileName.filename() || fileName.has_root_path() ||
            fileName.native().find(L':') != std::wstring::npos) continue;
        std::array<DWORD, 7> expected{}, actual{};
        for (auto& value : expected) stream >> value;
        if (!stream) continue;
        const auto output = located.outputDirectory / fileName;
        if (outputIdentity(output, actual) && expected == actual) return true;
        // The published clip is gone from the recorded path, so the user deleted
        // or renamed it in the library: the session was already published and
        // acted on, and recovering it again would resurrect or duplicate it.
        // A file still at that path is a different clip and may recover again.
        if (GetFileAttributesW(output.c_str()) == INVALID_FILE_ATTRIBUTES) {
            const DWORD missing = GetLastError();
            if (missing == ERROR_FILE_NOT_FOUND || missing == ERROR_PATH_NOT_FOUND) return true;
        }
    }
    return false;
}

void markRecovered(const LocatedManifest& located, const std::filesystem::path& output) {
    std::array<DWORD, 7> identity{};
    if (!outputIdentity(output, identity)) {
        Logger::instance().warning(L"mux", L"Could not identify recovered clip: " + output.wstring());
        return;
    }
    HANDLE file = INVALID_HANDLE_VALUE;
    std::filesystem::path marker;
    for (uint32_t suffix = 0; suffix < 10000; ++suffix) {
        marker = located.manifest.tempDirectory /
            (std::wstring(kRecoveredMarkerName) + (suffix ? L"_" + std::to_wstring(suffix) : L""));
        file = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) break;
    }
    if (file == INVALID_HANDLE_VALUE) {
        Logger::instance().warning(L"mux", L"Could not mark preserved recovery as completed: " + marker.wstring());
        return;
    }
    std::string text = "backtrack-recovered-v2\n" + wideToUtf8(output.filename().wstring()) + "\n";
    for (const auto value : identity) text += std::to_string(value) + " ";
    text += '\n';
    DWORD written = 0;
    const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                    written == static_cast<DWORD>(text.size()) && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) {
        Logger::instance().warning(L"mux", L"Could not finish recovery marker: " + marker.wstring());
    }
}

std::vector<LocatedManifest> findManifests(const std::filesystem::path& clipDirectory, bool stopAfterFirst = false) {
    std::vector<LocatedManifest> result;
    std::error_code error;
    const auto roots = ClipStorage::clipRoots(clipDirectory, error);
    if (error) {
        Logger::instance().warning(L"mux", L"Could not enumerate clip roots in " + clipDirectory.wstring() +
                                   L" (" + utf8ToWide(error.message()) + L")");
    }
    for (const auto& outputDirectory : roots) {
        const auto root = recoveryRoot(outputDirectory);
        if (!ClipStorage::isDirectoryWithoutReparsePoint(root)) {
            continue;
        }
        error.clear();
        std::filesystem::directory_iterator it(root, error), end;
        if (error) {
            Logger::instance().warning(L"mux", L"Could not enumerate recovery data in " + root.wstring() +
                                       L" (" + utf8ToWide(error.message()) + L")");
            continue;
        }
        for (; !error && it != end; it.increment(error)) {
            const auto session = it->path();
            if (!ClipStorage::isDirectoryWithoutReparsePoint(session)) {
                continue;
            }
            LocatedManifest located;
            located.outputDirectory = outputDirectory;
            if (!readRecoveryManifest(session, located.manifest)) {
                Logger::instance().warning(L"mux",
                    L"Ignoring unreadable recovery manifest: " + manifestPathFor(session).wstring());
                continue;
            }
            if (outputDirectory == clipDirectory && !located.manifest.inputs.outputSubdir.empty()) {
                const auto candidate = outputDirectory / located.manifest.inputs.outputSubdir;
                const DWORD attrs = GetFileAttributesW(candidate.c_str());
                if (attrs == INVALID_FILE_ATTRIBUTES) {
                    const DWORD attrError = GetLastError();
                    if (attrError == ERROR_FILE_NOT_FOUND || attrError == ERROR_PATH_NOT_FOUND) {
                        located.outputDirectory = candidate;
                    }
                } else if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                           (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                    located.outputDirectory = candidate;
                }
            }
            std::error_code timeError;
            located.modified = std::filesystem::last_write_time(manifestPathFor(session), timeError);
            if (!timeError && !alreadyRecovered(located)) {
                result.push_back(std::move(located));
                if (stopAfterFirst) {
                    return result;
                }
            }
        }
        if (error) {
            Logger::instance().warning(L"mux", L"Could not finish enumerating recovery data in " + root.wstring() +
                                       L" (" + utf8ToWide(error.message()) + L")");
        }
    }
    return result;
}

bool copyRecoveryInput(const std::filesystem::path& source, const std::filesystem::path& destination) {
    if (source.empty()) {
        return true;
    }
    return isRecoveryInputFile(source) &&
           (CreateHardLinkW(destination.c_str(), source.c_str(), nullptr) ||
            CopyFileW(source.c_str(), destination.c_str(), TRUE));
}

std::filesystem::path recoverManifest(const LocatedManifest& located, std::wstring& detail) {
    const auto& manifest = located.manifest;
    std::error_code error;
    const auto work = createUniqueDirectory(manifest.tempDirectory.parent_path() / L".backtrack_recovery_work", error);
    if (work.empty()) {
        detail = L"Could not create recovery work directory beside " + manifest.tempDirectory.wstring();
        return {};
    }
    struct WorkCleanup {
        std::filesystem::path path;
        ~WorkCleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
            if (error) {
                Logger::instance().warning(L"mux", L"Could not remove recovery work directory: " + path.wstring());
            }
        }
    } cleanup{work};

    // The muxer writes mixed_audio.wav beside its inputs; legacy sessions can
    // contain unrelated files with that name, so mux only from owned work files.
    auto inputs = manifest.inputs;
    const auto video = work / (inputs.codec == VideoCodec::H264 ? L"video.h264" : L"video.hevc");
    const auto system = work / L"system.wav";
    const auto microphone = work / L"microphone.wav";
    if (!copyRecoveryInput(inputs.videoPath, video) ||
        !copyRecoveryInput(inputs.systemAudioPath, system) ||
        !copyRecoveryInput(inputs.microphoneAudioPath, microphone)) {
        detail = L"Could not copy recovery streams from " + manifest.tempDirectory.wstring();
        return {};
    }
    inputs.videoPath = video;
    if (!inputs.systemAudioPath.empty()) inputs.systemAudioPath = system;
    if (!inputs.microphoneAudioPath.empty()) inputs.microphoneAudioPath = microphone;

    auto targetDirectory = located.outputDirectory;
    const auto sessionRoot = manifest.tempDirectory.parent_path().parent_path();
    if (!manifest.inputs.outputSubdir.empty() && targetDirectory != sessionRoot) {
        std::error_code dirError;
        std::filesystem::create_directories(targetDirectory, dirError);
        if (dirError || !ClipStorage::isDirectoryWithoutReparsePoint(targetDirectory)) {
            targetDirectory = sessionRoot;
        }
    }

    const auto output = uniqueRecoveredOutputPath(targetDirectory, manifest.tempDirectory.filename().wstring());
    if (output.empty()) {
        detail = L"Could not reserve a recovery output in " + targetDirectory.wstring();
        return {};
    }
    std::wstring muxError;
    if (!muxToMp4WithRetry(inputs, output, &muxError)) {
        removeEmptyOutputReservation(output);
        detail = preservedRecordingDetail(output, manifest.tempDirectory, manifest.inputs, muxError);
        return {};
    }

    // Old session directories are not evidence that every file in them is ours.
    markRecovered(located, output);
    detail = L"Recovered failed recording to " + output.filename().wstring();
    Logger::instance().info(L"mux", L"Recovered failed recording: " + output.wstring());
    return output;
}

} // namespace

bool hasRecoverableRecording(const std::filesystem::path& clipDirectory) {
    return !findManifests(clipDirectory, /*stopAfterFirst=*/true).empty();
}

std::filesystem::path recoverLatestFailedRecording(
    const std::filesystem::path& clipDirectory,
    std::wstring& detail) {
    detail.clear();
    auto manifests = findManifests(clipDirectory);
    if (manifests.empty()) {
        detail = L"No recoverable failed recording manifest was found in " + clipDirectory.wstring();
        return {};
    }
    const auto selected = std::max_element(manifests.begin(), manifests.end(),
        [](const LocatedManifest& a, const LocatedManifest& b) { return a.modified < b.modified; });
    return recoverManifest(*selected, detail);
}

void sweepOrphanedRecoveryData(const std::filesystem::path& clipDirectory) {
    for (const auto& located : findManifests(clipDirectory)) {
        std::wstring detail;
        if (recoverManifest(located, detail).empty()) {
            Logger::instance().warning(L"mux", L"Startup recovery failed: " + detail);
        }
    }
    // A hidden empty file alone does not prove ownership of a legacy reservation.
}

} // namespace RecoveryStore
} // namespace backtrack
