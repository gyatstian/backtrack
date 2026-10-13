#pragma once

#include "capture/D3DDevice.h"
#include "capture/GpuTexturePool.h"
#include "core/Types.h"

#include <d3d11_1.h>

#include <chrono>
#include <memory>
#include <unordered_map>

namespace backtrack {

class D3D11FrameScaler {
public:
    bool scale(
        D3DDevice& device,
        const GpuFrame& input,
        uint32_t outputWidth,
        uint32_t outputHeight,
        GpuFrame& output,
        bool fitWithBars = false,
        bool forceOutputTexture = false,
        DXGI_FORMAT preferredOutputFormat = DXGI_FORMAT_B8G8R8A8_UNORM);
    // Bumped by every output-pool teardown (reconfigure, reset, setPoolSize).
    // A change across a scale() call means the pool was rebuilt, so any GPU
    // registration the consumer still holds for those textures is dangling and
    // must be dropped before the new pool is consumed.
    uint64_t poolGeneration() const;
    void setPoolSize(uint32_t poolSize);
    void reset();
    bool hdrInputRejected() const;
    bool hdrMappingActive() const;

private:
    struct TextureSlot {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11RenderTargetView> renderTargetView;
        ComPtr<ID3D11VideoProcessorOutputView> outputView;
    };

    bool ensureInitialized(
        D3DDevice& device,
        uint32_t inputWidth,
        uint32_t inputHeight,
        DXGI_FORMAT inputFormat,
        uint32_t outputWidth,
        uint32_t outputHeight,
        DXGI_FORMAT outputFormat);
    // Allocates the texture pool for the same-size, same-format CopyResource
    // fast-path, which does not need the video-processor stack.
    bool ensureCopyPool(
        D3DDevice& device,
        uint32_t width,
        uint32_t height,
        DXGI_FORMAT format);
    // One VP attempt. RetrySdr latches the HDR SDR-fallback (never BGRA);
    // DowngradeBgra latches the NV12->BGRA fallback; a failure yields exactly
    // one of them. No teardown runs under the context lock; the caller retries
    // with plain sequential calls.
    enum class VpBltOutcome { Ok, Drop, RetrySdr, DowngradeBgra };
    VpBltOutcome tryVpBlt(
        D3DDevice& device,
        const GpuFrame& input,
        uint32_t outputWidth,
        uint32_t outputHeight,
        DXGI_FORMAT outputFormat,
        GpuFrame& output,
        bool fitWithBars,
        bool& hdrRetryAfterCooldown);
    // Same-size, same-format CopyResource path. Outside the VP retry budget.
    bool tryFastCopy(
        D3DDevice& device,
        const GpuFrame& input,
        uint32_t outputWidth,
        uint32_t outputHeight,
        DXGI_FORMAT outputFormat,
        GpuFrame& output);
    void releaseResources();
    ID3D11VideoProcessorInputView* inputViewFor(D3DDevice& device, ID3D11Texture2D* texture);
    ID3D11VideoProcessorInputView* copiedInputViewFor(D3DDevice& device, ID3D11Texture2D* texture);
    // Drops cached input views whose source texture is no longer referenced by
    // anyone except this cache (and the view itself). Views pin their texture,
    // so stale entries hold dead GPU surfaces in VRAM until pruned.
    void pruneStaleInputViews();

    // Sticky video-processor state. The ID3D11VideoContext Set* calls below
    // configure per-processor state that persists across blits; reissuing them
    // every frame costs ~8 driver calls for values that only change when the
    // processor is recreated or an input classification flips. The applied
    // configuration is cached in vpState_ and diffed against the frame's
    // desired state instead.
    struct VpState {
        // False after processor recreation until the state is (re)programmed.
        bool valid = false;
        bool colorSpace1Path = false; // ID3D11VideoContext1 present
        uint32_t inputWidth = 0;
        uint32_t inputHeight = 0;
        uint32_t outputWidth = 0;
        uint32_t outputHeight = 0;
        DXGI_FORMAT inputFormat = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT outputFormat = DXGI_FORMAT_UNKNOWN;
        bool hdr = false;
        bool hdrRejected = false;
        bool fitWithBars = false;
        RECT sourceRect{};
        RECT targetRect{};
        RECT outputRect{};

        bool equals(const VpState& other) const;
    };

    void applyVideoProcessorState(const VpState& state);

    ComPtr<ID3D11VideoDevice> videoDevice_;
    ComPtr<ID3D11VideoContext> videoContext_;
    ComPtr<ID3D11VideoContext1> videoContext1_;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    ComPtr<ID3D11VideoProcessor> processor_;
    ComPtr<ID3D11Texture2D> copiedInputTexture_;
    ComPtr<ID3D11VideoProcessorInputView> copiedInputView_;
    GpuTexturePool<TextureSlot> pool_;
    std::unordered_map<ID3D11Texture2D*, ComPtr<ID3D11VideoProcessorInputView>> inputViews_;
    // Cleared in releaseResources so every processor rebuild retries the
    // direct path; set when CreateVideoProcessorInputView rejects the
    // capture surface.
    bool useCopiedInput_ = false;
    bool nv12Unavailable_ = false;
    // Sticky with cooldown: set when the video processor rejected an HDR
    // (PQ/scRGB) input color space, so later frames fall back to the plain
    // SDR interpretation instead of failing every frame. Auto-clears on the
    // next HDR frame after the cooldown or when input format/dims change.
    bool hdrInputRejected_ = false;
    std::chrono::steady_clock::time_point hdrRejectedAt_{};
    DXGI_FORMAT hdrRejectedFormat_ = DXGI_FORMAT_UNKNOWN;
    uint32_t hdrRejectedWidth_ = 0;
    uint32_t hdrRejectedHeight_ = 0;
    uint32_t inputWidth_ = 0;
    uint32_t inputHeight_ = 0;
    DXGI_FORMAT inputFormat_ = DXGI_FORMAT_UNKNOWN;
    uint32_t outputWidth_ = 0;
    uint32_t outputHeight_ = 0;
    DXGI_FORMAT outputFormat_ = DXGI_FORMAT_UNKNOWN;
    uint32_t poolSize_ = 8;
    uint64_t poolGeneration_ = 0;

    // Video-processor configuration currently programmed into processor_.
    // Invalidated whenever the processor is recreated (releaseResources).
    VpState vpState_;
    // True when the state currently applied to the processor maps an HDR
    // input color space; drives the tone-map-rejection fallback when a Blt
    // fails under that mapping.
    bool appliedHdrInputSpace_ = false;
};

} // namespace backtrack
