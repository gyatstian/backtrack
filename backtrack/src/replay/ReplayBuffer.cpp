#include "replay/ReplayBuffer.h"

#include "audio/AudioConvert.h"
#include "core/Clock.h"
#include "core/Logger.h"
#include "encoder/BitstreamUtils.h"
#include "mux/AudioTimeline.h"
#include "mux/MuxedInputs.h"
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

// One GOP of tolerance: a cut may start up to a GOP before the window so the
// clip still begins at a keyframe.
int64_t gopTolerance100ns(uint32_t gopSeconds) {
    return secondsTo100ns((std::max<uint32_t>)(1, gopSeconds));
}

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

    // Shared pacing core with the recording path (mux/AudioTimeline.h): gap
    // fill capped at 10 s with resync past the cap, overlap trim, and the
    // replay window end as the write bound.
    const std::wstring fileName = path.filename().wstring();
    int64_t nextPts100ns = startPts100ns;
    for (const auto& packet : audio) {
        if (packet->frameCount == 0 || packet->bytes.empty()) {
            continue;
        }
        if (!packetFormatMatches(*packet)) {
            continue;
        }
        if (!paceAudioPacket(writer, nextPts100ns, *packet, endPts100ns, L"replay", fileName.c_str())) {
            return false;
        }
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

// Audio payloads are small (a second of stereo 48 kHz 16-bit is ~192 KiB per
// track), so the audio ring needs only a fraction of the video budget. 1 MiB
// still holds several seconds per track; anything below it is not a capacity
// change at all - it turns audio disk backing off, like the video floor.
constexpr uint64_t kMinAudioRingCapacityBytes = 1024ull * 1024;

// RAM fallback caps when configure arrives with a zero capacity: the same
// derivation RecorderController applies (one window at the default bitrate
// plus ~33% VBR headroom for video, one window of two PCM tracks doubled for
// audio). configure always passes real caps from the controller, so this only
// bites callers that pass none (tests).
uint64_t defaultRamVideoCapBytes(uint32_t seconds) {
    constexpr uint64_t kMinCapacityBytes = 64ull * 1024 * 1024;
    constexpr uint64_t kMaxCapacityBytes = 16ull * 1024 * 1024 * 1024;
    const uint64_t windowSeconds = (std::max<uint32_t>)(1, seconds);
    const uint64_t bytesPerWindow =
        (static_cast<uint64_t>(VideoSettings{}.bitrateKbps) * 1000ull / 8ull) * windowSeconds;
    return (std::min<uint64_t>)(
        (std::max<uint64_t>)(bytesPerWindow + bytesPerWindow / 3, kMinCapacityBytes),
        kMaxCapacityBytes);
}

uint64_t defaultRamAudioCapBytes(uint32_t seconds) {
    constexpr uint64_t kMinAudioCapacityBytes = 1024ull * 1024;
    constexpr uint64_t kMaxAudioCapacityBytes = 4ull * 1024 * 1024 * 1024;
    constexpr uint64_t kBytesPerSecond = 768000;
    const uint64_t windowSeconds = (std::max<uint32_t>)(1, seconds);
    return (std::min<uint64_t>)(
        (std::max<uint64_t>)(windowSeconds * kBytesPerSecond, kMinAudioCapacityBytes),
        kMaxAudioCapacityBytes);
}

} // namespace

void ReplayBuffer::publishCountersLocked() const {
    const bool disk = videoBackend_ == VideoBackend::Disk;
    mirroredVideoPackets_.store(disk ? diskVideo_.size() : video_.size(), std::memory_order_release);
    mirroredKeyFrames_.store(disk ? diskVideoKeyFrames_ : videoKeyFrames_, std::memory_order_release);
}

std::wstring ReplayBuffer::storageFallbackReason() const {
    std::scoped_lock lock(storageFallbackMutex_);
    if (videoFallbackReason_.empty()) {
        return audioFallbackReason_;
    }
    if (audioFallbackReason_.empty()) {
        return videoFallbackReason_;
    }
    return videoFallbackReason_ + L" " + audioFallbackReason_;
}

void ReplayBuffer::setVideoFallbackReasonLocked(std::wstring reason) {
    std::scoped_lock lock(storageFallbackMutex_);
    videoFallbackReason_ = std::move(reason);
}

void ReplayBuffer::setAudioFallbackReasonLocked(std::wstring reason) {
    std::scoped_lock lock(storageFallbackMutex_);
    audioFallbackReason_ = std::move(reason);
}

void ReplayBuffer::removeOrphanedRing(const std::filesystem::path& storageDirectory) {
    if (storageDirectory.empty()) {
        return;
    }
    const auto directory = storageDirectory / kRingFolderName;
    std::error_code error;
    if (!std::filesystem::exists(directory, error) || error) {
        return;
    }
    std::filesystem::remove_all(directory, error);
    if (error) {
        Logger::instance().warning(L"replay",
            L"Could not remove leftover replay ring folder " + directory.wstring() +
            L" (" + utf8ToWide(error.message()) + L")");
        return;
    }
    Logger::instance().info(L"replay",
        L"Removed leftover replay ring folder " + directory.wstring() +
        L" from a previous session");
}

ReplayBuffer::~ReplayBuffer() {
    std::scoped_lock lock(mutex_);
    closeRingNowLocked();
    closeAudioRingNowLocked();
}

