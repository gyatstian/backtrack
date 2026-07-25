#pragma once

#include "capture/D3DDevice.h"
#include "capture/ICaptureSource.h"

#include <chrono>
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
    uint64_t cursorOnlyFrames() const override { return cursorOnlyFrames_; }

    // True after at least one frame with a desktop present (not cursor-only).
    bool hasPresentedContent() const { return havePresentedContent_; }

private:
    struct TextureSlot {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11RenderTargetView> renderTargetView;
    };

    std::shared_ptr<TextureSlot> acquireSlot();
    bool createTexturePool(uint32_t width, uint32_t height, DXGI_FORMAT format, uint32_t poolSize);
    void markLost(const wchar_t* reason, HRESULT hr);
    void releaseHeldFrame();
    void logAcquireFailureOnce(const wchar_t* reason, HRESULT hr);

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
    uint64_t cursorOnlyFrames_ = 0;
    std::chrono::steady_clock::time_point nextCursorOnlyFrameAt_{};
    std::chrono::nanoseconds cursorOnlyFrameInterval_{};
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t poolSize_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    bool deviceLost_ = false;
    bool haveContent_ = false;
    bool havePresentedContent_ = false;
    bool frameHeld_ = false;

    // Rate-limit repeated acquire failure logs.
    HRESULT lastLoggedAcquireHr_ = S_OK;
    uint64_t suppressedAcquireLogs_ = 0;
    std::chrono::steady_clock::time_point lastAcquireLogAt_{};

    ComPtr<ID3D11Texture2D> desktopCopy_;
    ComPtr<ID3D11ShaderResourceView> desktopCopySrv_;
    bool captureCursor_ = true;

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

    ComPtr<ID3D11VertexShader> cursorVs_;
    ComPtr<ID3D11PixelShader> cursorPs_;
    ComPtr<ID3D11Buffer> cursorConstantBuffer_;
    ComPtr<ID3D11BlendState> cursorBlendState_;
    ComPtr<ID3D11RasterizerState> cursorRasterizerState_;
    ComPtr<ID3D11SamplerState> cursorSampler_;
    bool compositorReady_ = false;
};

} // namespace backtrack
