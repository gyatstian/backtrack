#pragma once

#include "capture/D3DDevice.h"
#include "capture/GpuTexturePool.h"
#include "capture/HdrStability.h"
#include "capture/ICaptureSource.h"

#include <dxgi1_6.h>

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
    uint64_t resolutionChangeCount() const override { return resolutionChanges_; }
    uint64_t hdrChangeCount() const { return hdrChangeCount_; }
    bool hdrActive() const { return hdrPin_.pinnedHdr(); }
    bool hdrPinned() const override { return hdrPin_.pinnedHdr(); }
    uint64_t hdrBridgedFrames() const override { return bridgedFrames_; }
    void setCursorCaptureEnabled(bool enabled) override { captureCursor_ = enabled; }

private:
    struct TextureSlot {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11RenderTargetView> renderTargetView;
    };

    bool createTexturePool(uint32_t width, uint32_t height, DXGI_FORMAT format, uint32_t poolSize);
    bool queryAdvancedColor();
    void applyHdrUpgrade();
    void markLost(const wchar_t* reason, HRESULT hr);
    void releaseHeldFrame();
    void logAcquireFailureOnce(const wchar_t* reason, HRESULT hr);

    bool ensureCompositor();
    // SDR->scRGB bridge for a pinned HDR session: stages the SDR surface and
    // normalizes it into the HDR desktop copy. Staging/shader build runs
    // outside the context lock; normalizeIntoDesktopCopy issues context work
    // and needs the immediate-context mutex held like compositeCursor.
    bool ensureBridgeResources(uint32_t width, uint32_t height, DXGI_FORMAT sdrFormat);
    void normalizeIntoDesktopCopy();
    void updateCursorState(const DXGI_OUTDUPL_FRAME_INFO& frameInfo);
    bool refreshCursorShape(uint32_t bufferSize);
    bool buildCursorTexture();
    void compositeCursor(const std::shared_ptr<TextureSlot>& slot);

    D3DDevice* device_ = nullptr;
    HMONITOR monitor_ = nullptr;
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<IDXGIOutput6> colorProbe_; // cached HDR state source; GetDesc1 is cheap, no factory walk
    GpuTexturePool<TextureSlot> pool_;
    uint64_t frameIndex_ = 0;
    uint64_t cursorOnlyFrames_ = 0;
    std::chrono::steady_clock::time_point nextCursorOnlyFrameAt_{};
    std::chrono::nanoseconds cursorOnlyFrameInterval_{};
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t poolSize_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    // Pinned HDR interpretation (HdrSessionPin in HdrStability.h): once an
    // HDR surface is seen the session stays HDR and every frame converts
    // through the one tone-map mapping. hdrPollState_ is the raw 1 Hz
    // GetDesc1 result, diagnostics only; surfaces decide pin and upgrades.
    HdrSessionPin hdrPin_;
    bool hdrPollState_ = false;
    uint64_t hdrChangeCount_ = 0;
    // SDR surfaces normalized into a pinned HDR session (single mapping, so
    // they encode identically to native HDR frames of the same desktop).
    uint64_t bridgedFrames_ = 0;
    uint64_t resolutionChanges_ = 0;
    std::chrono::steady_clock::time_point lastHdrPollAt_{};
    bool deviceLost_ = false;
    bool haveContent_ = false;
    bool frameHeld_ = false;

    // Rate-limit repeated acquire failure logs.
    HRESULT lastLoggedAcquireHr_ = S_OK;
    uint64_t suppressedAcquireLogs_ = 0;
    std::chrono::steady_clock::time_point lastAcquireLogAt_{};

    ComPtr<ID3D11Texture2D> desktopCopy_;
    ComPtr<ID3D11ShaderResourceView> desktopCopySrv_;
    ComPtr<ID3D11RenderTargetView> desktopCopyRtv_; // bridge render target into desktopCopy_
    // Bridge staging: last SDR surface, normalized per desktop change. Format
    // follows the arriving SDR surface (BGRA in practice); one texture, so a
    // format wobble recreates this instead of the 500 MB pool.
    ComPtr<ID3D11Texture2D> bridgeStaging_;
    ComPtr<ID3D11ShaderResourceView> bridgeStagingSrv_;
    DXGI_FORMAT bridgeStagingFormat_ = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11VertexShader> bridgeVs_;
    ComPtr<ID3D11PixelShader> bridgePs_;
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
