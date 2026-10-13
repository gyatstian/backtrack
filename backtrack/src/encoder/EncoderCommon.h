#pragma once

#include "core/Types.h"
#include "encoder/DecodeOrderTimestamps.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace backtrack {

// Shared encoder bounds. Drain mechanisms stay per-backend (NVENC polls EOS,
// AMF runs Drain() on a helper thread); only the escalation budgets converge.
inline constexpr uint32_t kEncoderMaxConsecutiveFailures = 30;
inline constexpr auto kEncoderOutputTimeout = std::chrono::seconds(5);
inline constexpr auto kEncoderInFlightMaxAge = std::chrono::seconds(30);

// Shared profile mapping only: LowestGpu/Balanced/Custom plus the dependent
// resets (lookahead/AQ/B-frames). Capability gating stays per-backend because
// the caps each vendor reports differ.
EncoderEffectiveSettings effectiveSettingsFor(const VideoSettings& settings);

// Prepends cached parameter sets to a keyframe that lacks them so every replay
// cut point is self-contained even when a driver drops header insertion.
void ensureKeyFrameCarriesHeaders(
    VideoCodec codec,
    EncodedPacket& packet,
    const std::vector<uint8_t>& cachedParameterSets);

// Single-source packet finalize shared by both backends: explicit
// driver-timestamp validity plus fallback, decode-order DTS, the
// IDR-or-bitstream keyframe rule, header patching, parameter-set caching, and
// EMA encode-time stats. Returns false for an empty
// payload (counts a drop); the caller then skips its ready queue. The
// keyframe-metadata read stays per-backend; pass it in as metadataIdr.
bool finalizePacket(
    VideoCodec codec,
    const uint8_t* data,
    size_t size,
    int64_t driverPts100ns,
    bool hasDriverPts,
    int64_t driverDuration100ns,
    int64_t fallbackPts100ns,
    int64_t fallbackDuration100ns,
    bool metadataIdr,
    DecodeOrderTimestamps& timestamps,
    std::vector<uint8_t>& cachedParameterSets,
    EncoderStats& stats,
    std::chrono::steady_clock::time_point submittedAt,
    EncodedPacket& out);

} // namespace backtrack
