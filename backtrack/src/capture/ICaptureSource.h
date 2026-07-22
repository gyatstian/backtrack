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
};

} // namespace backtrack
