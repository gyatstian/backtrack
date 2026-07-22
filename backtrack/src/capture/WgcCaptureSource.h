#pragma once

#include "capture/ICaptureSource.h"

#include <memory>

namespace backtrack {

class WgcCaptureSource final : public ICaptureSource {
public:
    WgcCaptureSource();
    ~WgcCaptureSource() override;

    bool initialize(D3DDevice& device, const AppSettings& settings, const CaptureTarget& target) override;
    bool acquireNextFrame(GpuFrame& frame, uint32_t timeoutMs) override;
    void shutdown() override;
    bool isDeviceLost() const override;
    CaptureBackend backend() const override { return CaptureBackend::WindowsGraphicsCapture; }
    uint32_t width() const override;
    uint32_t height() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace backtrack
