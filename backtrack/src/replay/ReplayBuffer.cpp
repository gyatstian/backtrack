#include "replay/ReplayBuffer.h"

#include "core/Logger.h"
#include "mux/Mp4Muxer.h"
#include "mux/WavWriter.h"

#include <mmreg.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

namespace backtrack {

namespace {

class ScopedDirectoryCleanup {
public:
    explicit ScopedDirectoryCleanup(std::filesystem::path path)
        : path_(std::move(path)) {
    }

    ~ScopedDirectoryCleanup() {
        cleanup();
    }

    void cleanup() {
        if (path_.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        if (error) {
            Logger::instance().warning(L"replay", L"Could not remove replay temporary directory: " + path_.wstring());
        }
        path_.clear();
    }

private:
    std::filesystem::path path_;
};

void writeTimelineAudio(
    WavWriter& writer,
    const std::vector<std::shared_ptr<const AudioPacket>>& audio,
    const std::filesystem::path& path,
    int64_t startPts100ns,
    int64_t endPts100ns) {
    if (audio.empty() || endPts100ns <= startPts100ns) {
        return;
    }

    const auto formatPacket = std::find_if(audio.begin(), audio.end(), [](const auto& packet) {
        return packet->format && packet->frameCount > 0 && !packet->bytes.empty();
    });
    if (formatPacket == audio.end() || !writer.open(path, *(*formatPacket)->format)) {
        return;
    }

    const uint32_t samplesPerSec = writer.samplesPerSec();
    const uint16_t blockAlign = writer.blockAlign();
    if (samplesPerSec == 0 || blockAlign == 0) {
        writer.close();
        return;
    }

    int64_t nextPts100ns = startPts100ns;
    for (const auto& packet : audio) {
        if (packet->frameCount == 0 || packet->bytes.empty()) {
            continue;
        }

        const int64_t packetDuration100ns =
            static_cast<int64_t>((static_cast<int64_t>(packet->frameCount) * kHundredNanosecondsPerSecond) / samplesPerSec);
        const int64_t packetEnd100ns = packet->pts100ns + packetDuration100ns;
        if (packetEnd100ns <= nextPts100ns) {
            continue;
        }
        if (packet->pts100ns >= endPts100ns) {
            continue;
        }

        if (packet->pts100ns > nextPts100ns) {
            const int64_t gapEnd100ns = std::min(packet->pts100ns, endPts100ns);
            const auto gapFrames = static_cast<uint64_t>(
                ((gapEnd100ns - nextPts100ns) * static_cast<int64_t>(samplesPerSec)) / kHundredNanosecondsPerSecond);
            writer.writeSilenceFrames(gapFrames);
            nextPts100ns += static_cast<int64_t>((gapFrames * kHundredNanosecondsPerSecond) / samplesPerSec);
            if (nextPts100ns >= endPts100ns) {
                break;
            }
        }

        uint32_t startFrame = 0;
        if (packet->pts100ns < nextPts100ns) {
            startFrame = static_cast<uint32_t>(std::min<uint64_t>(
                packet->frameCount,
                (((nextPts100ns - packet->pts100ns) * static_cast<int64_t>(samplesPerSec)) + kHundredNanosecondsPerSecond - 1) /
                    kHundredNanosecondsPerSecond));
        }
        if (startFrame >= packet->frameCount) {
            continue;
        }

        const size_t byteOffset = static_cast<size_t>(startFrame) * blockAlign;
        if (byteOffset >= packet->bytes.size()) {
            continue;
        }
        const int64_t writeStartPts100ns =
            packet->pts100ns +
            static_cast<int64_t>((static_cast<int64_t>(startFrame) * kHundredNanosecondsPerSecond) / samplesPerSec);
        if (writeStartPts100ns >= endPts100ns) {
            continue;
        }
        const uint64_t framesToReplayEnd = static_cast<uint64_t>(
            ((endPts100ns - writeStartPts100ns) * static_cast<int64_t>(samplesPerSec)) /
            kHundredNanosecondsPerSecond);
        if (framesToReplayEnd == 0) {
            continue;
        }
        const uint64_t frameLimit = std::min<uint64_t>(
            framesToReplayEnd,
            static_cast<uint64_t>((std::numeric_limits<size_t>::max)() / blockAlign));
        const size_t bytesToWrite = std::min(
            {packet->bytes.size() - byteOffset,
             static_cast<size_t>(packet->frameCount - startFrame) * blockAlign,
             static_cast<size_t>(frameLimit) * blockAlign});
        writer.write(packet->bytes.data() + byteOffset, bytesToWrite);
        const uint64_t framesWritten = bytesToWrite / blockAlign;
        nextPts100ns += static_cast<int64_t>((framesWritten * kHundredNanosecondsPerSecond) / samplesPerSec);
        if (nextPts100ns >= endPts100ns) {
            break;
        }
    }
    writer.close();
}

int64_t audioPacketEndPts100ns(const AudioPacket& packet) {
    if (!packet.format || packet.frameCount == 0 || packet.format->bytes.size() < sizeof(WAVEFORMATEX)) {
        return packet.pts100ns;
    }

    const auto* format = reinterpret_cast<const WAVEFORMATEX*>(packet.format->bytes.data());
    if (format->nSamplesPerSec == 0) {
        return packet.pts100ns;
    }

    const int64_t duration100ns =
        static_cast<int64_t>((static_cast<int64_t>(packet.frameCount) * kHundredNanosecondsPerSecond) /
                             format->nSamplesPerSec);
    return packet.pts100ns + duration100ns;
}

} // namespace

void ReplayBuffer::configure(const ReplaySettings& settings) {
    std::scoped_lock lock(mutex_);
    settings_ = settings;
    if (!settings_.enabled) {
        video_.clear();
        systemAudio_.clear();
        microphoneAudio_.clear();
        videoKeyFrames_ = 0;
        lastSaveEndPts100ns_ = 0;
        return;
    }
    trimLocked();
}

void ReplayBuffer::pushVideo(EncodedPacket packet) {
    if (packet.bytes.empty()) {
        return;
    }
    std::scoped_lock lock(mutex_);
    if (!settings_.enabled) {
        return;
    }
    if (packet.keyFrame) {
        ++videoKeyFrames_;
    }
    video_.push_back(std::make_shared<EncodedPacket>(std::move(packet)));
    trimLocked();
}

bool ReplayBuffer::extendLastVideoDuration(int64_t expectedPts100ns, int64_t duration100ns) {
    std::scoped_lock lock(mutex_);
    if (!settings_.enabled || video_.empty() || duration100ns <= 0 ||
        video_.back()->pts100ns != expectedPts100ns) {
        return false;
    }

    auto extended = std::make_shared<EncodedPacket>(*video_.back());
    extended->duration100ns = std::max(extended->duration100ns, duration100ns);
    video_.back() = std::move(extended);
    return true;
}

void ReplayBuffer::pushAudio(AudioPacket packet) {
    if (packet.bytes.empty()) {
        return;
    }
    std::scoped_lock lock(mutex_);
    if (!settings_.enabled) {
        return;
    }
    auto& queue = packet.track == AudioTrack::System ? systemAudio_ : microphoneAudio_;
    queue.push_back(std::make_shared<AudioPacket>(std::move(packet)));
    trimLocked();
}

bool ReplayBuffer::saveTo(const std::filesystem::path& outputPath, const VideoSettings& videoSettings) {
    std::vector<std::shared_ptr<const EncodedPacket>> video;
    std::vector<std::shared_ptr<const AudioPacket>> systemAudio;
    std::vector<std::shared_ptr<const AudioPacket>> microphoneAudio;
    int64_t lastSaveEndPts100ns = 0;
    {
        std::scoped_lock lock(mutex_);
        video.assign(video_.begin(), video_.end());
        systemAudio.assign(systemAudio_.begin(), systemAudio_.end());
        microphoneAudio.assign(microphoneAudio_.begin(), microphoneAudio_.end());
        lastSaveEndPts100ns = lastSaveEndPts100ns_;
    }

    if (video.empty()) {
        Logger::instance().warning(L"replay", L"Replay requested but video buffer is empty");
        return false;
    }

    const int64_t newestVideoEndPts =
        video.back()->pts100ns + std::max<int64_t>(video.back()->duration100ns, 0);
    constexpr int64_t kConsecutiveSaveGrace100ns = 3 * kHundredNanosecondsPerSecond;
    const bool saveSinceLastClip =
        lastSaveEndPts100ns > 0 &&
        lastSaveEndPts100ns <= newestVideoEndPts &&
        lastSaveEndPts100ns > video.front()->pts100ns;

    auto firstKeyFrame = video.end();
    if (saveSinceLastClip) {
        const int64_t desiredStartPts = lastSaveEndPts100ns - kConsecutiveSaveGrace100ns;
        for (auto it = video.begin(); it != video.end(); ++it) {
            if ((*it)->keyFrame && (*it)->pts100ns <= desiredStartPts) {
                firstKeyFrame = it;
            }
            if ((*it)->pts100ns > desiredStartPts) {
                break;
            }
        }
        if (firstKeyFrame == video.end()) {
            firstKeyFrame = std::find_if(video.begin(), video.end(), [desiredStartPts](const auto& packet) {
                return packet->keyFrame && packet->pts100ns >= desiredStartPts;
            });
        }
    } else {
        firstKeyFrame = std::find_if(video.begin(), video.end(), [](const auto& packet) {
            return packet->keyFrame;
        });
    }

    if (firstKeyFrame == video.end()) {
        firstKeyFrame = std::find_if(video.begin(), video.end(), [](const auto& packet) {
            return packet->keyFrame;
        });
    }

    if (firstKeyFrame == video.end()) {
        Logger::instance().warning(L"replay", L"Replay requested but buffer does not contain a keyframe yet");
        return false;
    }

    video.erase(video.begin(), firstKeyFrame);
    const int64_t replayStartPts = video.front()->pts100ns;
    const int64_t replayEndPts = newestVideoEndPts;
    if (!outputPath.parent_path().empty()) {
        std::error_code error;
        std::filesystem::create_directories(outputPath.parent_path(), error);
        if (error) {
            Logger::instance().error(L"replay", L"Could not create replay output directory: " + outputPath.parent_path().wstring());
            return false;
        }
    }

    const auto tempDirectory = outputPath.parent_path() / L".backtrack_tmp" / outputPath.stem();
    std::error_code tempError;
    std::filesystem::create_directories(tempDirectory, tempError);
    if (tempError) {
        Logger::instance().error(L"replay", L"Could not create replay temporary directory: " + tempDirectory.wstring());
        return false;
    }
    ScopedDirectoryCleanup cleanup(tempDirectory);

    const auto videoPath = tempDirectory / (videoSettings.codec == VideoCodec::H264 ? L"replay.h264" : L"replay.hevc");

    std::ofstream videoStream(videoPath, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!videoStream.is_open()) {
        Logger::instance().error(L"replay", L"Could not open replay video stream: " + videoPath.wstring());
        return false;
    }
    std::vector<MuxedInputs::VideoSample> videoSamples;
    uint64_t videoOffset = 0;
    for (const auto& packet : video) {
        videoStream.write(reinterpret_cast<const char*>(packet->bytes.data()), static_cast<std::streamsize>(packet->bytes.size()));
        videoSamples.push_back(MuxedInputs::VideoSample{
            videoOffset,
            static_cast<uint32_t>(packet->bytes.size()),
            packet->pts100ns,
            packet->duration100ns,
            packet->keyFrame});
        videoOffset += packet->bytes.size();
    }
    videoStream.close();
    if (!videoStream) {
        Logger::instance().error(L"replay", L"Could not write replay video stream: " + videoPath.wstring());
        return false;
    }

    WavWriter systemWriter;
    WavWriter microphoneWriter;
    std::filesystem::path systemPath;
    std::filesystem::path microphonePath;

    if (!systemAudio.empty()) {
        systemPath = tempDirectory / L"replay_system.wav";
        writeTimelineAudio(systemWriter, systemAudio, systemPath, replayStartPts, replayEndPts);
        if (systemWriter.dataBytes() == 0) {
            systemPath.clear();
            Logger::instance().warning(L"replay", L"Replay system audio stream had no samples on the saved timeline");
        }
    }

    if (!microphoneAudio.empty()) {
        microphonePath = tempDirectory / L"replay_microphone.wav";
        writeTimelineAudio(microphoneWriter, microphoneAudio, microphonePath, replayStartPts, replayEndPts);
        if (microphoneWriter.dataBytes() == 0) {
            microphonePath.clear();
            Logger::instance().warning(L"replay", L"Replay microphone audio stream had no samples on the saved timeline");
        }
    }

    MuxedInputs inputs;
    inputs.videoPath = videoPath;
    inputs.systemAudioPath = systemPath;
    inputs.microphoneAudioPath = microphonePath;
    inputs.videoSamples = std::move(videoSamples);
    inputs.codec = videoSettings.codec;
    inputs.fps = videoSettings.fps;
    inputs.width = videoSettings.width;
    inputs.height = videoSettings.height;
    inputs.bitrateKbps = videoSettings.bitrateKbps;
    inputs.resolutionMode = videoSettings.resolutionMode;

    const bool ok = Mp4Muxer::muxToMp4(inputs, outputPath);
    if (ok) {
        std::scoped_lock lock(mutex_);
        lastSaveEndPts100ns_ = newestVideoEndPts;
        Logger::instance().info(L"replay", L"Replay saved: " + outputPath.wstring());
    } else {
        std::error_code removeError;
        std::filesystem::remove(outputPath, removeError);
        Logger::instance().warning(L"replay", L"Replay mux failed; removed temporary replay files for " + outputPath.wstring());
    }
    return ok;
}

void ReplayBuffer::clear() {
    std::scoped_lock lock(mutex_);
    video_.clear();
    systemAudio_.clear();
    microphoneAudio_.clear();
    videoKeyFrames_ = 0;
    lastSaveEndPts100ns_ = 0;
}

size_t ReplayBuffer::videoPacketCount() const {
    std::scoped_lock lock(mutex_);
    return video_.size();
}

size_t ReplayBuffer::videoKeyFrameCount() const {
    std::scoped_lock lock(mutex_);
    return videoKeyFrames_;
}

void ReplayBuffer::trimLocked() {
    if (video_.empty()) {
        systemAudio_.clear();
        microphoneAudio_.clear();
        videoKeyFrames_ = 0;
        lastSaveEndPts100ns_ = 0;
        return;
    }

    const int64_t newestPts = video_.back()->pts100ns;
    const int64_t oldestAllowed = newestPts - secondsTo100ns(settings_.seconds);

    auto trimPacketQueue = [&](auto& queue) {
        while (!queue.empty() && audioPacketEndPts100ns(*queue.front()) <= oldestAllowed) {
            queue.pop_front();
        }
    };

    auto popVideoFront = [&]() {
        if (!video_.empty() && video_.front()->keyFrame && videoKeyFrames_ > 0) {
            --videoKeyFrames_;
        }
        video_.pop_front();
    };

    while (!video_.empty() && video_.front()->pts100ns < oldestAllowed) {
        popVideoFront();
    }
    auto firstBufferedKeyFrame = std::find_if(video_.begin(), video_.end(), [](const auto& packet) {
        return packet->keyFrame;
    });
    if (firstBufferedKeyFrame != video_.end()) {
        const auto packetsBeforeKeyFrame = static_cast<size_t>(std::distance(video_.begin(), firstBufferedKeyFrame));
        for (size_t index = 0; index < packetsBeforeKeyFrame; ++index) {
            popVideoFront();
        }
    }
    trimPacketQueue(systemAudio_);
    trimPacketQueue(microphoneAudio_);
    if (!video_.empty()) {
        const int64_t videoStartPts = video_.front()->pts100ns;
        auto trimAudioPacketQueueBeforeVideo = [videoStartPts](auto& queue) {
            while (!queue.empty() && audioPacketEndPts100ns(*queue.front()) <= videoStartPts) {
                queue.pop_front();
            }
        };
        trimAudioPacketQueueBeforeVideo(systemAudio_);
        trimAudioPacketQueueBeforeVideo(microphoneAudio_);
    }
}

} // namespace backtrack