void ReplayBuffer::configure(const ReplaySettings& settings,
                             const std::filesystem::path& storageDirectory,
                             uint64_t capacityBytes,
                             uint64_t audioCapacityBytes) {
    std::scoped_lock lock(mutex_);
    const std::filesystem::path previousDirectory = ringDirectory_;
    settings_ = settings;
    ringDirectory_ = storageDirectory;
    // Bound RAM too: a long window at a high bitrate would otherwise grow the
    // process without limit. Failover keeps these, so the cap survives to RAM.
    ramVideoCapacityBytes_ =
        capacityBytes != 0 ? capacityBytes : defaultRamVideoCapBytes(settings.seconds);
    ramAudioCapacityBytes_ =
        audioCapacityBytes != 0 ? audioCapacityBytes : defaultRamAudioCapBytes(settings.seconds);

    if (!settings_.enabled) {
        video_.clear();
        systemAudio_.clear();
        microphoneAudio_.clear();
        ramVideoBytes_ = 0;
        ramAudioBytes_ = 0;
        clearBufferedVideoLocked();
        clearBufferedAudioLocked();
        lastSaveEndPts100ns_ = 0;
        codecHeaderBytes_.clear();
        closeRingLocked();
        closeAudioRingLocked();
        videoBackend_ = VideoBackend::Ram;
        audioBackend_ = AudioBackend::Ram;
        setVideoFallbackReasonLocked({});
        setAudioFallbackReasonLocked({});
        return;
    }

    const bool wantDisk = settings_.diskBackedVideo &&
                          !storageDirectory.empty() &&
                          capacityBytes >= kMinRingCapacityBytes;
    const bool wantAudioDisk = settings_.diskBackedAudio &&
                               !storageDirectory.empty() &&
                               audioCapacityBytes >= kMinAudioRingCapacityBytes;
    const bool usingDisk = videoBackend_ == VideoBackend::Disk;
    const bool usingAudioDisk = audioBackend_ == AudioBackend::Disk;
    const bool directoryChanged = (wantDisk || wantAudioDisk) && storageDirectory != previousDirectory;

    if (!wantDisk) {
        if (usingDisk) {
            Logger::instance().info(L"replay", L"Replay video buffering switched back to memory");
        }
        clearBufferedVideoLocked();
        closeRingLocked();
        videoBackend_ = VideoBackend::Ram;
        // Also clears a reason left over from a failed open: disk backing is
        // off now, so there is nothing degraded to report.
        setVideoFallbackReasonLocked({});
    } else if (!usingDisk || directoryChanged) {
        // A backend or location switch makes buffered video unreadable in the
        // new configuration; drop it exactly like a codec change does.
        openRingWithFallbackLocked(capacityBytes);
    } else if (capacityBytes != videoRing_.capacity()) {
        // A capacity change alone must not drop buffered video: the recorded
        // extents stay valid at their offsets, and settings_.seconds (already
        // applied above) reshapes the window on the next trim. Re-laying out a
        // ring that still holds packets would invalidate every live extent, so
        // the resize waits until there is nothing left to invalidate - which is
        // what makes editing the replay window length non-destructive.
        if (diskVideo_.empty() && !savePinsRefs_) {
            openRingWithFallbackLocked(capacityBytes);
        } else {
            Logger::instance().info(L"replay",
                L"Replay ring stays at " + std::to_wstring(videoRing_.capacity() / (1024ull * 1024ull)) +
                L" MiB while buffered video remains; the new capacity takes effect on a later settings change once the buffer has drained");
        }
    }

    if (!wantAudioDisk) {
        if (usingAudioDisk) {
            Logger::instance().info(L"replay", L"Replay audio buffering switched back to memory");
        }
        clearBufferedAudioLocked();
        closeAudioRingLocked();
        audioBackend_ = AudioBackend::Ram;
        setAudioFallbackReasonLocked({});
    } else if (!usingAudioDisk || directoryChanged) {
        // Same rule as video: a backend or location switch drops buffered
        // audio, which is unreadable in the new configuration. Only the audio
        // window is dropped; video buffering is untouched.
        openAudioRingWithFallbackLocked(audioCapacityBytes);
    } else if (audioCapacityBytes != audioRing_.capacity()) {
        // Same non-destructive rule as the video ring: re-laying out a ring
        // that still holds packets would invalidate every live extent.
        if (diskSystemAudio_.empty() && diskMicrophoneAudio_.empty() && !savePinsRefs_) {
            openAudioRingWithFallbackLocked(audioCapacityBytes);
        } else {
            Logger::instance().info(L"replay",
                L"Replay audio ring stays at " + std::to_wstring(audioRing_.capacity() / (1024ull * 1024ull)) +
                L" MiB while buffered audio remains; the new capacity takes effect on a later settings change once the buffer has drained");
        }
    }

    trimLocked();
}

void ReplayBuffer::openRingWithFallbackLocked(uint64_t capacityBytes) {
    // Caller holds mutex_ and has already decided the buffer may be dropped.
    clearBufferedVideoLocked();
    closeRingLocked();
    if (!openRingLocked(capacityBytes)) {
        videoBackend_ = VideoBackend::Ram;
        // Surfaced on the Diagnostics page and next to the settings checkbox:
        // the user asked for a disk ring and is silently getting RAM, which
        // changes what a long buffer costs in system memory.
        setVideoFallbackReasonLocked(
            L"Replay disk buffering could not be used (" + ringDirectory_.wstring() +
            L"); replay video is being buffered in memory instead. See the log for details.");
        Logger::instance().warning(L"replay",
            L"Disk-backed replay buffering unavailable; falling back to memory");
        return;
    }
    videoBackend_ = VideoBackend::Disk;
    setVideoFallbackReasonLocked({});
    Logger::instance().info(L"replay",
        L"Replay video buffering moved to disk ring " + videoRing_.path().wstring() +
        L" (" + std::to_wstring(capacityBytes / (1024ull * 1024ull)) + L" MiB)");
}

bool ReplayBuffer::openRingLocked(uint64_t capacityBytes) {
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
    return videoRing_.open(ringDirectory_ / kRingFolderName, kRingFileName, capacityBytes);
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
    videoRing_.closeNow();
    ringPendingClose_ = false;
}

void ReplayBuffer::openAudioRingWithFallbackLocked(uint64_t capacityBytes) {
    // Caller holds mutex_ and has already decided the audio buffer may be dropped.
    clearBufferedAudioLocked();
    closeAudioRingLocked();
    if (!openAudioRingLocked(capacityBytes)) {
        audioBackend_ = AudioBackend::Ram;
        setAudioFallbackReasonLocked(
            L"Replay sound disk buffering could not be used (" + ringDirectory_.wstring() +
            L"); replay sound is being buffered in memory instead. See the log for details.");
        Logger::instance().warning(L"replay",
            L"Disk-backed replay audio buffering unavailable; falling back to memory");
        return;
    }
    audioBackend_ = AudioBackend::Disk;
    setAudioFallbackReasonLocked({});
    Logger::instance().info(L"replay",
        L"Replay audio buffering moved to disk ring " + audioRing_.path().wstring() +
        L" (" + std::to_wstring(capacityBytes / (1024ull * 1024ull)) + L" MiB)");
}

