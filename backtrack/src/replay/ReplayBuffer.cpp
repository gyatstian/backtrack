#include "replay/ReplayBuffer.h"

#include "audio/AudioConvert.h"
#include "core/Logger.h"
#include "encoder/BitstreamUtils.h"
#include "mux/Mp4Muxer.h"
#include "mux/RecoveryStore.h"
#include "mux/WavWriter.h"

#include <mmreg.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <limits>
#include <string_view>
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

    // Disarms the guard without deleting: used when a failed save must keep
    // its elementary streams and recovery manifest on disk for later recovery.
    void release() {
        path_.clear();
    }

private:
    std::filesystem::path path_;
};

// Writes the buffered packets onto one shared-timeline WAV. Returns false
// when a write to disk failed (typically disk full); the caller must fail the
// save instead of muxing a silently truncated audio track.
bool writeTimelineAudio(
    WavWriter& writer,
    const std::vector<std::shared_ptr<const AudioPacket>>& audio,
    const std::filesystem::path& path,
    int64_t startPts100ns,
    int64_t endPts100ns) {
    if (audio.empty() || endPts100ns <= startPts100ns) {
        return true;
    }

    const auto formatPacket = std::find_if(audio.begin(), audio.end(), [](const auto& packet) {
        return packet->format && packet->frameCount > 0 && !packet->bytes.empty();
    });
    if (formatPacket == audio.end()) {
        return true;
    }
    if (!writer.open(path, *(*formatPacket)->format)) {
        // Matches the recording path: a track whose stream never opened
        // degrades to video-only. Mid-stream WRITE failures below are the
        // ones that must fail the save.
        return true;
    }

    // Packets are canonicalized at ingest, so every buffered packet must match
    // the format the WAV was opened with. If one ever does not (a producer
    // bypassing canonicalization), copying its raw bytes would append
    // wrong-rate samples and skew the timeline: drop it and let the PTS
    // gap-fill below cover the hole with silence.
    const AudioFormatInfo streamFormat = parseAudioFormat(*(*formatPacket)->format);
    auto packetFormatMatches = [&streamFormat](const AudioPacket& packet) {
        if (!packet.format) {
            return false;
        }
        const AudioFormatInfo packetFormat = parseAudioFormat(*packet.format);
        if (packetFormat == streamFormat) {
            return true;
        }
        static std::atomic<bool> warnedMismatch{false};
        if (!warnedMismatch.exchange(true)) {
            Logger::instance().warning(L"replay",
                L"Dropping replay audio whose format no longer matches the saved stream");
        }
        return false;
    };

    const uint32_t samplesPerSec = writer.samplesPerSec();
    const uint16_t blockAlign = writer.blockAlign();
    if (samplesPerSec == 0 || blockAlign == 0) {
        writer.close();
        return false;
    }

    int64_t nextPts100ns = startPts100ns;
    for (const auto& packet : audio) {
        if (packet->frameCount == 0 || packet->bytes.empty()) {
            continue;
        }
        if (!packetFormatMatches(*packet)) {
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
            const bool silenceWritten = writer.writeSilenceFrames(gapFrames);
            nextPts100ns += static_cast<int64_t>((gapFrames * kHundredNanosecondsPerSecond) / samplesPerSec);
            if (!silenceWritten) {
                return false;
            }
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
        if (!writer.write(packet->bytes.data() + byteOffset, bytesToWrite)) {
            return false;
        }
        const uint64_t framesWritten = bytesToWrite / blockAlign;
        nextPts100ns += static_cast<int64_t>((framesWritten * kHundredNanosecondsPerSecond) / samplesPerSec);
        if (nextPts100ns >= endPts100ns) {
            break;
        }
    }
    writer.close();
    return true;
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
        codecHeaderBytes_.clear();
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
    // Capture the codec parameter sets from the first keyframe that carried
    // them (typically the session-start IDR) so later saves can patch a cut
    // point whose start packet lacks decodable headers.
    if (packet.keyFrame && codecHeaderBytes_.empty()) {
        std::vector<uint8_t> headers;
        if (extractParameterSets(packet.codec, packet.bytes, headers)) {
            codecHeaderBytes_ = std::move(headers);
        }
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

bool ReplayBuffer::saveTo(
    const std::filesystem::path& outputPath,
    const VideoSettings& videoSettings,
    std::wstring* errorDetail) {
    // Specific, user-presentable reason for every failure path; the UI shows
    // this instead of a generic "could not be saved" message.
    auto fail = [errorDetail](std::wstring_view reason) {
        if (errorDetail) {
            errorDetail->assign(reason);
        }
        Logger::instance().warning(L"replay", std::wstring(reason));
        return false;
    };

    std::vector<std::shared_ptr<const EncodedPacket>> video;
    std::vector<std::shared_ptr<const AudioPacket>> systemAudio;
    std::vector<std::shared_ptr<const AudioPacket>> microphoneAudio;
    int64_t lastSaveEndPts100ns = 0;
    std::vector<uint8_t> codecHeaderBytes;
    {
        std::scoped_lock lock(mutex_);
        video.assign(video_.begin(), video_.end());
        systemAudio.assign(systemAudio_.begin(), systemAudio_.end());
        microphoneAudio.assign(microphoneAudio_.begin(), microphoneAudio_.end());
        lastSaveEndPts100ns = lastSaveEndPts100ns_;
        codecHeaderBytes = codecHeaderBytes_;
    }

    if (video.empty()) {
        return fail(L"replay video buffer is empty");
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
        return fail(L"no keyframe buffered yet");
    }

    video.erase(video.begin(), firstKeyFrame);
    const int64_t replayStartPts = video.front()->pts100ns;
    const int64_t replayEndPts = newestVideoEndPts;
    if (!outputPath.parent_path().empty()) {
        std::error_code error;
        std::filesystem::create_directories(outputPath.parent_path(), error);
        if (error) {
            return fail(L"could not create output folder " + outputPath.parent_path().wstring() +
                        L" (" + utf8ToWide(error.message()) + L")");
        }
    }

    const auto tempDirectory = outputPath.parent_path() / L".backtrack_tmp" / outputPath.stem();
    std::error_code tempError;
    std::filesystem::create_directories(tempDirectory, tempError);
    if (tempError) {
        return fail(L"could not create replay temporary folder " + tempDirectory.wstring() +
                    L" (" + utf8ToWide(tempError.message()) + L")");
    }
    ScopedDirectoryCleanup cleanup(tempDirectory);

    // A recovery manifest left here belongs to a previous failed save whose
    // elementary streams this attempt is about to truncate. Remove it before
    // opening the streams so a crash mid-attempt can never leave a manifest
    // describing bytes the streams no longer contain; this attempt publishes
    // a fresh manifest (matching the new streams) right before muxing.
    const std::wstring staleManifestNames[] = {
        std::wstring(RecoveryStore::kRecoveryManifestName),
        std::wstring(RecoveryStore::kRecoveryManifestName) + L".tmp"};
    for (const auto& staleName : staleManifestNames) {
        std::error_code removeError;
        std::filesystem::remove(tempDirectory / staleName, removeError);
    }

    const auto videoPath = tempDirectory / (videoSettings.codec == VideoCodec::H264 ? L"replay.h264" : L"replay.hevc");

    std::ofstream videoStream(videoPath, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!videoStream.is_open()) {
        return fail(L"could not open temporary replay video stream " + videoPath.wstring());
    }
    // The clip starts at an IDR, but a driver that ignored header insertion
    // may have produced a keyframe without SPS/PPS/VPS, which no decoder can
    // bootstrap. Merge the cached parameter sets into the FIRST sample (the
    // MP4 sample table only exposes bytes covered by samples) so the clip is
    // decodable from its first frame.
    const bool firstSampleHasHeaders = video.empty() ||
        bitstreamContainsParameterSets(videoSettings.codec, video.front()->bytes);
    if (!firstSampleHasHeaders && codecHeaderBytes.empty()) {
        Logger::instance().warning(L"replay",
            L"Replay starts at a keyframe without codec headers and no cached headers are available");
    }
    const uint64_t prependedHeaderBytes =
        !firstSampleHasHeaders && !codecHeaderBytes.empty() ? codecHeaderBytes.size() : 0;
    if (prependedHeaderBytes > 0) {
        videoStream.write(
            reinterpret_cast<const char*>(codecHeaderBytes.data()),
            static_cast<std::streamsize>(codecHeaderBytes.size()));
    }
    std::vector<MuxedInputs::VideoSample> videoSamples;
    uint64_t videoOffset = 0;
    for (size_t index = 0; index < video.size(); ++index) {
        const auto& packet = *video[index];
        // A decode timestamp is used only when explicitly flagged valid:
        // values can legitimately be zero or negative, so no numeric sentinel
        // works. When the flag is missing (or the DTS regressed past the PTS,
        // which the encoders never produce), presentation time stands in.
        const bool hasDts = packet.hasDts && packet.dts100ns <= packet.pts100ns;
        const uint64_t sampleBytes =
            packet.bytes.size() + (index == 0 ? prependedHeaderBytes : 0);
        videoSamples.push_back(MuxedInputs::VideoSample{
            videoOffset,
            static_cast<uint32_t>(std::min<uint64_t>(sampleBytes, std::numeric_limits<uint32_t>::max())),
            packet.pts100ns,
            hasDts ? packet.dts100ns : packet.pts100ns,
            hasDts,
            packet.duration100ns,
            packet.keyFrame});
        videoStream.write(reinterpret_cast<const char*>(packet.bytes.data()), static_cast<std::streamsize>(packet.bytes.size()));
        videoOffset += sampleBytes;
    }
    videoStream.close();
    if (!videoStream) {
        // Most common cause of a mid-stream write failure is a full disk.
        return fail(L"could not write the temporary replay video stream " + videoPath.wstring() +
                    L" (disk full?)");
    }

    WavWriter systemWriter;
    WavWriter microphoneWriter;
    std::filesystem::path systemPath;
    std::filesystem::path microphonePath;

    if (!systemAudio.empty()) {
        systemPath = tempDirectory / L"replay_system.wav";
        if (!writeTimelineAudio(systemWriter, systemAudio, systemPath, replayStartPts, replayEndPts)) {
            // A failed WAV write (typically disk full) must fail the save;
            // muxing would publish a silently truncated audio track.
            return fail(L"could not write the temporary replay audio stream " + systemPath.wstring() +
                        L" (disk full?)");
        }
        if (systemWriter.dataBytes() == 0) {
            systemPath.clear();
            Logger::instance().warning(L"replay", L"Replay system audio stream had no samples on the saved timeline");
        }
    }

    if (!microphoneAudio.empty()) {
        microphonePath = tempDirectory / L"replay_microphone.wav";
        if (!writeTimelineAudio(microphoneWriter, microphoneAudio, microphonePath, replayStartPts, replayEndPts)) {
            return fail(L"could not write the temporary replay audio stream " + microphonePath.wstring() +
                        L" (disk full?)");
        }
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

    // Mirror the recording finalize path: persist the recovery manifest (and
    // readme) before muxing, so a failed save keeps the elementary streams
    // recoverable via "Recover Failed" or the startup sweep instead of
    // destroying them. The replay temp layout matches what
    // recoverLatestFailedRecording scans, so recovery needs no special case.
    const bool manifestWritten = RecoveryStore::writeManifest(inputs, tempDirectory);
    RecoveryStore::writeReadme(inputs, tempDirectory);

    std::wstring muxErrorDetail;
    // Same retry wrapper the recording finalize uses: a transient Media
    // Foundation init/finalize failure should not fail a save that one retry
    // would pass.
    const bool ok = RecoveryStore::muxToMp4WithRetry(inputs, outputPath, &muxErrorDetail);
    if (ok) {
        cleanup.cleanup();
        std::scoped_lock lock(mutex_);
        lastSaveEndPts100ns_ = newestVideoEndPts;
        Logger::instance().info(L"replay", L"Replay saved: " + outputPath.wstring());
    } else {
        // Only an empty output-name reservation may be removed; a real file
        // that appeared at the target in the meantime must survive.
        removeEmptyOutputReservation(outputPath);
        if (manifestWritten) {
            // The manifest exactly describes the streams on disk: keep the
            // directory so the clip stays recoverable.
            cleanup.release();
            if (errorDetail) {
                *errorDetail = RecoveryStore::preservedRecordingDetail(
                    outputPath,
                    tempDirectory,
                    inputs,
                    muxErrorDetail.empty() ? L"MP4 muxing failed; see log for details" : muxErrorDetail);
            }
            Logger::instance().warning(L"replay",
                L"Replay mux failed; preserved elementary streams for recovery in " + tempDirectory.wstring());
        } else {
            // The manifest could not be published (typically a full disk): the
            // directory holds no trustworthy recovery data, and a stale one
            // would misdescribe these streams. Remove it.
            cleanup.cleanup();
            if (errorDetail) {
                *errorDetail = muxErrorDetail.empty() ? L"MP4 muxing failed; see log for details" : muxErrorDetail;
            }
            Logger::instance().warning(L"replay",
                L"Replay mux failed and no recovery manifest could be written; removed temporary replay files for " +
                outputPath.wstring());
        }
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
    codecHeaderBytes_.clear();
}

size_t ReplayBuffer::videoPacketCount() const {
    std::scoped_lock lock(mutex_);
    return video_.size();
}

size_t ReplayBuffer::videoKeyFrameCount() const {
    std::scoped_lock lock(mutex_);
    return videoKeyFrames_;
}

int64_t ReplayBuffer::videoSpan100ns() const {
    std::scoped_lock lock(mutex_);
    if (video_.empty()) {
        return 0;
    }
    const auto firstKeyFrame = std::find_if(video_.begin(), video_.end(), [](const auto& packet) {
        return packet->keyFrame;
    });
    if (firstKeyFrame == video_.end()) {
        return 0;
    }
    const int64_t newestVideoEndPts =
        video_.back()->pts100ns + std::max<int64_t>(video_.back()->duration100ns, 0);
    return std::max<int64_t>(0, newestVideoEndPts - (*firstKeyFrame)->pts100ns);
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
