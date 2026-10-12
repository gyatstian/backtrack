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

// One knob for every capture and scaler texture pool, so a queue-limit
// mapping change is a single edit here. (RecorderController keeps its own
// copy for the scaler feed; it is the only other site.)
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