bool ReplayBuffer::openAudioRingLocked(uint64_t capacityBytes) {
    if (audioRingPendingClose_) {
        if (savePinsRefs_) {
            Logger::instance().warning(L"replay",
                L"Cannot reopen the replay audio ring while a save is still reading the old one");
            return false;
        }
        closeAudioRingNowLocked();
    }
    return audioRing_.open(ringDirectory_ / kRingFolderName, kAudioRingFileName, capacityBytes);
}

void ReplayBuffer::closeAudioRingLocked() noexcept {
    if (savePinsRefs_) {
        audioRingPendingClose_ = true;
        return;
    }
    closeAudioRingNowLocked();
}

void ReplayBuffer::closeAudioRingNowLocked() noexcept {
    audioRing_.closeNow();
    audioRingPendingClose_ = false;
}

void ReplayBuffer::breakRingForTesting() noexcept {
    std::scoped_lock lock(mutex_);
    videoRing_.breakForTesting();
}

void ReplayBuffer::breakAudioRingForTesting() noexcept {
    std::scoped_lock lock(mutex_);
    audioRing_.breakForTesting();
}

void ReplayBuffer::pushVideo(EncodedPacket packet) {
    if (packet.bytes.empty()) {
        return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (!settings_.enabled) {
        return;
    }
    // Storage backstop, canonical in mux/MuxedInputs.h: upstream durations are
    // wall-clock and a coalesced hold may arrive oversized; healthy frames are
    // milliseconds, so this only bites rogue tails.
    packet.duration100ns = clampSingleVideoDuration100ns(packet.duration100ns);
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
        failOverToRamLocked(
            L"Replay disk buffering failed mid-session (the disk may be full); replay video is "
            L"being buffered in memory for the rest of this session. See the log for details.");
    }

    if (packet.keyFrame) {
        ++videoKeyFrames_;
    }
    // A packet larger than the whole RAM cap can never fit, same as the ring.
    if (ramVideoCapacityBytes_ > 0 && packet.bytes.size() > ramVideoCapacityBytes_) {
        if (packet.keyFrame && videoKeyFrames_ > 0) {
            --videoKeyFrames_;
        }
        return;
    }
    ramVideoBytes_ += packet.bytes.size();
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
        // Same canonical coalescing as the recording path: stretch, then ceil.
        diskVideo_.back().meta.duration100ns = coalesceVideoDuration100ns(
            diskVideo_.back().meta.duration100ns, duration100ns);
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
    packet.duration100ns = coalesceVideoDuration100ns(packet.duration100ns, duration100ns);
    return true;
}

