#pragma once

#include "core/Types.h"
#include "mux/WavWriter.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace backtrack {

// Open-ended window bound for paceAudioPacket: the recording path has no video
// end yet, so gaps are measured against the incoming packet alone.
inline constexpr int64_t kNoAudioWindowEnd = (std::numeric_limits<int64_t>::max)();

// Advances nextPts100ns across one packet on the shared audio timeline:
// format mismatch is the caller's responsibility (checked by the caller),
// fills the full PTS gap with silence so the saved audio keeps the real gap
// duration (a gap is never compressed against the unchanged video track),
// trims an overlap with already-written audio, and writes at most up to
// endPts100ns. The replay path passes its video end; the recording path passes
// kNoAudioWindowEnd. Returns false only on a write failure (typically disk
// full); the caller maps that onto its own failure (fail the save vs flag the
// recording). Zeroed writer timing means the writer was never opened, which
// the caller owns, so it skips rather than fails here. logSubsystem and
// fileName stay in the signature because both callers pass them; they are not
// used here anymore.
inline bool paceAudioPacket(
    WavWriter& writer,
    int64_t& nextPts100ns,
    const AudioPacket& packet,
    int64_t endPts100ns,
    const wchar_t* /*logSubsystem*/,
    const wchar_t* /*fileName*/) {
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
        // The whole gap becomes silence so post-outage audio lands at its true
        // absolute time. The old 10 s cap resynced the pacing pointer past the
        // residual gap and played resumed audio (gap - 10 s) early against the
        // unchanged video track.
        const int64_t gap100ns = std::min(packet.pts100ns, endPts100ns) - nextPts100ns;
        // A gap this large cannot be stored anyway (thousands of TB of
        // silence); saturating keeps the multiply below well-defined and lets
        // the write fail on disk-full instead of overflowing.
        const int64_t maxSafeGap100ns =
            (std::numeric_limits<int64_t>::max)() / static_cast<int64_t>(samplesPerSec);
        const int64_t fill100ns = gap100ns > maxSafeGap100ns ? maxSafeGap100ns : gap100ns;
        const auto gapFrames = static_cast<uint64_t>(
            (fill100ns * static_cast<int64_t>(samplesPerSec)) / kHundredNanosecondsPerSecond);
        if (!writer.writeSilenceFrames(gapFrames)) {
            return false;
        }
        nextPts100ns += static_cast<int64_t>((gapFrames * kHundredNanosecondsPerSecond) / samplesPerSec);
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
