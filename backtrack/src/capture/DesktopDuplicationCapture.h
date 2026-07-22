#pragma once

#include "capture/D3DDevice.h"
#include "capture/ICaptureSource.h"

#include <memory>
#include <vector>

namespace backtrack {

class DesktopDuplicationCapture final : public ICaptureSource {
public:
    bool initialize(D3DDevice& device, const AppSettings& settings, const CaptureTarget& target) override;
    bool acquireNextFrame(GpuFrame& frame, uint32_t timeoutMs) override;
    void shutdown() override;
    bool isDeviceLost() const override { return deviceLost_; }
    CaptureBackend backend() const override { return CaptureBackend::DesktopDuplication; }
    uint32_t width() const override { return width_; }
    uint32_t height() const override { return height_; }

private:
    struct TextureSlot {
        ComPtr<ID3D11Texture2D> texture;
    };

    std::shared_ptr<TextureSlot> acquireSlot();
    bool createTexturePool(uint32_t width, uint32_t height, DXGI_FORMAT format, uint32_t poolSize);

    D3DDevice* device_ = nullptr;
    ComPtr<IDXGIOutputDuplication> duplication_;
    std::vector<std::shared_ptr<TextureSlot>> pool_;
    uint64_t poolExhaustionDrops_ = 0;
    uint64_t frameIndex_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    bool deviceLost_ = false;
};

} // namespace backtrack