void ReplayBuffer::pushAudio(AudioPacket packet) {
    if (packet.bytes.empty()) {
        return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (!settings_.enabled) {
        return;
    }
    if (audioBackend_ == AudioBackend::Disk) {
        const auto size = static_cast<uint32_t>(packet.bytes.size());
        if (!ensureAudioRingSpaceLocked(lock, size)) {
            return;
        }
        if (appendAudioRingLocked(packet)) {
            return;
        }
        failOverAudioToRamLocked(
            L"Replay sound disk buffering failed mid-session (the disk may be full); replay sound is "
            L"being buffered in memory for the rest of this session. See the log for details.");
    }
    auto& queue = packet.track == AudioTrack::System ? systemAudio_ : microphoneAudio_;
    if (ramAudioCapacityBytes_ > 0 && packet.bytes.size() > ramAudioCapacityBytes_) {
        return;
    }
    ramAudioBytes_ += packet.bytes.size();
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
    const int64_t saveStart100ns = steadyNow100ns();

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
    std::vector<DiskAudioPacket> diskSystemAudio;
    std::vector<DiskAudioPacket> diskMicrophoneAudio;
    int64_t lastSaveEndPts100ns = 0;
    uint32_t replayWindowSeconds = 0;
    uint32_t saveGopSeconds = 2;
    std::vector<uint8_t> codecHeaderBytes;
    bool diskMode = false;
    bool diskAudioMode = false;
    // Read under the same lock as the packet snapshot: a concurrent configure
    // can replace the ring files, and the pin release below may close them too.
    std::filesystem::path ringPath;
    std::filesystem::path audioRingPath;
    {
        std::scoped_lock lock(mutex_);
        diskMode = videoBackend_ == VideoBackend::Disk;
        diskAudioMode = audioBackend_ == AudioBackend::Disk;
        ringPath = videoRing_.path();
        audioRingPath = audioRing_.path();
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
        if (diskAudioMode) {
            diskSystemAudio.assign(diskSystemAudio_.begin(), diskSystemAudio_.end());
            diskMicrophoneAudio.assign(diskMicrophoneAudio_.begin(), diskMicrophoneAudio_.end());
        }
        lastSaveEndPts100ns = lastSaveEndPts100ns_;
        replayWindowSeconds = settings_.seconds;
        // Cache for videoSpan100ns, which has no VideoSettings of its own.
        lastGopSeconds_ = (std::max<uint32_t>)(1, videoSettings.gopSeconds);
        saveGopSeconds = lastGopSeconds_;
        codecHeaderBytes = codecHeaderBytes_;
        // While the pin holds, ring pushes that need space wait instead of
        // trimming: trimming could evict packets this save has not read yet.
        if (diskMode || diskAudioMode) {
            ++savePinsRefs_;
        }
    }
    // Releases the pin on every path (early returns included) once the ring
    // bytes have been consumed; also performs a ring close that the pin had
    // deferred, then resumes age-based trimming of the disk backends.
    struct PinRelease {
        ReplayBuffer* self;
        ~PinRelease() {
            {
                std::scoped_lock lock(self->mutex_);
                --self->savePinsRefs_;
                if (self->savePinsRefs_ > 0) {
                    return; // Another save still holds the pin.
                }
                const bool closedVideo = self->ringPendingClose_;
                if (closedVideo) {
                    self->closeRingNowLocked();
                }
                const bool closedAudio = self->audioRingPendingClose_;
                if (closedAudio) {
                    self->closeAudioRingNowLocked();
                }
                if (!closedVideo && !closedAudio &&
                    (self->videoBackend_ == VideoBackend::Disk ||
                     self->audioBackend_ == AudioBackend::Disk)) {
                    self->trimLocked();
                }
            }
            self->ringSpace_.notify_all();
        }
    };
    std::optional<PinRelease> pinRelease;
    if (diskMode || diskAudioMode) {
        pinRelease.emplace(this);
    }

    if (video.empty()) {
        return fail(L"replay video buffer is empty");
    }

    // Storage backstop (see extendLastVideoDuration): a day-long coalesced
    // tail must not stretch the cut or the muxed sample.
    video.back().meta.duration100ns =
        clampSingleVideoDuration100ns(video.back().meta.duration100ns);

    int64_t newestVideoEndPts =
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
    // Window cut, applied after (intersecting) the consecutive-save overlap
    // cut above: age eviction keys on PTS, which freezes while coalescing, so
    // a long hold would otherwise publish window + stretched tail. Packets are
    // dropped by end time, then realigned forward to a keyframe; the cut only
    // ever narrows.
    const int64_t oldestAllowedEndPts = newestVideoEndPts - secondsTo100ns(replayWindowSeconds) -
        gopTolerance100ns(saveGopSeconds);
    auto windowBegin = video.end();
    for (auto it = video.begin(); it != video.end(); ++it) {
        if (it->meta.pts100ns + (std::max<int64_t>)(it->meta.duration100ns, 0) > oldestAllowedEndPts) {
            windowBegin = it;
            break;
        }
    }
    if (windowBegin != video.end()) {
        const auto windowKeyFrame = std::find_if(
            windowBegin, video.end(), [](const SnapshottedVideo& snapshot) { return snapshot.meta.keyFrame; });
        if (windowKeyFrame != video.end()) {
            video.erase(video.begin(), windowKeyFrame);
        } else {
            // Tail region holds no keyframe (a stretched P-frame tail): the
            // buffer already starts at a keyframe, so shrink the tail to fit
            // the window budget instead of publishing past it.
            const int64_t tailBudget = secondsTo100ns(replayWindowSeconds) +
                gopTolerance100ns(saveGopSeconds) -
                (video.back().meta.pts100ns - video.front().meta.pts100ns);
            int64_t& tailDuration = video.back().meta.duration100ns;
            tailDuration = (std::min)(tailDuration, (std::max<int64_t>)(tailBudget, 0));
        }
        newestVideoEndPts =
            video.back().meta.pts100ns + std::max<int64_t>(video.back().meta.duration100ns, 0);
    }
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

    // A stale manifest belongs to a previous failed save whose elementary
    // streams this attempt is about to truncate; drop it before opening the
    // streams so a crash mid-attempt can never describe bytes that no longer
    // exist. This attempt publishes a fresh manifest before muxing.
    RecoveryStore::clearStaleManifest(tempDirectory);

    const auto videoPath = tempDirectory / (videoSettings.codec == VideoCodec::H264 ? L"replay.h264" : L"replay.hevc");

    // Backend-agnostic payload reader for the cut snapshots below. A RAM
    // snapshot owns its bytes; a disk snapshot pins ring extents, and the pin
    // taken above guarantees they cannot be overwritten while consumed. Ring
    // reads happen without holding mutex_. Both the parameter-set probe and
    // the dump loop read through this one function, so the cut/patch/mux path
    // never branches on the backend.
    std::vector<uint8_t> ringScratch;
    // A ring read failure means those packet bytes are gone for good, which is
    // a different failure from a full disk: the elementary stream is still
    // healthy, so only !videoStream cannot see it. Publishing the shorter
    // sample table would instead produce a clip whose video ends early against
    // audio running to the original end, so the save must fail. Flagged here
    // rather than folded into the stream state because the two need different
    // user-facing reasons.
    bool ringReadFailed = false;
    auto snapshotPayload = [&](const SnapshottedVideo& snapshot) -> const std::vector<uint8_t>* {
        if (snapshot.ram) {
            return &snapshot.ram->bytes;
        }
        const uint32_t totalBytes = snapshot.first.byteCount + snapshot.second.byteCount;
        ringScratch.resize(totalBytes);
        if (snapshot.first.byteCount != 0 &&
            !videoRing_.read(snapshot.first.offset, ringScratch.data(), snapshot.first.byteCount)) {
            ringReadFailed = true;
            return nullptr;
        }
        if (snapshot.second.byteCount != 0 &&
            !videoRing_.read(
                snapshot.second.offset,
                ringScratch.data() + snapshot.first.byteCount,
                snapshot.second.byteCount)) {
            ringReadFailed = true;
            return nullptr;
        }
        return &ringScratch;
    };
    auto snapshotPayloadSize = [](const SnapshottedVideo& snapshot) -> uint64_t {
        return snapshot.ram ? snapshot.ram->bytes.size()
                            : static_cast<uint64_t>(snapshot.first.byteCount) + snapshot.second.byteCount;
    };

    // The clip starts at an IDR, but a driver that ignored header insertion
    // may have produced a keyframe without SPS/PPS/VPS, which no decoder can
    // bootstrap. Merge the cached parameter sets into the FIRST sample (the
    // MP4 sample table only exposes bytes covered by samples) so the clip is
    // decodable from its first frame.
    const std::vector<uint8_t>* firstPayload = snapshotPayload(video.front());
    // A failed probe would otherwise read as "this frame carries no
    // parameter sets" and prepend sets the dump loop is about to write
    // anyway, duplicating them in the first sample.
    const bool firstSampleHasHeaders =
        firstPayload != nullptr && bitstreamContainsParameterSets(videoSettings.codec, *firstPayload);
    if (!firstSampleHasHeaders && !ringReadFailed && codecHeaderBytes.empty()) {
        Logger::instance().warning(L"replay",
            L"Replay starts at a keyframe without codec headers and no cached headers are available");
    }
    const uint64_t headersToWrite =
        !firstSampleHasHeaders && !ringReadFailed && !codecHeaderBytes.empty() ? codecHeaderBytes.size() : 0;

    // Fail fast on a full disk before writing hundreds of megabytes of
    // elementary streams: the estimate covers the cut video, the buffered
    // audio of both tracks, and the mux overhead. Best effort (silence fill
    // can grow audio past it); muxToMp4 re-checks with real file sizes.
    {
        uint64_t estimatedBytes = headersToWrite + RecoveryStore::kSaveFreeSpaceSlackBytes;
        for (const auto& snapshot : video) {
            estimatedBytes += snapshotPayloadSize(snapshot);
        }
        for (const auto& packet : systemAudio) {
            estimatedBytes += packet->bytes.size();
        }
        for (const auto& packet : microphoneAudio) {
            estimatedBytes += packet->bytes.size();
        }
        for (const auto& entry : diskSystemAudio) {
            estimatedBytes += static_cast<uint64_t>(entry.first.byteCount) + entry.second.byteCount;
        }
        for (const auto& entry : diskMicrophoneAudio) {
            estimatedBytes += static_cast<uint64_t>(entry.first.byteCount) + entry.second.byteCount;
        }
        std::wstring spaceDetail;
        if (!RecoveryStore::ensureFreeDiskSpace(outputPath, estimatedBytes, spaceDetail)) {
            return fail(spaceDetail);
        }
    }

    std::ofstream videoStream(videoPath, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!videoStream.is_open()) {
        return fail(L"could not open temporary replay video stream " + videoPath.wstring());
    }

    // Sized from the bytes actually written: sample 0's table entry must cover
    // exactly what is on disk, or every later sample offset shifts.
    uint32_t headersWritten = 0;
    if (headersToWrite > 0) {
        videoStream.write(
            reinterpret_cast<const char*>(codecHeaderBytes.data()),
            static_cast<std::streamsize>(codecHeaderBytes.size()));
        if (videoStream) {
            headersWritten = static_cast<uint32_t>(codecHeaderBytes.size());
        }
    }
    std::vector<MuxedInputs::VideoSample> videoSamples;
    uint64_t videoOffset = 0;
    for (size_t index = 0; index < video.size(); ++index) {
        const SnapshottedVideo& snapshot = video[index];
        // Shared validity rule with the recording path (mux/MuxedInputs.h):
        // argument order is (pts, dts, flag). A decode timestamp counts only
        // when explicitly flagged, and values can legitimately be zero or
        // negative, so no numeric sentinel works.
        const bool hasDts = hasEffectiveDts(snapshot.meta.pts100ns, snapshot.meta.dts100ns, snapshot.meta.hasDts);
        // One fetch feeds both the write and the table entry, so the recorded
        // size can never drift from the bytes on disk; sample 0 additionally
        // covers the headers written above.
        const std::vector<uint8_t>* payload = snapshotPayload(snapshot);
        if (payload == nullptr) {
            break;
        }
        videoStream.write(
            reinterpret_cast<const char*>(payload->data()),
            static_cast<std::streamsize>(payload->size()));
        if (!videoStream) {
            break;
        }
        const uint32_t sampleBytes =
            static_cast<uint32_t>(payload->size()) + (index == 0 ? headersWritten : 0);
        videoSamples.push_back(MuxedInputs::VideoSample{
            videoOffset,
            sampleBytes,
            snapshot.meta.pts100ns,
            effectiveDts(snapshot.meta.pts100ns, snapshot.meta.dts100ns, snapshot.meta.hasDts),
            hasDts,
            snapshot.meta.duration100ns,
            snapshot.meta.keyFrame});
        videoOffset += sampleBytes;
    }
    videoStream.close();
    // Reassemble disk-backed audio the same way: the pin guarantees the
    // referenced extents cannot be overwritten while they are consumed.
    bool audioRingReadFailed = false;
    // Unconditional: the disk queues are empty in RAM mode, so this no-ops
    // there and the save path never branches on the audio backend.
    {
        auto reassembleAudio = [&](const std::vector<DiskAudioPacket>& disk,
                                   std::vector<std::shared_ptr<const AudioPacket>>& out) {
            for (const DiskAudioPacket& entry : disk) {
                const uint32_t totalBytes = entry.first.byteCount + entry.second.byteCount;
                ringScratch.resize(totalBytes);
                if (entry.first.byteCount != 0 &&
                    !audioRing_.read(entry.first.offset, ringScratch.data(), entry.first.byteCount)) {
                    audioRingReadFailed = true;
                    return;
                }
                if (entry.second.byteCount != 0 &&
                    !audioRing_.read(
                        entry.second.offset,
                        ringScratch.data() + entry.first.byteCount,
                        entry.second.byteCount)) {
                    audioRingReadFailed = true;
                    return;
                }
                auto packet = std::make_shared<AudioPacket>();
                packet->track = entry.track;
                packet->processId = entry.processId;
                packet->bytes.assign(ringScratch.begin(), ringScratch.end());
                packet->frameCount = entry.frameCount;
                packet->pts100ns = entry.pts100ns;
                packet->format = entry.format;
                out.push_back(std::move(packet));
            }
        };
        reassembleAudio(diskSystemAudio, systemAudio);
        if (!audioRingReadFailed) {
            reassembleAudio(diskMicrophoneAudio, microphoneAudio);
        }
    }
    pinRelease.reset(); // All ring bytes consumed; pushes may reclaim space.

    // Checked before the stream state: a ring read failure never touches
    // videoStream, so the two are independent causes with different fixes.
    // Failing here also keeps lastSaveEndPts100ns_ and the audio window from
    // advancing past frames that were never published, which is what made the
    // lost range unrecoverable on the next consecutive save.
    if (ringReadFailed) {
        // The bytes behind these extents are gone, and nothing in the ring can
        // bring them back: every later save would cut from the same offsets and
        // fail identically, leaving the feature unusable for the rest of the
        // session. Drop the ring and buffer in memory instead, which is the
        // same policy a ring write failure already follows.
        {
            std::scoped_lock lock(mutex_);
            failOverToRamLocked(
                L"Replay disk buffering failed to read back its own data (the drive or folder "
                L"may have gone away); replay video is being buffered in memory for the rest of "
                L"this session. See the log for details.");
        }
        return fail(L"could not read buffered replay video from the disk ring " + ringPath.wstring());
    }
    if (audioRingReadFailed) {
        // Same policy as the video path: the bytes behind these extents are
        // gone, so every later save would fail identically. Drop the audio
        // ring and buffer sound in memory instead; video buffering is
        // untouched.
        {
            std::scoped_lock lock(mutex_);
            failOverAudioToRamLocked(
                L"Replay sound disk buffering failed to read back its own data (the drive or folder "
                L"may have gone away); replay sound is being buffered in memory for the rest of "
                L"this session. See the log for details.");
        }
        return fail(L"could not read buffered replay sound from the disk ring " + audioRingPath.wstring());
    }
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

    // Shared publish tail with the recording finalize path: manifest+readme
    // first, then mux with retry. The temp layout matches what
    // recoverLatestFailedRecording scans, so recovery needs no special case.
    switch (RecoveryStore::publishMuxedInputs(inputs, outputPath, tempDirectory, errorDetail)) {
        case RecoveryStore::MuxPublishResult::kPublished:
            cleanup.cleanup();
            {
                std::scoped_lock lock(mutex_);
                lastSaveEndPts100ns_ = newestVideoEndPts;
                Logger::instance().info(L"replay",
                    L"Replay saved: " + outputPath.wstring() +
                    L" in " + std::to_wstring((steadyNow100ns() - saveStart100ns) / 10000) + L"ms");
            }
            return true;
        case RecoveryStore::MuxPublishResult::kPreservedForRecovery:
            // The manifest describes the streams on disk: keep the directory
            // so the clip stays recoverable. errorDetail already carries the
            // preserved-recording reason.
            cleanup.release();
            return false;
        case RecoveryStore::MuxPublishResult::kFailed:
            // No manifest, so nothing is recoverable; drop the directory
            // rather than promise a recovery that cannot happen.
            cleanup.cleanup();
            return false;
    }
    return false;
}

void ReplayBuffer::clear() {
    std::scoped_lock lock(mutex_);
    video_.clear();
    systemAudio_.clear();
    microphoneAudio_.clear();
    ramVideoBytes_ = 0;
    ramAudioBytes_ = 0;
    clearBufferedVideoLocked();
    clearBufferedAudioLocked();
    lastSaveEndPts100ns_ = 0;
    codecHeaderBytes_.clear();
}

size_t ReplayBuffer::videoPacketCount() const {
    // Lock-free on purpose: RecorderController::stats reads this from the UI
    // thread, and taking mutex_ here would queue it behind the encode thread's
    // synchronous ring write. Acquire pairs with publishCountersLocked so a
    // reader never sees a count published before the buffer state behind it.
    return mirroredVideoPackets_.load(std::memory_order_acquire);
}

size_t ReplayBuffer::videoKeyFrameCount() const {
    return mirroredKeyFrames_.load(std::memory_order_acquire);
}

int64_t ReplayBuffer::videoSpan100ns() const {
    std::scoped_lock lock(mutex_);
    // Backend-agnostic span: collect the metadata once, then run the same
    // window cut saveTo applies so the reported span never exceeds what a save
    // would publish. Keyed on the last GOP saveTo observed.
    std::vector<VideoPacketMeta> metas;
    if (videoBackend_ == VideoBackend::Disk) {
        metas.reserve(diskVideo_.size());
        for (const auto& entry : diskVideo_) {
            metas.push_back(entry.meta);
        }
    } else {
        metas.reserve(video_.size());
        for (const auto& packet : video_) {
            VideoPacketMeta meta;
            meta.pts100ns = packet->pts100ns;
            meta.dts100ns = packet->dts100ns;
            meta.duration100ns = packet->duration100ns;
            meta.hasDts = packet->hasDts;
            meta.keyFrame = packet->keyFrame;
            metas.push_back(meta);
        }
    }
    if (metas.empty()) {
        return 0;
    }
    const auto firstKeyFrame = std::find_if(metas.begin(), metas.end(), [](const VideoPacketMeta& meta) {
        return meta.keyFrame;
    });
    if (firstKeyFrame == metas.end()) {
        return 0;
    }
    const int64_t newestVideoEndPts = metas.back().pts100ns +
        (std::max<int64_t>)(clampSingleVideoDuration100ns(metas.back().duration100ns), 0);
    const uint32_t gopSeconds = (std::max<uint32_t>)(1, lastGopSeconds_);
    const int64_t oldestAllowedEndPts =
        newestVideoEndPts - secondsTo100ns(settings_.seconds) - gopTolerance100ns(gopSeconds);
    auto windowBegin = metas.end();
    for (auto it = metas.begin(); it != metas.end(); ++it) {
        if (it->pts100ns + (std::max<int64_t>)(it->duration100ns, 0) > oldestAllowedEndPts) {
            windowBegin = it;
            break;
        }
    }
    if (windowBegin != metas.end()) {
        const auto windowKeyFrame = std::find_if(
            windowBegin, metas.end(), [](const VideoPacketMeta& meta) { return meta.keyFrame; });
        if (windowKeyFrame != metas.end()) {
            return (std::max<int64_t>)(0, newestVideoEndPts - windowKeyFrame->pts100ns);
        }
    }
    return (std::min)(
        (std::max<int64_t>)(0, newestVideoEndPts - firstKeyFrame->pts100ns),
        secondsTo100ns(settings_.seconds) + gopTolerance100ns(gopSeconds));
}

int64_t ReplayBuffer::newestVideoPts100ns() const {
    std::scoped_lock lock(mutex_);
    if (videoBackend_ == VideoBackend::Disk) {
        return diskVideo_.empty() ? 0 : diskVideo_.back().meta.pts100ns;
    }
    return video_.empty() ? 0 : video_.back()->pts100ns;
}

int64_t ReplayBuffer::diskAudioPacketEndPts100ns(const DiskAudioPacket& packet) {
    if (!packet.format || packet.frameCount == 0 || packet.format->bytes.size() < sizeof(WAVEFORMATEX)) {
        return packet.pts100ns;
    }
    const auto* format = reinterpret_cast<const WAVEFORMATEX*>(packet.format->bytes.data());
    if (format->nSamplesPerSec == 0) {
        return packet.pts100ns;
    }
    return packet.pts100ns +
           static_cast<int64_t>((static_cast<int64_t>(packet.frameCount) * kHundredNanosecondsPerSecond) /
                                format->nSamplesPerSec);
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
                videoRing_.releaseBytes(front.first.byteCount + front.second.byteCount);
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
            ramVideoBytes_ = 0;
            ramAudioBytes_ = 0;
            lastSaveEndPts100ns_ = 0;
            publishCountersLocked();
            return;
        }

        const int64_t newestPts = video_.back()->pts100ns;
        const int64_t oldestAllowed = newestPts - secondsTo100ns(settings_.seconds);

        auto popVideoFront = [&]() {
            if (!video_.empty()) {
                ramVideoBytes_ -= video_.front()->bytes.size();
                if (video_.front()->keyFrame && videoKeyFrames_ > 0) {
                    --videoKeyFrames_;
                }
                video_.pop_front();
            }
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
        // RAM byte cap, same realign as the disk ring: evict oldest until
        // under the cap, dropping the stranded P-frames before each keyframe
        // so the buffer stays decodable. Safe mid-save: RAM snapshots hold
        // shared_ptrs, so no pin wait is needed.
        while (ramVideoCapacityBytes_ > 0 && ramVideoBytes_ > ramVideoCapacityBytes_ && !video_.empty()) {
            popVideoFront();
            const auto nextKeyFrame = std::find_if(
                video_.begin(), video_.end(), [](const auto& packet) { return packet->keyFrame; });
            if (nextKeyFrame == video_.end()) {
                continue;
            }
            const auto stranded =
                static_cast<size_t>(std::distance(video_.begin(), nextKeyFrame));
            for (size_t index = 0; index < stranded; ++index) {
                popVideoFront();
            }
        }
    }

    // One audio trim for both backends and both bounds below: RAM packets
    // subtract their RAM bytes, disk packets release their ring bytes. The
    // end-time function differs because the disk metadata is not an
    // AudioPacket.
    const int64_t newestAudioReferencePts = videoBackend_ == VideoBackend::Disk
        ? (diskVideo_.empty() ? 0 : diskVideo_.back().meta.pts100ns)
        : (video_.empty() ? 0 : video_.back()->pts100ns);
    const int64_t oldestAudioAllowed = newestAudioReferencePts - secondsTo100ns(settings_.seconds);
    auto trimAudioQueue = [](auto& queue, int64_t bound, auto endPts, auto evicted) {
        while (!queue.empty() && endPts(queue.front()) <= bound) {
            evicted(queue.front());
            queue.pop_front();
        }
    };
    auto ramAudioEndPts = [](const std::shared_ptr<const AudioPacket>& packet) {
        return audioPacketEndPts100ns(*packet);
    };
    auto dropRamAudio = [this](const std::shared_ptr<const AudioPacket>& packet) {
        ramAudioBytes_ -= packet->bytes.size();
    };
    auto diskAudioEndPts = [](const DiskAudioPacket& packet) {
        return diskAudioPacketEndPts100ns(packet);
    };
    auto releaseDiskAudio = [this](const DiskAudioPacket& packet) {
        audioRing_.releaseBytes(packet.first.byteCount + packet.second.byteCount);
    };

    const bool videoEmpty = videoBackend_ == VideoBackend::Disk ? diskVideo_.empty() : video_.empty();
    if (videoEmpty) {
        systemAudio_.clear();
        microphoneAudio_.clear();
        ramAudioBytes_ = 0;
        if (video_.empty()) {
            ramVideoBytes_ = 0;
        }
        // Pinned extents keep their bytes until the deferred close, so a
        // mid-save clear must not make the ring look free (see
        // clearBufferedVideoLocked). The pin release path trims again.
        if (!savePinsRefs_) {
            clearBufferedAudioLocked();
        }
        videoKeyFrames_ = 0;
        diskVideoKeyFrames_ = 0;
        lastSaveEndPts100ns_ = 0;
        publishCountersLocked();
        return;
    }

    trimAudioQueue(systemAudio_, oldestAudioAllowed, ramAudioEndPts, dropRamAudio);
    trimAudioQueue(microphoneAudio_, oldestAudioAllowed, ramAudioEndPts, dropRamAudio);
    if (!savePinsRefs_) {
        trimAudioQueue(diskSystemAudio_, oldestAudioAllowed, diskAudioEndPts, releaseDiskAudio);
        trimAudioQueue(diskMicrophoneAudio_, oldestAudioAllowed, diskAudioEndPts, releaseDiskAudio);
    }

    const int64_t videoStartPts = videoBackend_ == VideoBackend::Disk
        ? diskVideo_.front().meta.pts100ns
        : video_.front()->pts100ns;
    trimAudioQueue(systemAudio_, videoStartPts, ramAudioEndPts, dropRamAudio);
    trimAudioQueue(microphoneAudio_, videoStartPts, ramAudioEndPts, dropRamAudio);
    if (!savePinsRefs_) {
        trimAudioQueue(diskSystemAudio_, videoStartPts, diskAudioEndPts, releaseDiskAudio);
        trimAudioQueue(diskMicrophoneAudio_, videoStartPts, diskAudioEndPts, releaseDiskAudio);
    }
    // RAM audio byte cap: plain oldest-first across both tracks by front PTS,
    // same policy as the audio ring. Safe mid-save for the same shared_ptr
    // reason as RAM video.
    while (ramAudioCapacityBytes_ > 0 && ramAudioBytes_ > ramAudioCapacityBytes_ &&
           (!systemAudio_.empty() || !microphoneAudio_.empty())) {
        const int64_t systemPts =
            systemAudio_.empty() ? (std::numeric_limits<int64_t>::max)() : systemAudio_.front()->pts100ns;
        const int64_t microphonePts = microphoneAudio_.empty()
            ? (std::numeric_limits<int64_t>::max)()
            : microphoneAudio_.front()->pts100ns;
        auto& oldest = systemPts <= microphonePts ? systemAudio_ : microphoneAudio_;
        ramAudioBytes_ -= oldest.front()->bytes.size();
        oldest.pop_front();
    }
    publishCountersLocked();
}

