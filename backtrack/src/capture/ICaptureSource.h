#pragma once

#include "core/Types.h"

namespace backtrack {

class D3DDevice;

class ICaptureSource {
public:
    virtual ~ICaptureSource() = default;

    virtual bool initialize(D3DDevice& device, const AppSettings& settings, const CaptureTarget& target) = 0;
    virtual bool acquireNextFrame(GpuFrame& frame, uint32_t timeoutMs) = 0;
    virtual void shutdown() = 0;
    virtual bool isDeviceLost() const = 0;
    virtual CaptureBackend backend() const = 0;
    virtual uint32_t width() const = 0;
    virtual uint32_t height() const = 0;
    virtual uint64_t cursorOnlyFrames() const { return 0; }
    // Counts in-place source resolution changes handled without a rebind
    // (e.g. the WGC FramePool.Recreate path on a display-mode change). The
    // capture loop requests an encoder keyframe whenever this advances so the
    // size jump never lands mid-GOP.
    virtual uint64_t resolutionChangeCount() const { return 0; }
    // Applies a capture-cursor preference change to the running source
    // without a backend rebind. Default: nothing to do (the preference is
    // consumed at initialize time).
    virtual void setCursorCaptureEnabled(bool /*enabled*/) {}
};

} // namespace backtrack
