#include "mux/RecoveryStore.h"

#include "core/Logger.h"

#include <chrono>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace backtrack {
namespace RecoveryStore {
namespace {

constexpr wchar_t kRecoveryManifestName[] = L"recovery.tsv";
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

std::filesystem::path uniqueRecoveredOutputPath(const std::filesystem::path& clipDirectory, const std::wstring& stem) {
    std::error_code error;
    const std::filesystem::path preferred = clipDirectory / (stem + L".mp4");
    if (!std::filesystem::exists(preferred, error)) {
        return preferred;
    }

    const std::filesystem::path recovered = clipDirectory / (stem + L"_recovered.mp4");
    error = {};
    if (!std::filesystem::exists(recovered, error)) {
        return recovered;
    }

    for (uint32_t suffix = 2; suffix < 1000; ++suffix) {
        const auto candidate = clipDirectory / (stem + L"_recovered_" + std::to_wstring(suffix) + L".mp4");
        error = {};
        if (!std::filesystem::exists(candidate, error)) {
            return candidate;
        }
    }
    return clipDirectory / (stem + L"_recovered_" + std::to_wstring(GetTickCount64()) + L".mp4");
}

bool readRecoveryManifest(const std::filesystem::path& tempDirectory, RecoveryManifest& manifest) {
    std::ifstream stream(manifestPathFor(tempDirectory));
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
    while (std::getline(stream, line)) {
        if (line.empty()) {
            continue;
        }

        const auto parts = splitTabs(line);
        if (parts.empty()) {
            continue;
        }

        try {
            if (parts[0] == "codec" && parts.size() >= 2) {
                manifest.inputs.codec = codecFromText(parts[1]);
            } else if (parts[0] == "fps" && parts.size() >= 2) {
                manifest.inputs.fps = static_cast<uint32_t>(std::stoul(parts[1]));
            } else if (parts[0] == "width" && parts.size() >= 2) {
                manifest.inputs.width = static_cast<uint32_t>(std::stoul(parts[1]));
            } else if (parts[0] == "height" && parts.size() >= 2) {
                manifest.inputs.height = static_cast<uint32_t>(std::stoul(parts[1]));
            } else if (parts[0] == "bitrateKbps" && parts.size() >= 2) {
                manifest.inputs.bitrateKbps = static_cast<uint32_t>(std::stoul(parts[1]));
            } else if (parts[0] == "video" && parts.size() >= 2) {
                manifest.inputs.videoPath = tempDirectory / std::filesystem::path(parts[1]);
            } else if (parts[0] == "systemAudio" && parts.size() >= 2) {
                manifest.inputs.systemAudioPath = tempDirectory / std::filesystem::path(parts[1]);
            } else if (parts[0] == "microphoneAudio" && parts.size() >= 2) {
                manifest.inputs.microphoneAudioPath = tempDirectory / std::filesystem::path(parts[1]);
            } else if (parts[0] == "samples" && parts.size() >= 2) {
                expectedSamples = static_cast<size_t>(std::stoull(parts[1]));
                manifest.inputs.videoSamples.reserve(expectedSamples);
            } else if (parts[0] == "sample" && parts.size() >= 6) {
                MuxedInputs::VideoSample sample;
                sample.offset = std::stoull(parts[1]);
                sample.size = static_cast<uint32_t>(std::stoul(parts[2]));
                sample.pts100ns = std::stoll(parts[3]);
                sample.duration100ns = std::stoll(parts[4]);
                sample.keyFrame = std::stoul(parts[5]) != 0;
                manifest.inputs.videoSamples.push_back(sample);
            }
        } catch (...) {
            return false;
        }
    }

    if (manifest.inputs.videoPath.empty()) {
        manifest.inputs.videoPath = tempDirectory / (manifest.inputs.codec == VideoCodec::H264 ? L"video.h264" : L"video.hevc");
    }
    if (!manifest.inputs.systemAudioPath.empty() && !std::filesystem::exists(manifest.inputs.systemAudioPath)) {
        manifest.inputs.systemAudioPath.clear();
    }
    if (!manifest.inputs.microphoneAudioPath.empty() && !std::filesystem::exists(manifest.inputs.microphoneAudioPath)) {
        manifest.inputs.microphoneAudioPath.clear();
    }

    return std::filesystem::exists(manifest.inputs.videoPath) &&
           !manifest.inputs.videoSamples.empty() &&
           (expectedSamples == 0 || manifest.inputs.videoSamples.size() == expectedSamples);
}

} // namespace

bool writeManifest(const MuxedInputs& inputs, const std::filesystem::path& tempDirectory) {
    std::ofstream stream(manifestPathFor(tempDirectory), std::ios::out | std::ios::trunc);
    if (!stream.is_open()) {
        Logger::instance().warning(L"mux", L"Could not write recording recovery manifest: " + manifestPathFor(tempDirectory).wstring());
        return false;
    }

    stream << "backtrack-recovery-v1\n";
    stream << "codec\t" << codecText(inputs.codec) << "\n";
    stream << "fps\t" << inputs.fps << "\n";
    stream << "width\t" << inputs.width << "\n";
    stream << "height\t" << inputs.height << "\n";
    stream << "bitrateKbps\t" << inputs.bitrateKbps << "\n";
    stream << "video\t" << narrowFileName(inputs.videoPath) << "\n";
    if (!inputs.systemAudioPath.empty()) {
        stream << "systemAudio\t" << narrowFileName(inputs.systemAudioPath) << "\n";
    }
    if (!inputs.microphoneAudioPath.empty()) {
        stream << "microphoneAudio\t" << narrowFileName(inputs.microphoneAudioPath) << "\n";
    }
    stream << "samples\t" << inputs.videoSamples.size() << "\n";
    for (const auto& sample : inputs.videoSamples) {
        stream << "sample\t"
               << sample.offset << "\t"
               << sample.size << "\t"
               << sample.pts100ns << "\t"
               << sample.duration100ns << "\t"
               << (sample.keyFrame ? 1 : 0) << "\n";
    }
    return stream.good();
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
    const MuxedInputs& inputs) {
    return L"Native MP4 mux failed after retry for " + outputPath.filename().wstring() +
           L". Preserved " + std::to_wstring(inputs.videoSamples.size()) +
           L" video packets and elementary streams in " + tempDirectory.wstring() +
           L". Use Recover Failed to retry, or recover manually from the preserved video/audio files.";
}

bool muxToMp4WithRetry(const MuxedInputs& inputs, const std::filesystem::path& outputPath) {
    if (Mp4Muxer::muxToMp4(inputs, outputPath)) {
        return true;
    }

    Logger::instance().warning(L"mux", L"Native MP4 mux failed; retrying once after a short delay: " + outputPath.wstring());
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    return Mp4Muxer::muxToMp4(inputs, outputPath);
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
    if (!muxToMp4WithRetry(selected->inputs, outputPath)) {
        std::error_code removeError;
        std::filesystem::remove(outputPath, removeError);
        detail = preservedRecordingDetail(outputPath, selected->tempDirectory, selected->inputs);
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

} // namespace RecoveryStore
} // namespace backtrack