void ReplayBuffer::clearBufferedVideoLocked() {
    video_.clear();
    diskVideo_.clear();
    videoKeyFrames_ = 0;
    diskVideoKeyFrames_ = 0;
    ramVideoBytes_ = 0;
    videoRing_.clearLive();
    publishCountersLocked();
    // The write cursor deliberately survives: re-zeroing it would point the
    // next push at offset 0, the most likely place for a still-pinned save to
    // have unread bytes. Resuming from the current cursor keeps the write head
    // at least a full buffer ahead of that. It does NOT make clear() safe
    // during a pin - zeroing the live count makes the whole ring look free, so
    // a push could wrap onto pinned bytes. That is why every caller other than
    // failOverToRamLocked runs on the controller worker, serialized against
    // saveReplay. failOverToRamLocked is the one caller that can clear from the
    // encode thread mid-pin, and it is safe only because it immediately flips
    // the backend to RAM, so no push writes the ring again and the pinned
    // extents keep their bytes until the deferred close. Any new caller must
    // preserve one of those two properties. Only RingFile::open and closeNow
    // reset the cursor.
}

bool ReplayBuffer::ensureRingSpaceLocked(std::unique_lock<std::mutex>& lock, uint32_t size) {
    // Video eviction policy: freeRingSpaceLocked realigns to the next
    // keyframe so the buffer stays independently decodable.
    return videoRing_.ensureSpace(
        lock,
        size,
        savePinsRefs_,
        ringSpace_,
        [this](uint64_t bytesNeeded) { freeRingSpaceLocked(bytesNeeded); },
        [this] { return diskVideo_.empty(); },
        L"ring");
}

