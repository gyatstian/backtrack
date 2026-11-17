#pragma once

#include "core/Logger.h"

#include <dxgiformat.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace backtrack {

// Bounds for every texture pool sized from the frame queue limit. The limit
// itself is clamped to 1..8 in sanitizeSettings; +4 covers the slot the
// capture loop pins as lastFrame, so the live range is 5..12.
inline constexpr uint32_t kMinTexturePoolSize = 5;
inline constexpr uint32_t kMaxTexturePoolSize = 16;

// Ceiling for the encoder-feeding pool (the scaler's output pool). Capture
// and source pools stay capped at kMaxTexturePoolSize: they only feed the
// scaler and never see the encoder's reorder retention. The scaler output is
// the last pool the encoder reads from, so it must hold every accepted input
// the encoder retains before producing output (NVENC lookahead can pin 31)
// plus the queue and the capture loop's held/scale slot. 64 covers the
// deepest NVENC budget: 31 lookahead + 2 B-frame + 4 bitstream buffers + 8
// queue limit + 1 held-frame slack.
inline constexpr uint32_t kMaxEncoderFeedingPoolSize = 64;

// Sizes the encoder-feeding pool from the negotiated input-retention budget:
// queued frames each pin one slot, the encoder retains `encoderRetention`
// accepted inputs (IEncoder::inputRetentionBudget) before producing output,
// and one slot is held as the in-flight scale / lastFrame slack. The queue
// derived capture pools are NOT grown by this; only the final feeding pool.
inline uint32_t encoderFeedingPoolSizeFor(
    uint32_t frameQueueLimit,
    uint32_t encoderRetention) {
    return std::clamp<uint32_t>(
        frameQueueLimit + encoderRetention + 1,
        kMinTexturePoolSize,
        kMaxEncoderFeedingPoolSize);
}

// One knob for every capture and scaler texture pool, so a queue-limit
// mapping change is a single edit here. (RecorderController keeps its own
// copy as the pre-encoder default; the scaler's encoder feed now uses
// encoderFeedingPoolSizeFor above.)
inline uint32_t texturePoolSizeForQueueLimit(uint32_t frameQueueLimit) {
    return std::clamp<uint32_t>(
        frameQueueLimit + 4, kMinTexturePoolSize, kMaxTexturePoolSize);
}

// WGC Direct3D11CaptureFramePool bound: in-flight zero-copy leases must never
// exhaust it, so it stays smaller (3..8) than the texture pool.
inline uint32_t wgcFramePoolSizeForQueueLimit(uint32_t frameQueueLimit) {
    return std::clamp<uint32_t>(frameQueueLimit + 2, 3, 8);
}

// Zero-copy leases must leave one WGC pool slot free for FrameArrived.
inline uint32_t wgcMaxZeroCopyLeases(uint32_t framePoolSize) {
    return framePoolSize > 1 ? framePoolSize - 1 : 1;
}

// How many inputs the source side can pin when a frame is not copied: the WGC
// zero-copy leases plus the queue-derived capture pool. This matters because a
// same-size, same-format WGC frame skips the scaler and aliases the source
// texture, so it pins a source-side slot instead of a scaler output slot and
// never reaches the (enlarged) encoder-feeding pool.
inline uint32_t wgcAliasInputCapacity(uint32_t frameQueueLimit) {
    return wgcMaxZeroCopyLeases(wgcFramePoolSizeForQueueLimit(frameQueueLimit)) +
        texturePoolSizeForQueueLimit(frameQueueLimit);
}

// The alias path bypasses the encoder-feeding pool, so a deep-lookahead
// encoder that retains more inputs than the source side can pin would starve
// the one pool the alias does use. False means the caller must take the owned
// (scaler output) path for that pipeline run.
inline bool encoderAliasFitsRetention(uint32_t aliasCapacity, uint32_t encoderRetention) {
    return encoderRetention <= aliasCapacity;
}

// Shared 1st + every-300th drop logging for every pool exhaustion counter,
// including the WGC zero-copy lease cap, which is not a texture pool.
inline bool gpuDropShouldLog(uint64_t& count) {
    ++count;
    return count == 1 || (count % 300) == 0;
}

inline uint64_t dxgiTextureBytes(uint32_t width, uint32_t height, DXGI_FORMAT format) {
    const uint64_t pixels = static_cast<uint64_t>(width) * height;
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        return pixels * 8;
    }
    if (format == DXGI_FORMAT_NV12) {
        return pixels * 3 / 2;
    }
    return pixels * 4; // BGRA, R10G10B10A2, and anything else pooled here
}

// Fixed GPU texture pool shared by both capture sources and the scaler:
// free-slot scan (use_count == 1), unified exhaustion logging, and VRAM
// footprint tracking. Slot setup stays with the owner; only the pool
// mechanics are shared.
template <typename Slot>
class GpuTexturePool {
public:
    std::shared_ptr<Slot> acquire() {
        for (auto& slot : slots_) {
            if (slot.use_count() == 1) {
                return slot;
            }
        }
        return {};
    }

    // Logs the 1st and every 300th exhaustion under the owner's pool name and
    // resets the counter on success, matching the old per-owner behavior.
    std::shared_ptr<Slot> acquireOrLog(const wchar_t* poolName) {
        auto slot = acquire();
        if (!slot) {
            if (gpuDropShouldLog(exhaustionDrops_)) {
                Logger::instance().warning(L"capture",
                    std::wstring(poolName) + L"; dropping frame count=" +
                    std::to_wstring(exhaustionDrops_));
            }
            return {};
        }
        exhaustionDrops_ = 0;
        return slot;
    }

    void push_back(std::shared_ptr<Slot> slot) {
        slots_.push_back(std::move(slot));
    }
    bool empty() const {
        return slots_.empty();
    }
    size_t size() const {
        return slots_.size();
    }
    void clear() {
        slots_.clear();
        footprintBytes_ = 0;
        exhaustionDrops_ = 0;
    }

    // VRAM held by pooled slot textures, set by the owner when it builds the
    // pool. Read in the pool-build debug logs.
    void setFootprintBytes(uint64_t bytes) {
        footprintBytes_ = bytes;
    }
    uint64_t footprintBytes() const {
        return footprintBytes_;
    }

private:
    std::vector<std::shared_ptr<Slot>> slots_;
    uint64_t footprintBytes_ = 0;
    uint64_t exhaustionDrops_ = 0;
};

} // namespace backtrack
