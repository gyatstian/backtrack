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

// Hard limit on PTS gap-fill, the same rule the recording path applies in
// Mp4Muxer::writeTimelineAudioPacket. A replay window is up to an hour, so a
// loopback device that drops out early would otherwise be written out as tens
// of minutes of silence, and the mux preflight then demands hundreds of
// megabytes of free disk for a clip whose real audio is a few. Past the cap the
// pacing pointer resyncs to the incoming packet, compressing the audio
// timeline instead of inflating the temporary WAV.
constexpr int64_t kMaxSilenceGapFill100ns = 10 * kHundredNanosecondsPerSecond;

// Single-sample duration ceiling: storage-level backstop for wall-clock
// durations from coalesced hold intervals. Covers the largest healthy
// heartbeat (30 s max GOP plus low-fps coalesce slop) yet stays ~1400x below
// a day-long exclusive-fullscreen hold.
constexpr int64_t kMaxSingleVideoDuration100ns = 60 * kHundredNanosecondsPerSecond;

int64_t clampSingleVideoDuration100ns(int64_t duration100ns) {
    return duration100ns > kMaxSingleVideoDuration100ns ? kMaxSingleVideoDuration100ns : duration100ns;
}

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
            const int64_t gap100ns = std::min(packet->pts100ns, endPts100ns) - nextPts100ns;
            const int64_t fill100ns = std::min<int64_t>(gap100ns, kMaxSilenceGapFill100ns);
            const auto gapFrames = static_cast<uint64_t>(
                (fill100ns * static_cast<int64_t>(samplesPerSec)) / kHundredNanosecondsPerSecond);
            const bool silenceWritten = writer.writeSilenceFrames(gapFrames);
            nextPts100ns += static_cast<int64_t>((gapFrames * kHundredNanosecondsPerSecond) / samplesPerSec);
            if (!silenceWritten) {
                return false;
            }
            if (fill100ns < gap100ns) {
                // Cap hit: bridge with the capped silence, then resynchronize the
                // pacing pointer to the incoming packet. Without the resync every
                // later packet would still lag the residual gap and trigger
                // another capped fill, re-inflating the stream. Post-outage audio
                // then plays (gap - limit) earlier against video than captured,
                // which beats an unbounded silent WAV.
                nextPts100ns = std::min(packet->pts100ns, endPts100ns);
                Logger::instance().warning(L"replay",
                    L"Replay audio gap exceeded the silence-fill limit; audio timeline compressed: " +
                    path.filename().wstring());
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

// Audio payloads are small (a second of stereo 48 kHz 16-bit is ~192 KiB per
// track), so the audio ring needs only a fraction of the video budget. 1 MiB
// still holds several seconds per track; anything below it is not a capacity
// change at all - it turns audio disk backing off, like the video floor.
constexpr uint64_t kMinAudioRingCapacityBytes = 1024ull * 1024;

// Bound on a single ring IRP. The ring folder is user-chosen, so a USB stick,
// a network drive or a sleeping disk can leave a request pending forever, and
// both callers hold mutex_ while this runs: an unbounded wait would wedge the
// encode loop, the UI, and stopPipeline's thread joins, with no way to close the
// app. Generous enough that a merely slow removable drive is not mistaken for a
// stall, short enough that a wedged one degrades to memory rather than hanging.
constexpr DWORD kRingIoTimeoutMs = 8000;

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
            if (GetOverlappedResult(file, &overlapped, &moved, kRingIoTimeoutMs)) {
                // completed within the budget
            } else {
                // Returning here would leave the IRP writing into this frame's
                // OVERLAPPED and into the caller's buffer after both go away, so
                // cancel and then drain instead: CancelIoEx guarantees the
                // request completes, which makes this second INFINITE wait
                // return promptly with ERROR_OPERATION_ABORTED rather than
                // block. Report the transfer as failed either way, so the
                // caller falls back to memory instead of using partial bytes.
                CancelIoEx(file, &overlapped);
                GetOverlappedResult(file, &overlapped, &moved, TRUE);
                SetLastError(write ? ERROR_WRITE_FAULT : ERROR_READ_FAULT);
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

    if (!settings_.enabled) {
        video_.clear();
        systemAudio_.clear();
        microphoneAudio_.clear();
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
        openRingLocked(capacityBytes);
    } else if (capacityBytes != ringCapacityBytes_) {
        // A capacity change alone must not drop buffered video: the recorded
        // extents stay valid at their offsets, and settings_.seconds (already
        // applied above) reshapes the window on the next trim. Re-laying out a
        // ring that still holds packets would invalidate every live extent, so
        // the resize waits until there is nothing left to invalidate - which is
        // what makes editing the replay window length non-destructive.
        if (diskVideo_.empty() && !savePinsRefs_) {
            openRingLocked(capacityBytes);
        } else {
            Logger::instance().info(L"replay",
                L"Replay ring stays at " + std::to_wstring(ringCapacityBytes_ / (1024ull * 1024ull)) +
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
    } else if (audioCapacityBytes != audioRingCapacityBytes_) {
        // Same non-destructive rule as the video ring: re-laying out a ring
        // that still holds packets would invalidate every live extent.
        if (diskSystemAudio_.empty() && diskMicrophoneAudio_.empty() && !savePinsRefs_) {
            openAudioRingWithFallbackLocked(audioCapacityBytes);
        } else {
            Logger::instance().info(L"replay",
                L"Replay audio ring stays at " + std::to_wstring(audioRingCapacityBytes_ / (1024ull * 1024ull)) +
                L" MiB while buffered audio remains; the new capacity takes effect on a later settings change once the buffer has drained");
        }
    }

    trimLocked();
}

void ReplayBuffer::openRingLocked(uint64_t capacityBytes) {
    // Caller holds mutex_ and has already decided the buffer may be dropped.
    clearBufferedVideoLocked();
    closeRingLocked();
    ringCapacityBytes_ = capacityBytes;
    if (!openRingLocked()) {
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
        L"Replay video buffering moved to disk ring " + ringPath_.wstring() +
        L" (" + std::to_wstring(capacityBytes / (1024ull * 1024ull)) + L" MiB)");
}

void ReplayBuffer::openAudioRingWithFallbackLocked(uint64_t capacityBytes) {
    // Caller holds mutex_ and has already decided the audio buffer may be dropped.
    clearBufferedAudioLocked();
    closeAudioRingLocked();
    audioRingCapacityBytes_ = capacityBytes;
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
        L"Replay audio buffering moved to disk ring " + audioRingPath_.wstring() +
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
    std::error_code error;
    const auto directory = ringDirectory_ / kRingFolderName;
    std::filesystem::create_directories(directory, error);
    if (error) {
        Logger::instance().warning(L"replay",
            L"Could not create replay ring folder " + directory.wstring() +
            L" (" + utf8ToWide(error.message()) + L")");
        return false;
    }
    audioRingPath_ = directory / kAudioRingFileName;
    audioRingFile_ = CreateFileW(
        audioRingPath_.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (audioRingFile_ == INVALID_HANDLE_VALUE) {
        audioRingFile_ = nullptr;
        Logger::instance().warning(L"replay",
            L"Could not create replay audio ring file " + audioRingPath_.wstring() +
            L" (Win32 error " + std::to_wstring(GetLastError()) + L")");
        return false;
    }
    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(capacityBytes);
    if ((!SetFilePointerEx(audioRingFile_, end, nullptr, FILE_BEGIN) || !SetEndOfFile(audioRingFile_))) {
        Logger::instance().warning(L"replay",
            L"Could not pre-size replay audio ring file " + audioRingPath_.wstring() +
            L" (Win32 error " + std::to_wstring(GetLastError()) + L"); it will grow as packets arrive");
    }
    audioRingWriteOffset_ = 0;
    audioRingLiveBytes_ = 0;
    return true;
}

void ReplayBuffer::closeAudioRingLocked() noexcept {
    if (savePinsRefs_) {
        audioRingPendingClose_ = true;
        return;
    }
    closeAudioRingNowLocked();
}

void ReplayBuffer::closeAudioRingNowLocked() noexcept {
    if (audioRingFile_) {
        CloseHandle(audioRingFile_);
        audioRingFile_ = nullptr;
    }
    if (!audioRingPath_.empty()) {
        std::error_code error;
        std::filesystem::remove(audioRingPath_, error);
        // The folder is shared with the video ring, so only take it when it is
        // actually empty; otherwise the video backend's file keeps it alive.
        if (!error) {
            std::error_code directoryError;
            std::filesystem::remove(audioRingPath_.parent_path(), directoryError);
        }
        audioRingPath_.clear();
    }
    audioRingWriteOffset_ = 0;
    audioRingLiveBytes_ = 0;
    audioRingPendingClose_ = false;
}

void ReplayBuffer::breakRingForTesting() noexcept {
    std::scoped_lock lock(mutex_);
    if (ringFile_) {
        CloseHandle(ringFile_);
        ringFile_ = nullptr;
    }
}

void ReplayBuffer::breakAudioRingForTesting() noexcept {
    std::scoped_lock lock(mutex_);
    if (audioRingFile_) {
        CloseHandle(audioRingFile_);
        audioRingFile_ = nullptr;
    }
}

void ReplayBuffer::pushVideo(EncodedPacket packet) {
    if (packet.bytes.empty()) {
        return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (!settings_.enabled) {
        return;
    }
    // Storage backstop: upstream durations are wall-clock and a coalesced hold
    // may arrive oversized; healthy frames are milliseconds, so this only bites
    // rogue tails.
    if (packet.duration100ns > kMaxSingleVideoDuration100ns) {
        packet.duration100ns = kMaxSingleVideoDuration100ns;
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
        failOverToRamLocked(
            L"Replay disk buffering failed mid-session (the disk may be full); replay video is "
            L"being buffered in memory for the rest of this session. See the log for details.");
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
        diskVideo_.back().meta.duration100ns = clampSingleVideoDuration100ns(
            (std::max)(diskVideo_.back().meta.duration100ns, duration100ns));
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
    packet.duration100ns =
        clampSingleVideoDuration100ns((std::max)(packet.duration100ns, duration100ns));
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
    std::vector<DiskAudioPacket> diskSystemAudio;
    std::vector<DiskAudioPacket> diskMicrophoneAudio;
    int64_t lastSaveEndPts100ns = 0;
    uint32_t replayWindowSeconds = 0;
    uint32_t saveGopSeconds = 2;
    std::vector<uint8_t> codecHeaderBytes;
    bool diskMode = false;
    bool diskAudioMode = false;
    // Read under the same lock as the packet snapshot: a concurrent configure
    // can move or clear ringPath_, and the pin release below may clear it too.
    std::filesystem::path ringPath;
    std::filesystem::path audioRingPath;
    {
        std::scoped_lock lock(mutex_);
        diskMode = videoBackend_ == VideoBackend::Disk;
        diskAudioMode = audioBackend_ == AudioBackend::Disk;
        ringPath = ringPath_;
        audioRingPath = audioRingPath_;
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
    // A ring read failure means those packet bytes are gone for good, which is
    // a different failure from a full disk: the elementary stream is still
    // healthy, so only !videoStream cannot see it. Publishing the shorter
    // sample table would instead produce a clip whose video ends early against
    // audio running to the original end, so the save must fail. Flagged here
    // rather than folded into the stream state because the two need different
    // user-facing reasons.
    bool ringReadFailed = false;
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
            ringReadFailed = true;
            return false;
        }
        if (snapshot.second.byteCount != 0 &&
            !readRingLocked(
                snapshot.second.offset,
                ringScratch.data() + snapshot.first.byteCount,
                snapshot.second.byteCount)) {
            ringReadFailed = true;
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
        const bool probeRead =
            readRingLocked(first.first.offset, ringScratch.data(), first.first.byteCount) &&
            (first.second.byteCount == 0 ||
             readRingLocked(
                 first.second.offset,
                 ringScratch.data() + first.first.byteCount,
                 first.second.byteCount));
        // A failed probe would otherwise read as "this frame carries no
        // parameter sets" and prepend sets the dump loop is about to write
        // anyway, duplicating them in the first sample.
        ringReadFailed = !probeRead;
        firstSampleHasHeaders = probeRead && bitstreamContainsParameterSets(videoSettings.codec, ringScratch);
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
    // Reassemble disk-backed audio the same way: the pin guarantees the
    // referenced extents cannot be overwritten while they are consumed.
    bool audioRingReadFailed = false;
    if (diskAudioMode) {
        auto reassembleAudio = [&](const std::vector<DiskAudioPacket>& disk,
                                   std::vector<std::shared_ptr<const AudioPacket>>& out) {
            for (const DiskAudioPacket& entry : disk) {
                const uint32_t totalBytes = entry.first.byteCount + entry.second.byteCount;
                ringScratch.resize(totalBytes);
                if (entry.first.byteCount != 0 &&
                    !readAudioRingLocked(entry.first.offset, ringScratch.data(), entry.first.byteCount)) {
                    audioRingReadFailed = true;
                    return;
                }
                if (entry.second.byteCount != 0 &&
                    !readAudioRingLocked(
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
            // Every elementary-stream write above was verified before this
            // point, so a manifest that will not publish means the disk or the
            // folder failed, not that a scanner briefly held recovery.tsv (that
            // race is retried inside writeManifest). A directory with streams
            // but no manifest is skipped by recoverLatestFailedRecording and
            // swept as garbage later, so keeping it would only promise the user
            // a recovery that cannot happen. Remove it.
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
        const int64_t newestVideoEndPts = newest.meta.pts100ns +
            (std::max<int64_t>)(clampSingleVideoDuration100ns(newest.meta.duration100ns), 0);
        // Same window cut saveTo applies, so the reported span never exceeds
        // what a save would publish; keyed on the last GOP saveTo observed.
        const int64_t oldestAllowedEndPts = newestVideoEndPts - secondsTo100ns(settings_.seconds) -
            gopTolerance100ns((std::max<uint32_t>)(1, lastGopSeconds_));
        auto windowBegin = diskVideo_.end();
        for (auto it = diskVideo_.begin(); it != diskVideo_.end(); ++it) {
            if (it->meta.pts100ns + (std::max<int64_t>)(it->meta.duration100ns, 0) > oldestAllowedEndPts) {
                windowBegin = it;
                break;
            }
        }
        if (windowBegin != diskVideo_.end()) {
            const auto windowKeyFrame = std::find_if(
                windowBegin, diskVideo_.end(), [](const DiskVideoPacket& packet) { return packet.meta.keyFrame; });
            if (windowKeyFrame != diskVideo_.end()) {
                return (std::max<int64_t>)(0, newestVideoEndPts - windowKeyFrame->meta.pts100ns);
            }
        }
        return (std::min)(
            (std::max<int64_t>)(0, newestVideoEndPts - firstKeyFrame->meta.pts100ns),
            secondsTo100ns(settings_.seconds) +
                gopTolerance100ns((std::max<uint32_t>)(1, lastGopSeconds_)));
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
    const int64_t newestVideoEndPts = video_.back()->pts100ns +
        (std::max<int64_t>)(clampSingleVideoDuration100ns(video_.back()->duration100ns), 0);
    // Same window cut saveTo applies, so the reported span never exceeds what
    // a save would publish; keyed on the last GOP saveTo observed.
    const int64_t oldestAllowedEndPts = newestVideoEndPts - secondsTo100ns(settings_.seconds) -
        gopTolerance100ns((std::max<uint32_t>)(1, lastGopSeconds_));
    auto windowBegin = video_.end();
    for (auto it = video_.begin(); it != video_.end(); ++it) {
        if ((*it)->pts100ns + (std::max<int64_t>)((*it)->duration100ns, 0) > oldestAllowedEndPts) {
            windowBegin = it;
            break;
        }
    }
    if (windowBegin != video_.end()) {
        const auto windowKeyFrame =
            std::find_if(windowBegin, video_.end(), [](const auto& packet) { return packet->keyFrame; });
        if (windowKeyFrame != video_.end()) {
            return (std::max<int64_t>)(0, newestVideoEndPts - (*windowKeyFrame)->pts100ns);
        }
    }
    return (std::min)(
        (std::max<int64_t>)(0, newestVideoEndPts - (*firstKeyFrame)->pts100ns),
        secondsTo100ns(settings_.seconds) +
            gopTolerance100ns((std::max<uint32_t>)(1, lastGopSeconds_)));
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
            publishCountersLocked();
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

    auto trimDiskAudioQueue = [&](auto& queue) {
        const int64_t referencePts = videoBackend_ == VideoBackend::Disk
            ? (diskVideo_.empty() ? 0 : diskVideo_.back().meta.pts100ns)
            : (video_.empty() ? 0 : video_.back()->pts100ns);
        const int64_t oldestAllowed = referencePts - secondsTo100ns(settings_.seconds);
        while (!queue.empty() && diskAudioPacketEndPts100ns(queue.front()) <= oldestAllowed) {
            audioRingLiveBytes_ -= queue.front().first.byteCount + queue.front().second.byteCount;
            queue.pop_front();
        }
    };

    const bool videoEmpty = videoBackend_ == VideoBackend::Disk ? diskVideo_.empty() : video_.empty();
    if (videoEmpty) {
        systemAudio_.clear();
        microphoneAudio_.clear();
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

    trimPacketQueue(systemAudio_);
    trimPacketQueue(microphoneAudio_);
    if (!savePinsRefs_) {
        trimDiskAudioQueue(diskSystemAudio_);
        trimDiskAudioQueue(diskMicrophoneAudio_);
    }

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
    if (!savePinsRefs_) {
        auto trimDiskAudioQueueBeforeVideo = [this, videoStartPts](auto& queue) {
            while (!queue.empty() && diskAudioPacketEndPts100ns(queue.front()) <= videoStartPts) {
                audioRingLiveBytes_ -= queue.front().first.byteCount + queue.front().second.byteCount;
                queue.pop_front();
            }
        };
        trimDiskAudioQueueBeforeVideo(diskSystemAudio_);
        trimDiskAudioQueueBeforeVideo(diskMicrophoneAudio_);
    }
    publishCountersLocked();
}

void ReplayBuffer::clearBufferedVideoLocked() {
    video_.clear();
    diskVideo_.clear();
    videoKeyFrames_ = 0;
    diskVideoKeyFrames_ = 0;
    ringLiveBytes_ = 0;
    publishCountersLocked();
    // ringWriteOffset_ deliberately survives: re-zeroing it would point the
    // next push at offset 0, the most likely place for a still-pinned save to
    // have unread bytes. Resuming from the current cursor keeps the write head
    // at least a full buffer ahead of that. It does NOT make clear() safe
    // during a pin - zeroing ringLiveBytes_ makes the whole ring look free, so
    // a push could wrap onto pinned bytes. That is why every caller other than
    // failOverToRamLocked runs on the controller worker, serialized against
    // saveReplay. failOverToRamLocked is the one caller that can clear from the
    // encode thread mid-pin, and it is safe only because it immediately flips
    // the backend to RAM, so no push writes the ring again and the pinned
    // extents keep their bytes until the deferred close. Any new caller must
    // preserve one of those two properties. Only openRingLocked and
    // closeRingNowLocked reset the cursor.
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
    const auto directory = ringDirectory_ / kRingFolderName;
    std::filesystem::create_directories(directory, error);
    if (error) {
        Logger::instance().warning(L"replay",
            L"Could not create replay ring folder " + directory.wstring() +
            L" (" + utf8ToWide(error.message()) + L")");
        return false;
    }
    ringPath_ = directory / kRingFileName;
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
        // The folder is shared with the audio ring, so only take it when it
        // is actually empty; otherwise the audio backend's file keeps it
        // alive. Only attempted when the ring file itself is gone.
        const auto directory = ringPath_.parent_path();
        if (!error) {
            std::error_code directoryError;
            std::filesystem::remove(directory, directoryError);
        }
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
    audioRingLiveBytes_ = 0;
    // audioRingWriteOffset_ deliberately survives, for the same reason the
    // video cursor does in clearBufferedVideoLocked.
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
    const uint64_t capacity = audioRingCapacityBytes_;
    if (size > capacity) {
        Logger::instance().warning(L"replay",
            L"Replay audio ring packet of " + std::to_wstring(size) +
            L" bytes exceeds the entire audio ring capacity; dropping it");
        return false;
    }
    const uint64_t freeBytes = audioRingLiveBytes_ >= capacity ? 0 : capacity - audioRingLiveBytes_;
    if (freeBytes >= size) {
        return true;
    }
    while (true) {
        if (savePinsRefs_) {
            ringSpace_.wait(lock);
        } else {
            freeAudioRingSpaceLocked(size);
        }
        const uint64_t availableNow =
            audioRingLiveBytes_ >= capacity ? 0 : capacity - audioRingLiveBytes_;
        if (availableNow >= size) {
            return true;
        }
        if (!savePinsRefs_ && diskSystemAudio_.empty() && diskMicrophoneAudio_.empty()) {
            Logger::instance().warning(L"replay", L"Replay audio ring could not free enough space; dropping a packet");
            return false;
        }
    }
}

bool ReplayBuffer::appendAudioRingLocked(const AudioPacket& packet) {
    if (!audioRingFile_ && !openAudioRingLocked(audioRingCapacityBytes_)) {
        return false;
    }
    const uint64_t capacity = audioRingCapacityBytes_;
    const uint32_t size = static_cast<uint32_t>(packet.bytes.size());

    auto writeAt = [this](uint64_t offset, const uint8_t* data, DWORD byteCount) -> bool {
        return transferRing(audioRingFile_, true, offset, const_cast<uint8_t*>(data), byteCount);
    };

    const uint64_t firstSize = std::min<uint64_t>(size, capacity - audioRingWriteOffset_);
    const uint64_t secondSize = size - firstSize;
    if (firstSize > 0 && !writeAt(audioRingWriteOffset_, packet.bytes.data(), static_cast<DWORD>(firstSize))) {
        Logger::instance().warning(L"replay",
            L"Replay audio ring write failed (Win32 error " + std::to_wstring(GetLastError()) + L")");
        return false;
    }
    if (secondSize > 0 && !writeAt(0, packet.bytes.data() + firstSize, static_cast<DWORD>(secondSize))) {
        Logger::instance().warning(L"replay",
            L"Replay audio ring wrap write failed (Win32 error " + std::to_wstring(GetLastError()) + L")");
        return false;
    }

    DiskAudioPacket entry;
    entry.track = packet.track;
    entry.processId = packet.processId;
    entry.pts100ns = packet.pts100ns;
    entry.frameCount = packet.frameCount;
    entry.format = packet.format;
    entry.first = RingExtent{audioRingWriteOffset_, static_cast<uint32_t>(firstSize)};
    entry.second = RingExtent{0, static_cast<uint32_t>(secondSize)};
    audioRingWriteOffset_ = (audioRingWriteOffset_ + size) % capacity;
    audioRingLiveBytes_ += size;
    auto& queue = packet.track == AudioTrack::System ? diskSystemAudio_ : diskMicrophoneAudio_;
    queue.push_back(std::move(entry));
    trimLocked();
    return true;
}

bool ReplayBuffer::readAudioRingLocked(uint64_t offset, uint8_t* destination, uint32_t size) const {
    if (!audioRingFile_ || size == 0) {
        return size == 0;
    }
    return transferRing(audioRingFile_, false, offset, destination, size);
}

void ReplayBuffer::freeAudioRingSpaceLocked(uint64_t bytesNeeded) {
    // Audio has no keyframe realignment: every packet stands alone, so plain
    // oldest-first eviction across both tracks (by front PTS) keeps the window
    // coherent without stranding anything.
    const auto freeBytes = [this]() {
        return audioRingLiveBytes_ >= audioRingCapacityBytes_ ? 0 : audioRingCapacityBytes_ - audioRingLiveBytes_;
    };
    auto frontPts = [](const std::deque<DiskAudioPacket>& queue) {
        return queue.empty() ? (std::numeric_limits<int64_t>::max)() : queue.front().pts100ns;
    };
    while ((!diskSystemAudio_.empty() || !diskMicrophoneAudio_.empty()) && freeBytes() < bytesNeeded) {
        auto& queue = frontPts(diskSystemAudio_) <= frontPts(diskMicrophoneAudio_)
            ? diskSystemAudio_
            : diskMicrophoneAudio_;
        const DiskAudioPacket& front = queue.front();
        audioRingLiveBytes_ -= front.first.byteCount + front.second.byteCount;
        queue.pop_front();
    }
}

} // namespace backtrack