bool ReplayBuffer::appendRingLocked(const EncodedPacket& packet) {
    if (!videoRing_.isOpen() && !openRingLocked(videoRing_.capacity())) {
        return false;
    }
    const uint32_t size = static_cast<uint32_t>(packet.bytes.size());

    DiskVideoPacket entry;
    entry.meta.pts100ns = packet.pts100ns;
    entry.meta.dts100ns = packet.dts100ns;
    entry.meta.duration100ns = packet.duration100ns;
    entry.meta.hasDts = packet.hasDts;
    entry.meta.keyFrame = packet.keyFrame;
    if (!videoRing_.writeWrapped(packet.bytes.data(), size, entry.first, entry.second, L"ring")) {
        return false;
    }
    if (entry.meta.keyFrame) {
        ++diskVideoKeyFrames_;
    }
    diskVideo_.push_back(std::move(entry));
    trimLocked();
    return true;
}

void ReplayBuffer::freeRingSpaceLocked(uint64_t bytesNeeded) {
    while (!diskVideo_.empty() && videoRing_.freeBytes() < bytesNeeded) {
        const DiskVideoPacket& front = diskVideo_.front();
        videoRing_.releaseBytes(front.first.byteCount + front.second.byteCount);
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
                videoRing_.releaseBytes(stale.first.byteCount + stale.second.byteCount);
                if (stale.meta.keyFrame && diskVideoKeyFrames_ > 0) {
                    --diskVideoKeyFrames_;
                }
                diskVideo_.pop_front();
            }
        }
    }
    // Byte-eviction changes the counts without going through trimLocked, and
    // the caller can still bail out afterwards, so publish here rather than
    // leaving the UI-thread mirrors stale.
    publishCountersLocked();
}

