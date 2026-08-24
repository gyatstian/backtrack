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
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
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

// Below this size the ring feature is pointless (a single GOP would not fit);
// RecorderController clamps the requested budget well above it.
constexpr uint64_t kMinRingCapacityBytes = 8ull * 1024 * 1024;

// Explicit-offset synchronous-completion transfer on an overlapped ring
// handle. The ring file carries no per-handle file position, so a save thread
// reading while the encoder thread writes cannot tear seek/read or
// seek/write pairs - every operation names its own offset. Returns false with
// GetLastError() set on failure.
bool transferRing(HANDLE file, bool write, uint64_t offset, uint8_t* data, uint32_t size) {
    uint32_t done = 0;
    while (done < size) {
        OVERLAPPED overlapped{};
        const uint64_t absolute = offset + done;
        overlapped.Offset = static_cast<DWORD>(absolute & 0xFFFFFFFFu);
        overlapped.OffsetHigh = static_cast<DWORD>(absolute >> 32);
        const DWORD chunkBytes = static_cast<DWORD>(std::min<uint64_t>(size - done, 16ull * 1024 * 1024));
        DWORD moved = 0;
        const BOOL ok = write
            ? WriteFile(file, data + done, chunkBytes, &moved, &overlapped)
            : ReadFile(file, data + done, chunkBytes, &moved, &overlapped);
        if (!ok) {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) {
                return false;
            }
            if (!GetOverlappedResult(file, &overlapped, &moved, TRUE)) {
                return false;
            }
        }
        if (moved == 0) {
            SetLastError(write ? ERROR_WRITE_FAULT : ERROR_READ_FAULT);
            return false;
        }
        done += moved;
    }
    return true;
}

} // namespace

ReplayBuffer::~ReplayBuffer() {
    std::scoped_lock lock(mutex_);
    closeRingNowLocked();
}

void ReplayBuffer::configure(const ReplaySettings& settings,
                             const std::filesystem::path& storageDirectory,
                             uint64_t capacityBytes) {
    std::scoped_lock lock(mutex_);
    const std::filesystem::path previousDirectory = ringDirectory_;
    settings_ = settings;
    ringDirectory_ = storageDirectory;

    if (!settings_.enabled) {
        video_.clear();
        systemAudio_.clear();
        microphoneAudio_.clear();
        clearBufferedVideoLocked();
        lastSaveEndPts100ns_ = 0;
        codecHeaderBytes_.clear();
        closeRingLocked();
        videoBackend_ = VideoBackend::Ram;
        return;
    }

    const bool wantDisk = settings_.diskBackedVideo &&
                          !storageDirectory.empty() &&
                          capacityBytes >= kMinRingCapacityBytes;
    const bool usingDisk = videoBackend_ == VideoBackend::Disk;
    const bool directoryChanged = wantDisk && storageDirectory != previousDirectory;

    if (!wantDisk && usingDisk) {
        Logger::instance().info(L"replay", L"Replay video buffering switched back to memory");
        clearBufferedVideoLocked();
        closeRingLocked();
        videoBackend_ = VideoBackend::Ram;
    } else if (wantDisk &&
               (!usingDisk || directoryChanged || capacityBytes != ringCapacityBytes_)) {
        // A backend/location/capacity switch makes buffered video unreadable
        // in the new configuration; drop it exactly like a codec change does.
        clearBufferedVideoLocked();
        closeRingLocked();
        ringCapacityBytes_ = capacityBytes;
        if (openRingLocked()) {
            videoBackend_ = VideoBackend::Disk;
            Logger::instance().info(L"replay",
                L"Replay video buffering moved to disk ring " + ringPath_.wstring() +
                L" (" + std::to_wstring(capacityBytes / (1024ull * 1024ull)) + L" MiB)");
        } else {
            videoBackend_ = VideoBackend::Ram;
            Logger::instance().warning(L"replay",
                L"Disk-backed replay buffering unavailable; falling back to memory");
        }
    }

    trimLocked();
}

