#pragma once

#include "core/Logger.h"
#include "core/Types.h"
#include "mux/WavWriter.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace backtrack {

// Hard limit on PTS gap-fill, shared by the recording path
// (Mp4Muxer::writeTimelineAudioPacket) and the replay path
// (ReplayBuffer::writeTimelineAudio). A replay window runs up to an hour, so
// one unavailable loopback device would otherwise be written out as tens of
// minutes of silence, and the mux free-space preflight then demands hundreds
// of megabytes for a clip whose real audio is a few. Past the cap the pacing
// pointer resyncs to the incoming packet, compressing the audio timeline
// instead of inflating the temporary WAV.
inline constexpr int64_t kMaxSilenceGapFill100ns = 10 * kHundredNanosecondsPerSecond;

// Open-ended window bound for paceAudioPacket: the recording path has no video
// end yet, so gaps are measured against the incoming packet alone.
inline constexpr int64_t kNoAudioWindowEnd = (std::numeric_limits<int64_t>::max)();

// Advances nextPts100ns across one packet on the shared audio timeline:
// drops format-mismatched callers' responsibility (checked by the caller),
// fills a PTS gap with at most kMaxSilenceGapFill100ns of silence, trims an
// overlap with already-written audio, and writes at most up to endPts100ns.
// The replay path passes its video end; the recording path passes
// kNoAudioWindowEnd. Returns false only on a write failure (typically disk
// full); the caller maps that onto its own failure (fail the save vs flag the
// recording). Zeroed writer timing means the writer was never opened, which
// the caller owns, so it skips rather than fails here.
inline bool paceAudioPacket(
    WavWriter& writer,
    int64_t& nextPts100ns,
    const AudioPacket& packet,
    int64_t endPts100ns,
    const wchar_t* logSubsystem,
    const wchar_t* fileName) {
    if (packet.frameCount == 0 || packet.bytes.empty()) {
        return true;
    }
    const uint32_t samplesPerSec = writer.samplesPerSec();
    const uint16_t blockAlign = writer.blockAlign();
    if (samplesPerSec == 0 || blockAlign == 0) {
        return true;
    }

    const int64_t packetDuration100ns =
        static_cast<int64_t>((static_cast<int64_t>(packet.frameCount) * kHundredNanosecondsPerSecond) / samplesPerSec);
    const int64_t packetEnd100ns = packet.pts100ns + packetDuration100ns;
    if (packetEnd100ns <= nextPts100ns) {
        return true;
    }
    if (packet.pts100ns >= endPts100ns) {
        return true;
    }

    if (packet.pts100ns > nextPts100ns) {
        const int64_t gap100ns = std::min(packet.pts100ns, endPts100ns) - nextPts100ns;
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
            nextPts100ns = std::min(packet.pts100ns, endPts100ns);
            Logger::instance().warning(logSubsystem,
                std::wstring(L"Audio gap exceeded the silence-fill limit; audio timeline compressed: ") +
                fileName);
        }
        if (nextPts100ns >= endPts100ns) {
            return true;
        }
    }

    uint32_t startFrame = 0;
    if (packet.pts100ns < nextPts100ns) {
        startFrame = static_cast<uint32_t>(std::min<uint64_t>(
            packet.frameCount,
            (((nextPts100ns - packet.pts100ns) * static_cast<int64_t>(samplesPerSec)) + kHundredNanosecondsPerSecond - 1) /
                kHundredNanosecondsPerSecond));
    }
    if (startFrame >= packet.frameCount) {
        return true;
    }

    const size_t byteOffset = static_cast<size_t>(startFrame) * blockAlign;
    if (byteOffset >= packet.bytes.size()) {
        return true;
    }
    const int64_t writeStartPts100ns =
        packet.pts100ns +
        static_cast<int64_t>((static_cast<int64_t>(startFrame) * kHundredNanosecondsPerSecond) / samplesPerSec);
    if (writeStartPts100ns >= endPts100ns) {
        return true;
    }
    uint64_t frameLimit = static_cast<uint64_t>((std::numeric_limits<size_t>::max)() / blockAlign);
    if (endPts100ns != kNoAudioWindowEnd) {
        // writeStartPts100ns < endPts100ns was checked above, so the
        // difference is positive and small; the open-ended path skips this
        // math entirely rather than overflowing int64.
        const uint64_t framesToWindowEnd = static_cast<uint64_t>(
            ((endPts100ns - writeStartPts100ns) * static_cast<int64_t>(samplesPerSec)) /
            kHundredNanosecondsPerSecond);
        if (framesToWindowEnd == 0) {
            return true;
        }
        frameLimit = std::min(frameLimit, framesToWindowEnd);
    }
    const size_t bytesToWrite = std::min(
        {packet.bytes.size() - byteOffset,
         static_cast<size_t>(packet.frameCount - startFrame) * blockAlign,
         static_cast<size_t>(frameLimit) * blockAlign});
    if (!writer.write(packet.bytes.data() + byteOffset, bytesToWrite)) {
        return false;
    }
    const uint64_t framesWritten = bytesToWrite / blockAlign;
    nextPts100ns += static_cast<int64_t>((framesWritten * kHundredNanosecondsPerSecond) / samplesPerSec);
    return true;
}

} // namespace backtrack