void ReplayBuffer::failOverToRamLocked(std::wstring reason) {
    clearBufferedVideoLocked();
    closeRingLocked();
    videoBackend_ = VideoBackend::Ram;
    setVideoFallbackReasonLocked(std::move(reason));
    Logger::instance().warning(L"replay",
        L"Replay ring storage failed; disk-backed buffering is disabled for this session and "
        L"video is buffered in memory again");
}

void ReplayBuffer::clearBufferedAudioLocked() {
    diskSystemAudio_.clear();
    diskMicrophoneAudio_.clear();
    audioRing_.clearLive();
    // The write cursor deliberately survives, for the same reason the video
    // cursor does in clearBufferedVideoLocked.
}

void ReplayBuffer::failOverAudioToRamLocked(std::wstring reason) {
    clearBufferedAudioLocked();
    closeAudioRingLocked();
    audioBackend_ = AudioBackend::Ram;
    setAudioFallbackReasonLocked(std::move(reason));
    Logger::instance().warning(L"replay",
        L"Replay audio ring storage failed; disk-backed audio buffering is disabled for this session and "
        L"sound is buffered in memory again");
}

bool ReplayBuffer::ensureAudioRingSpaceLocked(std::unique_lock<std::mutex>& lock, uint32_t size) {
    // Audio eviction policy: plain oldest-first across both tracks, since
    // audio packets stand alone with no keyframe realignment.
    return audioRing_.ensureSpace(
        lock,
        size,
        savePinsRefs_,
        ringSpace_,
        [this](uint64_t bytesNeeded) { freeAudioRingSpaceLocked(bytesNeeded); },
        [this] { return diskSystemAudio_.empty() && diskMicrophoneAudio_.empty(); },
        L"audio ring");
}

