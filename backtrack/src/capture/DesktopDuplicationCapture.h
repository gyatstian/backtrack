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

// CPU-side pointer state carried across a capture-source rebuild on the same
// output. DXGI reports the pointer shape only when it changes, so a fresh
// duplication would composite no cursor until the next pointer update - a
// visible blink on every pipeline recreate. The D3D resources are per-device
// and are rebuilt on the new device.
struct DesktopDuplicationCursorSnapshot {
    std::vector<uint8_t> shapeBuffer;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo{};
    POINT position{};
    bool visible = false;
};

struct DesktopDuplicationCursorCarry {
    HMONITOR monitor = nullptr;
    DesktopDuplicationCursorSnapshot snapshot;
    bool valid = false;

    // Only the same output keeps the last pointer state. A different target,
    // or a cleared entry after device loss, must not draw a stale cursor.
    bool restore(HMONITOR output, DesktopDuplicationCursorSnapshot& out) const {
        if (!valid || output == nullptr || output != monitor) {
            return false;
        }
        out = snapshot;
        return true;
    }

    void store(HMONITOR output, const DesktopDuplicationCursorSnapshot& in) {
        monitor = output;
        snapshot = in;
        valid = output != nullptr;
    }

    void clear() {
        monitor = nullptr;
        snapshot = {};
        valid = false;
    }
};

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
    void setCursorCaptureEnabled(bool enabled) override { captureCursor_ = enabled; }

private:
    struct TextureSlot {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11RenderTargetView> renderTargetView;
    };

    bool createTexturePool(uint32_t width, uint32_t height, DXGI_FORMAT format, uint32_t poolSize);
    bool queryAdvancedColor();
    void markLost(const wchar_t* reason, HRESULT hr);
    void releaseHeldFrame();
    void logAcquireFailureOnce(const wchar_t* reason, HRESULT hr);

    bool ensureCompositor();
    void updateCursorState(const DXGI_OUTDUPL_FRAME_INFO& frameInfo);
    bool refreshCursorShape(uint32_t bufferSize);
    bool buildCursorTexture();
    void compositeCursor(const std::shared_ptr<TextureSlot>& slot);
    // Carry the pointer shape across a rebuild on the same output (see
    // DesktopDuplicationCursorCarry).
    bool restoreCarriedCursor();
    void saveCarriedCursor();
    void clearCarriedCursor();

    // Rotated-display coordinate helpers. Desktop Duplication returns the
    // image unrotated: ModeDesc/DesktopCoordinates and the reported cursor
    // position live in physical (rotated) coordinates, while the texture,
    // pool, and GpuFrame dimensions are unrotated. Header-inline so the
    // rotation tests exercise the exact production math without a D3D device.
    static void unrotatedDimensions(DXGI_MODE_ROTATION rotation, uint32_t& width, uint32_t& height) {
        if (rotation == DXGI_MODE_ROTATION_ROTATE90 ||
            rotation == DXGI_MODE_ROTATION_ROTATE270) {
            const uint32_t temp = width;
            width = height;
            height = temp;
        }
    }

    static void transformCursorPosition(DXGI_MODE_ROTATION rotation,
        int32_t physX, int32_t physY,
        uint32_t texW, uint32_t texH,
        int32_t& outX, int32_t& outY) {
        switch (rotation) {
        case DXGI_MODE_ROTATION_ROTATE90:
            // Physical (px, py) -> Texture (tx=py, ty=Ht-px-1)
            outX = physY;
            outY = static_cast<int32_t>(texH) - 1 - physX;
            break;
        case DXGI_MODE_ROTATION_ROTATE270:
            // Physical (px, py) -> Texture (tx=Wt-py-1, ty=px)
            outX = static_cast<int32_t>(texW) - 1 - physY;
            outY = physX;
            break;
        case DXGI_MODE_ROTATION_ROTATE180:
            // Physical (px, py) -> Texture (tx=Wt-px-1, ty=Ht-py-1)
            outX = static_cast<int32_t>(texW) - 1 - physX;
            outY = static_cast<int32_t>(texH) - 1 - physY;
            break;
        default:
            outX = physX;
            outY = physY;
            break;
        }
    }

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
    bool hdrPollState_ = false;
    bool legacyDuplication_ = false;
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
    // True when desktopCopy_ does not hold the current desktop content: a frame
    // was copied straight into a slot because no cursor was drawn. A cursor that
    // reappears on a later frame must refresh desktopCopy_ before compositing.
    bool desktopCopyDirty_ = true;
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

    // Display rotation from DXGI_OUTDUPL_DESC. Desktop Duplication returns
    // the image unrotated; the rotation value describes the physical display
    // orientation. width_/height_ are the unrotated (texture) dimensions.
    // Cursor position is transformed from physical to texture coordinates
    // before compositing.
    DXGI_MODE_ROTATION rotation_ = DXGI_MODE_ROTATION_UNSPECIFIED;

    friend struct DesktopDuplicationRotationTestAccess;
};

} // namespace backtrack