void ReplayBuffer::pushVideo(EncodedPacket packet) {
    if (packet.bytes.empty()) {
        return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
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

    if (videoBackend_ == VideoBackend::Disk) {
        const auto size = static_cast<uint32_t>(packet.bytes.size());
        if (!ensureRingSpaceLocked(lock, size)) {
            // Cannot ever fit (packet larger than the whole ring): drop it.
            return;
        }
        if (appendRingLocked(packet)) {
            return;
        }
        // Ring storage failed (typically disk full): discard the buffered
        // window and continue in memory so recording never stalls.
        failOverToRamLocked();
    }

    if (packet.keyFrame) {
        ++videoKeyFrames_;
    }
    video_.push_back(std::make_shared<EncodedPacket>(std::move(packet)));
    trimLocked();
}

bool ReplayBuffer::extendLastVideoDuration(int64_t expectedPts100ns, int64_t duration100ns) {
    std::scoped_lock lock(mutex_);
    if (!settings_.enabled || duration100ns <= 0) {
        return false;
    }
    if (videoBackend_ == VideoBackend::Disk) {
        if (diskVideo_.empty() || diskVideo_.back().meta.pts100ns != expectedPts100ns) {
            return false;
        }
        diskVideo_.back().meta.duration100ns =
            std::max(diskVideo_.back().meta.duration100ns, duration100ns);
        return true;
    }
    if (video_.empty() || video_.back()->pts100ns != expectedPts100ns) {
        return false;
    }

    // Mutate in place: the packet payload is never touched after pushVideo and
    // every reader of duration100ns (saveTo's meta snapshot, span accounting,
    // trim) goes through mutex_, so copying the whole packet - keyframes are
    // megabytes - per emitted interval bought nothing but allocation spikes.
    EncodedPacket& packet = *video_.back();
    packet.duration100ns = std::max(packet.duration100ns, duration100ns);
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

    // Backend-neutral view of one buffered video packet. In RAM mode `ram`
    // owns the bytes; in disk mode the extents address them inside the ring.
    struct SnapshottedVideo {
        std::shared_ptr<const EncodedPacket> ram;
        VideoPacketMeta meta;
        RingExtent first;
        RingExtent second;
    };

    std::vector<SnapshottedVideo> video;
    std::vector<std::shared_ptr<const AudioPacket>> systemAudio;
    std::vector<std::shared_ptr<const AudioPacket>> microphoneAudio;
    int64_t lastSaveEndPts100ns = 0;
    std::vector<uint8_t> codecHeaderBytes;
    bool diskMode = false;
    {
        std::scoped_lock lock(mutex_);
        diskMode = videoBackend_ == VideoBackend::Disk;
        if (diskMode) {
            video.reserve(diskVideo_.size());
            for (const auto& entry : diskVideo_) {
                SnapshottedVideo snapshot;
                snapshot.meta = entry.meta;
                snapshot.first = entry.first;
                snapshot.second = entry.second;
                video.push_back(std::move(snapshot));
            }
        } else {
            video.reserve(video_.size());
            for (const auto& packet : video_) {
                SnapshottedVideo snapshot;
                snapshot.ram = packet;
                snapshot.meta.pts100ns = packet->pts100ns;
                snapshot.meta.dts100ns = packet->dts100ns;
                snapshot.meta.hasDts = packet->hasDts;
                snapshot.meta.duration100ns = packet->duration100ns;
                snapshot.meta.keyFrame = packet->keyFrame;
                video.push_back(std::move(snapshot));
            }
        }
        systemAudio.assign(systemAudio_.begin(), systemAudio_.end());
        microphoneAudio.assign(microphoneAudio_.begin(), microphoneAudio_.end());
        lastSaveEndPts100ns = lastSaveEndPts100ns_;
        codecHeaderBytes = codecHeaderBytes_;
        // While the pin holds, ring pushes that need space wait instead of
        // trimming: trimming could evict packets this save has not read yet.
        if (diskMode) {
            ++savePinsRefs_;
        }
    }
    // Releases the pin on every path (early returns included) once the video
    // bytes have been consumed; also performs a ring close that the pin had
    // deferred, then resumes age-based trimming of the disk backend.
    struct PinRelease {
        ReplayBuffer* self;
        ~PinRelease() {
            {
                std::scoped_lock lock(self->mutex_);
                --self->savePinsRefs_;
                if (self->savePinsRefs_ > 0) {
                    return; // Another save still holds the pin.
                }
                if (self->ringPendingClose_) {
                    self->closeRingNowLocked();
                } else if (self->videoBackend_ == VideoBackend::Disk) {
                    self->trimLocked();
                }
            }
            self->ringSpace_.notify_all();
        }
    };
    std::optional<PinRelease> pinRelease;
    if (diskMode) {
        pinRelease.emplace(this);
    }

    if (video.empty()) {
        return fail(L"replay video buffer is empty");
    }

    const int64_t newestVideoEndPts =
        video.back().meta.pts100ns + std::max<int64_t>(video.back().meta.duration100ns, 0);
    constexpr int64_t kConsecutiveSaveGrace100ns = 3 * kHundredNanosecondsPerSecond;
    const bool saveSinceLastClip =
        lastSaveEndPts100ns > 0 &&
        lastSaveEndPts100ns <= newestVideoEndPts &&
        lastSaveEndPts100ns > video.front().meta.pts100ns;

    auto firstKeyFrame = video.end();
    if (saveSinceLastClip) {
        const int64_t desiredStartPts = lastSaveEndPts100ns - kConsecutiveSaveGrace100ns;
        for (auto it = video.begin(); it != video.end(); ++it) {
            if (it->meta.keyFrame && it->meta.pts100ns <= desiredStartPts) {
                firstKeyFrame = it;
            }
            if (it->meta.pts100ns > desiredStartPts) {
                break;
            }
        }
        if (firstKeyFrame == video.end()) {
            firstKeyFrame = std::find_if(video.begin(), video.end(), [desiredStartPts](const SnapshottedVideo& snapshot) {
                return snapshot.meta.keyFrame && snapshot.meta.pts100ns >= desiredStartPts;
            });
        }
    } else {
        firstKeyFrame = std::find_if(video.begin(), video.end(), [](const SnapshottedVideo& snapshot) {
            return snapshot.meta.keyFrame;
        });
    }

    if (firstKeyFrame == video.end()) {
        firstKeyFrame = std::find_if(video.begin(), video.end(), [](const SnapshottedVideo& snapshot) {
            return snapshot.meta.keyFrame;
        });
    }

    if (firstKeyFrame == video.end()) {
        return fail(L"no keyframe buffered yet");
    }

    video.erase(video.begin(), firstKeyFrame);
    const int64_t replayStartPts = video.front().meta.pts100ns;
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

    // Copies one packet's bytes from whichever backend holds them into the
    // elementary stream. Ring reads happen without holding mutex_: the pin
    // guarantees the referenced extents cannot be overwritten while they are
    // being consumed.
    std::vector<uint8_t> ringScratch;
    auto appendSampleBytes = [&](const SnapshottedVideo& snapshot) -> bool {
        if (snapshot.ram) {
            videoStream.write(
                reinterpret_cast<const char*>(snapshot.ram->bytes.data()),
                static_cast<std::streamsize>(snapshot.ram->bytes.size()));
            return static_cast<bool>(videoStream);
        }
        const uint32_t totalBytes = snapshot.first.byteCount + snapshot.second.byteCount;
        ringScratch.resize(totalBytes);
        if (snapshot.first.byteCount != 0 &&
            !readRingLocked(snapshot.first.offset, ringScratch.data(), snapshot.first.byteCount)) {
            return false;
        }
        if (snapshot.second.byteCount != 0 &&
            !readRingLocked(
                snapshot.second.offset,
                ringScratch.data() + snapshot.first.byteCount,
                snapshot.second.byteCount)) {
            return false;
        }
        videoStream.write(reinterpret_cast<const char*>(ringScratch.data()), static_cast<std::streamsize>(totalBytes));
        return static_cast<bool>(videoStream);
    };

    // The clip starts at an IDR, but a driver that ignored header insertion
    // may have produced a keyframe without SPS/PPS/VPS, which no decoder can
    // bootstrap. Merge the cached parameter sets into the FIRST sample (the
    // MP4 sample table only exposes bytes covered by samples) so the clip is
    // decodable from its first frame.
    bool firstSampleHasHeaders = video.empty();
    if (!firstSampleHasHeaders && diskMode) {
        // Peek the first packet's bytes for the parameter-set probe; the dump
        // loop below re-reads them through the same pinned extents.
        const SnapshottedVideo& first = video.front();
        const uint32_t totalBytes = first.first.byteCount + first.second.byteCount;
        ringScratch.resize(totalBytes);
        firstSampleHasHeaders =
            readRingLocked(first.first.offset, ringScratch.data(), first.first.byteCount) &&
            (first.second.byteCount == 0 ||
             readRingLocked(
                 first.second.offset,
                 ringScratch.data() + first.first.byteCount,
                 first.second.byteCount)) &&
            bitstreamContainsParameterSets(videoSettings.codec, ringScratch);
        if (!firstSampleHasHeaders) {
            // Fall through to the cached-header logic; the probe result only
            // decides whether bytes must be prepended.
            firstSampleHasHeaders = false;
        }
    } else if (!video.empty()) {
        firstSampleHasHeaders = bitstreamContainsParameterSets(videoSettings.codec, video.front().ram->bytes);
    }
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
        const SnapshottedVideo& snapshot = video[index];
        // A decode timestamp is used only when explicitly flagged valid:
        // values can legitimately be zero or negative, so no numeric sentinel
        // works. When the flag is missing (or the DTS regressed past the PTS,
        // which the encoders never produce), presentation time stands in.
        const bool hasDts = snapshot.meta.hasDts && snapshot.meta.dts100ns <= snapshot.meta.pts100ns;
        const uint32_t payloadBytes = snapshot.ram
            ? static_cast<uint32_t>(snapshot.ram->bytes.size())
            : snapshot.first.byteCount + snapshot.second.byteCount;
        const uint32_t sampleBytes =
            payloadBytes + static_cast<uint32_t>(index == 0 ? prependedHeaderBytes : 0);
        if (!appendSampleBytes(snapshot)) {
            break;
        }
        videoSamples.push_back(MuxedInputs::VideoSample{
            videoOffset,
            sampleBytes,
            snapshot.meta.pts100ns,
            hasDts ? snapshot.meta.dts100ns : snapshot.meta.pts100ns,
            hasDts,
            snapshot.meta.duration100ns,
            snapshot.meta.keyFrame});
        videoOffset += sampleBytes;
    }
    videoStream.close();
    pinRelease.reset(); // All ring bytes consumed; pushes may reclaim space.

    if (!videoStream) {
        if (diskMode && videoSamples.size() != video.size()) {
            return fail(L"could not read buffered replay video from the disk ring " +
                        ringPath_.wstring());
        }
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
    clearBufferedVideoLocked();
    lastSaveEndPts100ns_ = 0;
    codecHeaderBytes_.clear();
}

size_t ReplayBuffer::videoPacketCount() const {
    std::scoped_lock lock(mutex_);
    return videoBackend_ == VideoBackend::Disk ? diskVideo_.size() : video_.size();
}

size_t ReplayBuffer::videoKeyFrameCount() const {
    std::scoped_lock lock(mutex_);
    return videoBackend_ == VideoBackend::Disk ? diskVideoKeyFrames_ : videoKeyFrames_;
}

int64_t ReplayBuffer::videoSpan100ns() const {
    std::scoped_lock lock(mutex_);
    if (videoBackend_ == VideoBackend::Disk) {
        if (diskVideo_.empty()) {
            return 0;
        }
        const auto firstKeyFrame = std::find_if(diskVideo_.begin(), diskVideo_.end(), [](const DiskVideoPacket& packet) {
            return packet.meta.keyFrame;
        });
        if (firstKeyFrame == diskVideo_.end()) {
            return 0;
        }
        const DiskVideoPacket& newest = diskVideo_.back();
        const int64_t newestVideoEndPts =
            newest.meta.pts100ns + std::max<int64_t>(newest.meta.duration100ns, 0);
        return std::max<int64_t>(0, newestVideoEndPts - firstKeyFrame->meta.pts100ns);
    }

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
    if (videoBackend_ == VideoBackend::Disk) {
        // An active save pins the buffered refs: age-based eviction here
        // could drop packets the save has not read yet. Eviction resumes at
        // pin release (the release path calls trimLocked again).
        if (!savePinsRefs_ && !diskVideo_.empty()) {
            const int64_t newestPts = diskVideo_.back().meta.pts100ns;
            const int64_t oldestAllowed = newestPts - secondsTo100ns(settings_.seconds);

            auto popDiskFront = [&]() {
                const DiskVideoPacket& front = diskVideo_.front();
                ringLiveBytes_ -= front.first.byteCount + front.second.byteCount;
                if (front.meta.keyFrame && diskVideoKeyFrames_ > 0) {
                    --diskVideoKeyFrames_;
                }
                diskVideo_.pop_front();
            };

            while (!diskVideo_.empty() && diskVideo_.front().meta.pts100ns < oldestAllowed) {
                popDiskFront();
            }
            const auto firstBufferedKeyFrame = std::find_if(
                diskVideo_.begin(), diskVideo_.end(), [](const DiskVideoPacket& packet) {
                    return packet.meta.keyFrame;
                });
            if (firstBufferedKeyFrame != diskVideo_.end()) {
                const auto packetsBeforeKeyFrame =
                    static_cast<size_t>(std::distance(diskVideo_.begin(), firstBufferedKeyFrame));
                for (size_t index = 0; index < packetsBeforeKeyFrame; ++index) {
                    popDiskFront();
                }
            }
        }
        // Audio trimming below applies to both backends.
    } else {
        if (video_.empty()) {
            systemAudio_.clear();
            microphoneAudio_.clear();
            videoKeyFrames_ = 0;
            lastSaveEndPts100ns_ = 0;
            return;
        }

        const int64_t newestPts = video_.back()->pts100ns;
        const int64_t oldestAllowed = newestPts - secondsTo100ns(settings_.seconds);

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
    }

    auto trimPacketQueue = [&](auto& queue) {
        const int64_t referencePts = videoBackend_ == VideoBackend::Disk
            ? (diskVideo_.empty() ? 0 : diskVideo_.back().meta.pts100ns)
            : (video_.empty() ? 0 : video_.back()->pts100ns);
        const int64_t oldestAllowed = referencePts - secondsTo100ns(settings_.seconds);
        while (!queue.empty() && audioPacketEndPts100ns(*queue.front()) <= oldestAllowed) {
            queue.pop_front();
        }
    };

    if (videoBackend_ == VideoBackend::Ram && video_.empty()) {
        systemAudio_.clear();
        microphoneAudio_.clear();
        videoKeyFrames_ = 0;
        lastSaveEndPts100ns_ = 0;
        return;
    }
    if (videoBackend_ == VideoBackend::Disk && diskVideo_.empty()) {
        systemAudio_.clear();
        microphoneAudio_.clear();
        diskVideoKeyFrames_ = 0;
        lastSaveEndPts100ns_ = 0;
        return;
    }

    trimPacketQueue(systemAudio_);
    trimPacketQueue(microphoneAudio_);

    const int64_t videoStartPts = videoBackend_ == VideoBackend::Disk
        ? diskVideo_.front().meta.pts100ns
        : video_.front()->pts100ns;
    auto trimAudioPacketQueueBeforeVideo = [videoStartPts](auto& queue) {
        while (!queue.empty() && audioPacketEndPts100ns(*queue.front()) <= videoStartPts) {
            queue.pop_front();
        }
    };
    trimAudioPacketQueueBeforeVideo(systemAudio_);
    trimAudioPacketQueueBeforeVideo(microphoneAudio_);
}

void ReplayBuffer::clearBufferedVideoLocked() {
    video_.clear();
    diskVideo_.clear();
    videoKeyFrames_ = 0;
    diskVideoKeyFrames_ = 0;
    ringLiveBytes_ = 0;
    ringWriteOffset_ = 0;
}

bool ReplayBuffer::openRingLocked() {
    if (ringPendingClose_) {
        // A previous ring awaits its close. If a save is still reading it,
        // reopening now is impossible without leaking the old handle.
        if (savePinsRefs_) {
            Logger::instance().warning(L"replay",
                L"Cannot reopen the replay ring while a save is still reading the old one");
            return false;
        }
        closeRingNowLocked();
    }
    std::error_code error;
    const auto directory = ringDirectory_ / L".backtrack_ring";
    std::filesystem::create_directories(directory, error);
    if (error) {
        Logger::instance().warning(L"replay",
            L"Could not create replay ring folder " + directory.wstring() +
            L" (" + utf8ToWide(error.message()) + L")");
        return false;
    }
    ringPath_ = directory / L"replay_video.bin";
    ringFile_ = CreateFileW(
        ringPath_.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        // Overlapped: every transfer names its own offset, so save reads and
        // push writes never share a file position.
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (ringFile_ == INVALID_HANDLE_VALUE) {
        ringFile_ = nullptr;
        Logger::instance().warning(L"replay",
            L"Could not create replay ring file " + ringPath_.wstring() +
            L" (Win32 error " + std::to_wstring(GetLastError()) + L")");
        return false;
    }
    // Pre-size the file so the steady-state footprint is fixed from the
    // start. NTFS assigns real clusters lazily, so this costs no I/O beyond
    // metadata until packets actually occupy the ranges.
    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(ringCapacityBytes_);
    if ((!SetFilePointerEx(ringFile_, end, nullptr, FILE_BEGIN) || !SetEndOfFile(ringFile_))) {
        Logger::instance().warning(L"replay",
            L"Could not pre-size replay ring file " + ringPath_.wstring() +
            L" (Win32 error " + std::to_wstring(GetLastError()) + L"); it will grow as packets arrive");
    }
    ringWriteOffset_ = 0;
    ringLiveBytes_ = 0;
    return true;
}

void ReplayBuffer::closeRingLocked() noexcept {
    if (savePinsRefs_) {
        // A save may still be reading through the handle; defer the physical
        // close to pin release.
        ringPendingClose_ = true;
        return;
    }
    closeRingNowLocked();
}

void ReplayBuffer::closeRingNowLocked() noexcept {
    if (ringFile_) {
        CloseHandle(ringFile_);
        ringFile_ = nullptr;
    }
    if (!ringPath_.empty()) {
        std::error_code error;
        std::filesystem::remove(ringPath_, error);
        ringPath_.clear();
    }
    ringWriteOffset_ = 0;
    ringLiveBytes_ = 0;
    ringPendingClose_ = false;
}

bool ReplayBuffer::ensureRingSpaceLocked(std::unique_lock<std::mutex>& lock, uint32_t size) {
    const uint64_t capacity = ringCapacityBytes_;
    if (size > capacity) {
        Logger::instance().warning(L"replay",
            L"Replay ring packet of " + std::to_wstring(size) +
            L" bytes exceeds the entire ring capacity; dropping it");
        return false;
    }
    // Saturating free-space: a VBR overshoot can push ringLiveBytes_ past the
    // capacity; without the clamp the unsigned difference would wrap to ~2^64
    // and defeat every eviction/wait decision below.
    const uint64_t freeBytes = ringLiveBytes_ >= capacity ? 0 : capacity - ringLiveBytes_;
    if (freeBytes >= size) {
        return true;
    }
    while (true) {
        if (savePinsRefs_) {
            // A save is still consuming pinned refs; wait for it to finish
            // rather than evicting packets it may not have read yet.
            ringSpace_.wait(lock);
        } else {
            freeRingSpaceLocked(size);
        }
        const uint64_t availableNow =
            ringLiveBytes_ >= capacity ? 0 : capacity - ringLiveBytes_;
        if (availableNow >= size) {
            return true;
        }
        if (!savePinsRefs_ && diskVideo_.empty()) {
            // Everything evicted and still not enough cannot happen
            // (size <= capacity), but bail out defensively instead of spinning.
            Logger::instance().warning(L"replay", L"Replay ring could not free enough space; dropping a packet");
            return false;
        }
    }
}

bool ReplayBuffer::appendRingLocked(const EncodedPacket& packet) {
    if (!ringFile_ && !openRingLocked()) {
        return false;
    }
    const uint64_t capacity = ringCapacityBytes_;
    const uint32_t size = static_cast<uint32_t>(packet.bytes.size());

    // Explicit-offset overlapped writes: the handle carries no file position,
    // so a save reading concurrently can never observe a torn seek/write pair.
    auto writeAt = [this](uint64_t offset, const uint8_t* data, DWORD byteCount) -> bool {
        return transferRing(ringFile_, true, offset, const_cast<uint8_t*>(data), byteCount);
    };

    const uint64_t firstSize = std::min<uint64_t>(size, capacity - ringWriteOffset_);
    const uint64_t secondSize = size - firstSize;
    if (firstSize > 0 && !writeAt(ringWriteOffset_, packet.bytes.data(), static_cast<DWORD>(firstSize))) {
        Logger::instance().warning(L"replay",
            L"Replay ring write failed (Win32 error " + std::to_wstring(GetLastError()) + L")");
        return false;
    }
    if (secondSize > 0 && !writeAt(0, packet.bytes.data() + firstSize, static_cast<DWORD>(secondSize))) {
        Logger::instance().warning(L"replay",
            L"Replay ring wrap write failed (Win32 error " + std::to_wstring(GetLastError()) + L")");
        return false;
    }

    DiskVideoPacket entry;
    entry.meta.pts100ns = packet.pts100ns;
    entry.meta.dts100ns = packet.dts100ns;
    entry.meta.duration100ns = packet.duration100ns;
    entry.meta.hasDts = packet.hasDts;
    entry.meta.keyFrame = packet.keyFrame;
    entry.first = RingExtent{ringWriteOffset_, static_cast<uint32_t>(firstSize)};
    // secondSize == 0 marks a non-wrapping packet; its offset is unused.
    entry.second = RingExtent{0, static_cast<uint32_t>(secondSize)};
    ringWriteOffset_ = (ringWriteOffset_ + size) % capacity;
    ringLiveBytes_ += size;
    if (entry.meta.keyFrame) {
        ++diskVideoKeyFrames_;
    }
    diskVideo_.push_back(std::move(entry));
    trimLocked();
    return true;
}

bool ReplayBuffer::readRingLocked(uint64_t offset, uint8_t* destination, uint32_t size) const {
    if (!ringFile_ || size == 0) {
        return size == 0;
    }
    return transferRing(ringFile_, false, offset, destination, size);
}

void ReplayBuffer::freeRingSpaceLocked(uint64_t bytesNeeded) {
    // Saturating subtraction: see ensureRingSpaceLocked.
    const auto freeBytes = [this]() {
        return ringLiveBytes_ >= ringCapacityBytes_ ? 0 : ringCapacityBytes_ - ringLiveBytes_;
    };
    while (!diskVideo_.empty() && freeBytes() < bytesNeeded) {
        const DiskVideoPacket& front = diskVideo_.front();
        ringLiveBytes_ -= front.first.byteCount + front.second.byteCount;
        if (front.meta.keyFrame && diskVideoKeyFrames_ > 0) {
            --diskVideoKeyFrames_;
        }
        diskVideo_.pop_front();
        // Evicting the head can strand non-keyframe descendants; realign to
        // the next keyframe so the buffer stays independently decodable.
        const auto firstBufferedKeyFrame = std::find_if(
            diskVideo_.begin(), diskVideo_.end(), [](const DiskVideoPacket& packet) {
                return packet.meta.keyFrame;
            });
        if (firstBufferedKeyFrame != diskVideo_.end()) {
            const auto packetsBeforeKeyFrame =
                static_cast<size_t>(std::distance(diskVideo_.begin(), firstBufferedKeyFrame));
            for (size_t index = 0; index < packetsBeforeKeyFrame; ++index) {
                const DiskVideoPacket& stale = diskVideo_.front();
                ringLiveBytes_ -= stale.first.byteCount + stale.second.byteCount;
                if (stale.meta.keyFrame && diskVideoKeyFrames_ > 0) {
                    --diskVideoKeyFrames_;
                }
                diskVideo_.pop_front();
            }
        }
    }
}

void ReplayBuffer::failOverToRamLocked() {
    clearBufferedVideoLocked();
    closeRingLocked();
    videoBackend_ = VideoBackend::Ram;
    Logger::instance().warning(L"replay",
        L"Replay ring write failed; disk-backed buffering is disabled for this session and "
        L"video is buffered in memory again");
}

} // namespace backtrack