bool ReplayBuffer::appendAudioRingLocked(const AudioPacket& packet) {
    if (!audioRing_.isOpen() && !openAudioRingLocked(audioRing_.capacity())) {
        return false;
    }
    const uint32_t size = static_cast<uint32_t>(packet.bytes.size());

    DiskAudioPacket entry;
    entry.track = packet.track;
    entry.processId = packet.processId;
    entry.pts100ns = packet.pts100ns;
    entry.frameCount = packet.frameCount;
    entry.format = packet.format;
    if (!audioRing_.writeWrapped(packet.bytes.data(), size, entry.first, entry.second, L"audio ring")) {
        return false;
    }
    auto& queue = packet.track == AudioTrack::System ? diskSystemAudio_ : diskMicrophoneAudio_;
    queue.push_back(std::move(entry));
    trimLocked();
    return true;
}

void ReplayBuffer::freeAudioRingSpaceLocked(uint64_t bytesNeeded) {
    // Audio has no keyframe realignment: every packet stands alone, so plain
    // oldest-first eviction across both tracks (by front PTS) keeps the window
    // coherent without stranding anything.
    auto frontPts = [](const std::deque<DiskAudioPacket>& queue) {
        return queue.empty() ? (std::numeric_limits<int64_t>::max)() : queue.front().pts100ns;
    };
    while ((!diskSystemAudio_.empty() || !diskMicrophoneAudio_.empty()) && audioRing_.freeBytes() < bytesNeeded) {
        auto& queue = frontPts(diskSystemAudio_) <= frontPts(diskMicrophoneAudio_)
            ? diskSystemAudio_
            : diskMicrophoneAudio_;
        const DiskAudioPacket& front = queue.front();
        audioRing_.releaseBytes(front.first.byteCount + front.second.byteCount);
        queue.pop_front();
    }
}

} // namespace backtrack
