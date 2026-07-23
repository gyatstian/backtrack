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
        ComPtr<ID3D11RenderTargetView> renderTargetView;
    };

    std::shared_ptr<TextureSlot> acquireSlot();
    bool createTexturePool(uint32_t width, uint32_t height, DXGI_FORMAT format, uint32_t poolSize);

    // Cursor compositing. Desktop Duplication delivers the desktop image and the
    // mouse pointer separately; we persist the latest desktop image and blend the
    // cursor onto each emitted frame so a moving pointer over a static desktop
    // still produces fresh frames.
    bool ensureCompositor();
    void updateCursorState(const DXGI_OUTDUPL_FRAME_INFO& frameInfo);
    bool refreshCursorShape(uint32_t bufferSize);
    bool buildCursorTexture();
    void compositeCursor(const std::shared_ptr<TextureSlot>& slot);

    D3DDevice* device_ = nullptr;
    ComPtr<IDXGIOutputDuplication> duplication_;
    std::vector<std::shared_ptr<TextureSlot>> pool_;
    uint64_t poolExhaustionDrops_ = 0;
    uint64_t frameIndex_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t poolSize_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    bool deviceLost_ = false;
    bool haveContent_ = false;

    // Persistent full-desktop image; blended with the cursor into each pool slot.
    ComPtr<ID3D11Texture2D> desktopCopy_;
    ComPtr<ID3D11ShaderResourceView> desktopCopySrv_;
    bool captureCursor_ = true;

    // Latest decoded cursor shape and position.
    std::vector<uint8_t> cursorShapeBuffer_;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO cursorShapeInfo_{};
    POINT cursorPosition_{};
    bool cursorVisible_ = false;
    bool cursorShapeDirty_ = false;
    bool cursorShapeValid_ = false;
    ComPtr<ID3D11Texture2D> cursorTexture_;
    ComPtr<ID3D11ShaderResourceView> cursorSrv_;
    uint32_t cursorTexWidth_ = 0;
    uint32_t cursorTexHeight_ = 0;
    uint32_t cursorShapeType_ = 0;

    // Alpha-blend quad pipeline used to draw the cursor onto a slot.
    ComPtr<ID3D11VertexShader> cursorVs_;
    ComPtr<ID3D11PixelShader> cursorPs_;
    ComPtr<ID3D11Buffer> cursorConstantBuffer_;
    ComPtr<ID3D11BlendState> cursorBlendState_;
    ComPtr<ID3D11RasterizerState> cursorRasterizerState_;
    ComPtr<ID3D11SamplerState> cursorSampler_;
    bool compositorReady_ = false;
};

} // namespace backtrack
