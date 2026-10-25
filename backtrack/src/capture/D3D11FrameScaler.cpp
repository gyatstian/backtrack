#include "capture/D3D11FrameScaler.h"

#include "capture/GpuPoolBuilder.h"
#include "core/Logger.h"
#include "platform/Win32Util.h"

#include <algorithm>
#include <vector>

namespace backtrack {

namespace {

constexpr size_t kMaxInputViewCacheEntries = 64;

// HDR rejection backoff lives in capture/HdrStability.h (shared, tested): a
// fixed retry window re-alternates tone-mapped and SDR frames on drivers
// that only sometimes accept the HDR input space.

// Only a refusal of the HDR color space latches the sticky SDR fallback.
// Anything else under an HDR mapping (device loss, timeout, ...) is
// transient: the next HDR frame retries tone-mapping.
bool isHdrRejectionHr(HRESULT hr) {
    return hr == E_INVALIDARG || hr == DXGI_ERROR_UNSUPPORTED;
}

} // namespace

bool D3D11FrameScaler::scale(
    D3DDevice& device,
    const GpuFrame& input,
    uint32_t outputWidth,
    uint32_t outputHeight,
    GpuFrame& output,
    bool fitWithBars,
    bool forceOutputTexture,
    DXGI_FORMAT preferredOutputFormat) {
    if (!input.texture || input.width == 0 || input.height == 0 ||
        outputWidth == 0 || outputHeight == 0) {
        return false;
    }

    DXGI_FORMAT outputFormat =
        preferredOutputFormat == DXGI_FORMAT_NV12 && nv12Unavailable_
            ? DXGI_FORMAT_B8G8R8A8_UNORM
            : preferredOutputFormat;

    // Budget: initial attempt + NV12->BGRA fallback + HDR-rejection retry.
    bool hdrRetryAfterCooldown = false;
    if (hdrInputRejected_ && input.hdr) {
        const bool formatChanged = input.format != hdrRejectedFormat_ ||
            input.width != hdrRejectedWidth_ || input.height != hdrRejectedHeight_;
        if (formatChanged) {
            // Fresh format, fresh chance: retry tone-mapping immediately.
            hdrInputRejected_ = false;
            hdrRejectStrikes_ = 0;
            hdrRetryAfterCooldown = true;
        } else if (std::chrono::steady_clock::now() - hdrRejectedAt_ >=
            hdrRejectionCooldown(hdrRejectStrikes_)) {
            hdrInputRejected_ = false;
            hdrRetryAfterCooldown = true;
        }
    }

    // Same-size, same-format frames skip the video processor entirely. This
    // fast path never consumed the retry budget, so it runs before the slow
    // path instead of inside its loop.
    if (input.width == outputWidth && input.height == outputHeight &&
        input.format == outputFormat) {
        if (!forceOutputTexture) {
            output = input;
            return true;
        }
        return tryFastCopy(device, input, outputWidth, outputHeight, outputFormat, output);
    }

    // Slow path: one VP attempt, then at most one SDR retry (HDR rejection)
    // and one BGRA downgrade (NV12 failure) as plain sequential calls.
    VpBltOutcome outcome = tryVpBlt(
        device, input, outputWidth, outputHeight, outputFormat, output,
        fitWithBars, hdrRetryAfterCooldown);
    if (outcome == VpBltOutcome::RetrySdr) {
        outcome = tryVpBlt(
            device, input, outputWidth, outputHeight, outputFormat, output,
            fitWithBars, hdrRetryAfterCooldown);
    }
    if (outcome == VpBltOutcome::DowngradeBgra && outputFormat == DXGI_FORMAT_NV12) {
        nv12Unavailable_ = true;
        Logger::instance().warning(L"capture",
            L"Preferred NV12 encoder input failed at runtime; falling back to BGRA for this capture session");
        releaseResources();
        outputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        // The downgrade can make a same-size BGRA frame copyable directly.
        if (input.width == outputWidth && input.height == outputHeight &&
            input.format == outputFormat) {
            if (!forceOutputTexture) {
                output = input;
                return true;
            }
            return tryFastCopy(device, input, outputWidth, outputHeight, outputFormat, output);
        }
        outcome = tryVpBlt(
            device, input, outputWidth, outputHeight, outputFormat, output,
            fitWithBars, hdrRetryAfterCooldown);
        if (outcome == VpBltOutcome::RetrySdr) {
            outcome = tryVpBlt(
                device, input, outputWidth, outputHeight, outputFormat, output,
                fitWithBars, hdrRetryAfterCooldown);
        }
    }

    return outcome == VpBltOutcome::Ok;
}

bool D3D11FrameScaler::tryFastCopy(
    D3DDevice& device,
    const GpuFrame& input,
    uint32_t outputWidth,
    uint32_t outputHeight,
    DXGI_FORMAT outputFormat,
    GpuFrame& output) {
    // Sources such as Desktop Duplication must outlive their acquire call,
    // but a same-format, same-size frame does not need VideoProcessorBlt.
    // The copy path still needs pooled output textures; build them here
    // since ensureInitialized (and its pool allocation) is skipped.
    if (!ensureCopyPool(device, outputWidth, outputHeight, outputFormat)) {
        return false;
    }
    auto slot = pool_.acquireOrLog(L"D3D11 scaler texture pool exhausted");
    if (!slot) {
        return false;
    }
    {
        std::scoped_lock lock(device.immediateContextMutex());
        device.context()->CopyResource(slot->texture.Get(), input.texture.Get());
    }
    output = input;
    output.texture = slot->texture;
    output.lease = slot;
    output.format = outputFormat;
    return true;
}

D3D11FrameScaler::VpBltOutcome D3D11FrameScaler::tryVpBlt(
    D3DDevice& device,
    const GpuFrame& input,
    uint32_t outputWidth,
    uint32_t outputHeight,
    DXGI_FORMAT outputFormat,
    GpuFrame& output,
    bool fitWithBars,
    bool& hdrRetryAfterCooldown) {
    if (!ensureInitialized(
            device,
            input.width,
            input.height,
            input.format,
            outputWidth,
            outputHeight,
            outputFormat)) {
        // Init failure under NV12 is an output-path failure, not an HDR one.
        return outputFormat == DXGI_FORMAT_NV12 ? VpBltOutcome::DowngradeBgra : VpBltOutcome::Drop;
    }

    auto slot = pool_.acquireOrLog(L"D3D11 scaler texture pool exhausted");
    if (!slot) {
        return VpBltOutcome::Drop;
    }

    RECT sourceRect{
        0,
        0,
        static_cast<LONG>(input.width),
        static_cast<LONG>(input.height),
    };
    RECT targetRect{
        0,
        0,
        static_cast<LONG>(outputWidth),
        static_cast<LONG>(outputHeight),
    };
    if (fitWithBars) {
        const bool sourceWider =
            static_cast<uint64_t>(input.width) * static_cast<uint64_t>(outputHeight) >
            static_cast<uint64_t>(outputWidth) * static_cast<uint64_t>(input.height);
        uint32_t targetWidth = outputWidth;
        uint32_t targetHeight = outputHeight;
        if (sourceWider) {
            targetHeight = std::max<uint32_t>(
                1,
                static_cast<uint32_t>(
                    (static_cast<uint64_t>(outputWidth) * static_cast<uint64_t>(input.height)) /
                    static_cast<uint64_t>(input.width)));
        } else {
            targetWidth = std::max<uint32_t>(
                1,
                static_cast<uint32_t>(
                    (static_cast<uint64_t>(outputHeight) * static_cast<uint64_t>(input.width)) /
                    static_cast<uint64_t>(input.height)));
        }
        targetRect.left = static_cast<LONG>((outputWidth - targetWidth) / 2);
        targetRect.top = static_cast<LONG>((outputHeight - targetHeight) / 2);
        targetRect.right = targetRect.left + static_cast<LONG>(targetWidth);
        targetRect.bottom = targetRect.top + static_cast<LONG>(targetHeight);
    }
    RECT outputRect{
        0,
        0,
        static_cast<LONG>(outputWidth),
        static_cast<LONG>(outputHeight),
    };

    // Program the processor only when the desired state differs
    // from what is already applied: steady-state blits skip all
    // eight driver calls (see VpState).
    VpState desired;
    desired.colorSpace1Path = videoContext1_ != nullptr;
    desired.inputWidth = input.width;
    desired.inputHeight = input.height;
    desired.outputWidth = outputWidth;
    desired.outputHeight = outputHeight;
    desired.inputFormat = input.format;
    desired.outputFormat = outputFormat;
    desired.hdr = input.hdr;
    desired.hdrRejected = hdrInputRejected_;
    desired.fitWithBars = fitWithBars;
    desired.sourceRect = sourceRect;
    desired.targetRect = targetRect;
    desired.outputRect = outputRect;

    ID3D11VideoProcessorInputView* inputView = nullptr;
    bool inputViewFailed = false;
    HRESULT bltHr = S_OK;
    {
        std::scoped_lock lock(device.immediateContextMutex());
        inputView = inputViewFor(device, input.texture.Get());
        if (!inputView) {
            // Input-side failure (unknown capture surface), not an NV12
            // output failure: drop the frame without latching BGRA.
            inputViewFailed = true;
        } else {
            if (fitWithBars && slot->renderTargetView) {
                constexpr FLOAT black[] = {0.0f, 0.0f, 0.0f, 1.0f};
                device.context()->ClearRenderTargetView(
                    slot->renderTargetView.Get(), black);
            }
            if (!vpState_.valid || !vpState_.equals(desired)) {
                applyVideoProcessorState(desired);
            }

            D3D11_VIDEO_PROCESSOR_STREAM stream{};
            stream.Enable = TRUE;
            stream.pInputSurface = inputView;

            bltHr = videoContext_->VideoProcessorBlt(
                processor_.Get(), slot->outputView.Get(), 0, 1, &stream);
        }
    } // Lock released before any teardown below: destroying the enumerator
      // under the context mutex serialized teardown behind in-flight blits.

    if (inputViewFailed) {
        return VpBltOutcome::Drop;
    }
    if (FAILED(bltHr)) {
        // An HDR color-space rejection is not an NV12-path failure: the
        // processor accepted RGB→NV12 and only refused tone-mapping. Latch
        // the sticky SDR mapping and retry in-call, keeping NV12 instead of
        // degrading to BGRA. The retry reprograms state (hdrRejected flips
        // the cached VpState) with no pool rebuild.
        if (appliedHdrInputSpace_ && !hdrInputRejected_ && isHdrRejectionHr(bltHr)) {
            hdrInputRejected_ = true;
            ++hdrRejectStrikes_;
            hdrRejectedAt_ = std::chrono::steady_clock::now();
            hdrRejectedFormat_ = input.format;
            hdrRejectedWidth_ = input.width;
            hdrRejectedHeight_ = input.height;
            Logger::instance().warning(L"capture",
                L"D3D11 video processor rejected HDR input conversion (" +
                hresultToString(bltHr) + L"); falling back to SDR interpretation, tone-map retry in " +
                std::to_wstring(hdrRejectionCooldown(hdrRejectStrikes_).count()) + L"s");
            return VpBltOutcome::RetrySdr;
        }
        Logger::instance().warning(L"capture",
            L"D3D11 VideoProcessorBlt failed: " + hresultToString(bltHr));
        return outputFormat == DXGI_FORMAT_NV12 ? VpBltOutcome::DowngradeBgra : VpBltOutcome::Drop;
    }

    if (hdrRetryAfterCooldown && appliedHdrInputSpace_) {
        hdrRetryAfterCooldown = false;
        hdrRejectStrikes_ = 0;
        Logger::instance().info(L"capture",
            L"D3D11 video processor HDR tone-mapping recovered after transient rejection");
    }
    output = input;
    output.texture = slot->texture;
    output.lease = slot;
    output.width = outputWidth;
    output.height = outputHeight;
    output.format = outputFormat;
    return VpBltOutcome::Ok;
}

bool D3D11FrameScaler::VpState::equals(const VpState& other) const {
    return colorSpace1Path == other.colorSpace1Path &&
        inputWidth == other.inputWidth &&
        inputHeight == other.inputHeight &&
        outputWidth == other.outputWidth &&
        outputHeight == other.outputHeight &&
        inputFormat == other.inputFormat &&
        outputFormat == other.outputFormat &&
        hdr == other.hdr &&
        hdrRejected == other.hdrRejected &&
        fitWithBars == other.fitWithBars &&
        sourceRect.left == other.sourceRect.left &&
        sourceRect.top == other.sourceRect.top &&
        sourceRect.right == other.sourceRect.right &&
        sourceRect.bottom == other.sourceRect.bottom &&
        targetRect.left == other.targetRect.left &&
        targetRect.top == other.targetRect.top &&
        targetRect.right == other.targetRect.right &&
        targetRect.bottom == other.targetRect.bottom &&
        outputRect.left == other.outputRect.left &&
        outputRect.top == other.outputRect.top &&
        outputRect.right == other.outputRect.right &&
        outputRect.bottom == other.outputRect.bottom;
}

void D3D11FrameScaler::applyVideoProcessorState(const VpState& state) {
    D3D11_VIDEO_COLOR background{};
    background.RGBA.A = 1.0f;
    videoContext_->VideoProcessorSetOutputBackgroundColor(
        processor_.Get(), FALSE, &background);
    videoContext_->VideoProcessorSetStreamFrameFormat(
        processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

    // HDR sources carry PQ (10-bit) or scRGB (FP16) content.
    // Interpreting them as gamma-22 SDR washes out color and clips
    // highlights; declaring the real input color space makes the
    // video processor tone-map down to the SDR output. A rejected
    // conversion falls back to the SDR mapping with backoff so a
    // limited processor degrades to old behavior, never to frame
    // drops (finding 16).
    DXGI_COLOR_SPACE_TYPE inputColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    if (!state.hdrRejected) {
        if (state.hdr && state.inputFormat == DXGI_FORMAT_R10G10B10A2_UNORM) {
            inputColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        } else if (state.hdr && state.inputFormat == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            inputColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        }
    }
    // Legacy path programs no input color space, so nothing tone-maps there:
    // report no HDR mapping so hdrMappingActive() stays truthful.
    appliedHdrInputSpace_ =
        state.colorSpace1Path &&
        inputColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    if (!state.colorSpace1Path &&
        inputColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) {
        Logger::instance().warning(L"capture",
            L"D3D11 video processor lacks VideoContext1; HDR input will record without tone-mapping");
    }

    if (state.colorSpace1Path) {
        videoContext1_->VideoProcessorSetStreamColorSpace1(
            processor_.Get(),
            0,
            inputColorSpace);
        videoContext1_->VideoProcessorSetOutputColorSpace1(
            processor_.Get(),
            state.outputFormat == DXGI_FORMAT_NV12
                ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709
                : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE legacyInputColorSpace{};
        legacyInputColorSpace.RGB_Range = 0;
        legacyInputColorSpace.YCbCr_Matrix = 1;
        videoContext_->VideoProcessorSetStreamColorSpace(
            processor_.Get(), 0, &legacyInputColorSpace);

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE legacyOutputColorSpace{};
        legacyOutputColorSpace.YCbCr_Matrix = 1;
        legacyOutputColorSpace.Nominal_Range =
            state.outputFormat == DXGI_FORMAT_NV12 ? 1 : 0;
        videoContext_->VideoProcessorSetOutputColorSpace(
            processor_.Get(), &legacyOutputColorSpace);
    }

    videoContext_->VideoProcessorSetStreamSourceRect(
        processor_.Get(), 0, TRUE, &state.sourceRect);
    videoContext_->VideoProcessorSetStreamDestRect(
        processor_.Get(), 0, TRUE, &state.targetRect);
    videoContext_->VideoProcessorSetOutputTargetRect(
        processor_.Get(), TRUE, &state.outputRect);

    vpState_ = state;
    vpState_.valid = true;
}

void D3D11FrameScaler::releaseResources() {
    // Every path into here destroys the output pool, including the failure
    // paths, so the bump is unconditional: consumers watch poolGeneration()
    // and drop the registrations they made against these textures.
    ++poolGeneration_;
    inputViews_.clear();
    copiedInputView_.Reset();
    copiedInputTexture_.Reset();
    pool_.clear();
    processor_.Reset();
    enumerator_.Reset();
    videoContext1_.Reset();
    videoContext_.Reset();
    videoDevice_.Reset();
    // The sticky state died with the processor; the next blit reprograms it.
    // useCopiedInput_ is per-generation, not per-session: every rebuild
    // retries the direct path first (one extra failed VP call is cheap
    // against enumerator recreation), so DD<->WGC switches don't pin the copy.
    useCopiedInput_ = false;
    vpState_ = {};
    appliedHdrInputSpace_ = false;
    inputWidth_ = 0;
    inputHeight_ = 0;
    inputFormat_ = DXGI_FORMAT_UNKNOWN;
    outputWidth_ = 0;
    outputHeight_ = 0;
    outputFormat_ = DXGI_FORMAT_UNKNOWN;
}

void D3D11FrameScaler::reset() {
    releaseResources();
    useCopiedInput_ = false;
    nv12Unavailable_ = false;
    hdrInputRejected_ = false;
    hdrRejectStrikes_ = 0;
    hdrRejectedAt_ = {};
    hdrRejectedFormat_ = DXGI_FORMAT_UNKNOWN;
    hdrRejectedWidth_ = 0;
    hdrRejectedHeight_ = 0;
}

bool D3D11FrameScaler::hdrInputRejected() const {
    return hdrInputRejected_;
}

bool D3D11FrameScaler::hdrMappingActive() const {
    return appliedHdrInputSpace_;
}

uint64_t D3D11FrameScaler::poolGeneration() const {
    return poolGeneration_;
}

void D3D11FrameScaler::setPoolSize(uint32_t poolSize) {
    // Min 5: captureLoop keeps one slot pinned via lastFrame (effective free = size−1).
    const uint32_t normalized = GpuPoolBuilder::normalizePoolSize(poolSize);
    if (poolSize_ == normalized) {
        return;
    }
    poolSize_ = normalized;
    reset();
}

bool D3D11FrameScaler::ensureInitialized(
    D3DDevice& device,
    uint32_t inputWidth,
    uint32_t inputHeight,
    DXGI_FORMAT inputFormat,
    uint32_t outputWidth,
    uint32_t outputHeight,
    DXGI_FORMAT outputFormat) {
    if (processor_ &&
        inputWidth_ == inputWidth &&
        inputHeight_ == inputHeight &&
        inputFormat_ == inputFormat &&
        outputWidth_ == outputWidth &&
        outputHeight_ == outputHeight &&
        outputFormat_ == outputFormat) {
        return true;
    }

    releaseResources();

    HRESULT hr = device.device()->QueryInterface(IID_PPV_ARGS(&videoDevice_));
    if (FAILED(hr)) {
        Logger::instance().warning(L"capture", L"D3D11 video device is unavailable for scaling: " + hresultToString(hr));
        return false;
    }
    hr = device.context()->QueryInterface(IID_PPV_ARGS(&videoContext_));
    if (FAILED(hr)) {
        Logger::instance().warning(L"capture", L"D3D11 video context is unavailable for scaling: " + hresultToString(hr));
        return false;
    }
    device.context()->QueryInterface(IID_PPV_ARGS(&videoContext1_));

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = inputWidth;
    content.InputHeight = inputHeight;
    content.OutputWidth = outputWidth;
    content.OutputHeight = outputHeight;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    hr = videoDevice_->CreateVideoProcessorEnumerator(&content, &enumerator_);
    if (FAILED(hr)) {
        Logger::instance().warning(L"capture", L"CreateVideoProcessorEnumerator failed: " + hresultToString(hr));
        releaseResources();
        return false;
    }

    UINT formatSupport = 0;
    hr = enumerator_->CheckVideoProcessorFormat(outputFormat, &formatSupport);
    if (FAILED(hr) || (formatSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
        Logger::instance().warning(L"capture",
            L"D3D11 video processor does not support the requested output format: " +
            hresultToString(hr));
        releaseResources();
        return false;
    }

    hr = videoDevice_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_);
    if (FAILED(hr)) {
        Logger::instance().warning(L"capture", L"CreateVideoProcessor failed: " + hresultToString(hr));
        releaseResources();
        return false;
    }

    GpuPoolBuildOptions poolOptions;
    poolOptions.wantSrv = (outputFormat == DXGI_FORMAT_B8G8R8A8_UNORM);
    poolOptions.wantOutputView = true;
    std::vector<ComPtr<ID3D11Texture2D>> textures;
    if (!GpuPoolBuilder::build(
            device.device(), outputWidth, outputHeight, outputFormat,
            poolOptions, poolSize_, textures, L"D3D11 scaler")) {
        releaseResources();
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc{};
    outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputViewDesc.Texture2D.MipSlice = 0;

    for (auto& texture : textures) {
        auto slot = std::make_shared<TextureSlot>();
        slot->texture = std::move(texture);
        if (outputFormat == DXGI_FORMAT_B8G8R8A8_UNORM) {
            hr = device.device()->CreateRenderTargetView(slot->texture.Get(), nullptr, &slot->renderTargetView);
            if (FAILED(hr)) {
                Logger::instance().warning(L"capture", L"CreateRenderTargetView for D3D11 scaler failed: " + hresultToString(hr));
                releaseResources();
                return false;
            }
        }
        hr = videoDevice_->CreateVideoProcessorOutputView(slot->texture.Get(), enumerator_.Get(), &outputViewDesc, &slot->outputView);
        if (FAILED(hr)) {
            Logger::instance().warning(L"capture", L"CreateVideoProcessorOutputView failed: " + hresultToString(hr));
            releaseResources();
            return false;
        }
        pool_.push_back(std::move(slot));
    }

    inputWidth_ = inputWidth;
    inputHeight_ = inputHeight;
    inputFormat_ = inputFormat;
    outputWidth_ = outputWidth;
    outputHeight_ = outputHeight;
    outputFormat_ = outputFormat;
    pool_.setFootprintBytes(dxgiTextureBytes(outputWidth, outputHeight, outputFormat) * pool_.size());
    Logger::instance().debug(L"capture",
        L"D3D11 scaler initialized " +
        std::to_wstring(inputWidth) + L"x" + std::to_wstring(inputHeight) +
        L" -> " + std::to_wstring(outputWidth) + L"x" + std::to_wstring(outputHeight) +
        (outputFormat == DXGI_FORMAT_NV12 ? L" NV12" : L" BGRA") +
        L" pool=" + std::to_wstring(pool_.size()) +
        L" (" + std::to_wstring(pool_.footprintBytes() / 1024) + L" KiB)");
    return true;
}
bool D3D11FrameScaler::ensureCopyPool(
    D3DDevice& device,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format) {
    // Reuse the existing pool when it already matches the requested output.
    // Covers both a prior copy-pool build and a slow-path build at the same
    // dimensions/format.
    if (!pool_.empty() &&
        outputWidth_ == width &&
        outputHeight_ == height &&
        outputFormat_ == format) {
        return true;
    }

    releaseResources();

    // Mirror the slow-path bind flags so NVENC accepts these textures as input.
    GpuPoolBuildOptions copyOptions;
    copyOptions.wantSrv = (format == DXGI_FORMAT_B8G8R8A8_UNORM);
    copyOptions.wantOutputView = true;
    std::vector<ComPtr<ID3D11Texture2D>> textures;
    if (!GpuPoolBuilder::build(
            device.device(), width, height, format,
            copyOptions, poolSize_, textures, L"D3D11 scaler copy pool")) {
        releaseResources();
        return false;
    }
    for (auto& texture : textures) {
        auto slot = std::make_shared<TextureSlot>();
        slot->texture = std::move(texture);
        pool_.push_back(std::move(slot));
    }

    inputWidth_ = width;
    inputHeight_ = height;
    inputFormat_ = format;
    outputWidth_ = width;
    outputHeight_ = height;
    outputFormat_ = format;
    pool_.setFootprintBytes(dxgiTextureBytes(width, height, format) * pool_.size());
    Logger::instance().debug(L"capture",
        L"D3D11 scaler copy pool initialized " +
        std::to_wstring(width) + L"x" + std::to_wstring(height) +
        (format == DXGI_FORMAT_NV12 ? L" NV12" : L" BGRA") +
        L" pool=" + std::to_wstring(pool_.size()) +
        L" (" + std::to_wstring(pool_.footprintBytes() / 1024) + L" KiB)");
    return true;
}

ID3D11VideoProcessorInputView* D3D11FrameScaler::inputViewFor(
    D3DDevice& device,
    ID3D11Texture2D* texture) {
    if (useCopiedInput_) {
        return copiedInputViewFor(device, texture);
    }

    const auto existing = inputViews_.find(texture);
    if (existing != inputViews_.end()) {
        return existing->second.Get();
    }
    if (inputViews_.size() >= kMaxInputViewCacheEntries) {
        pruneStaleInputViews();
        if (inputViews_.size() >= kMaxInputViewCacheEntries) {
            inputViews_.clear();
        }
    } else {
        // Miss path: surfaces abandoned by a source rebind or a WGC pool
        // recreate would otherwise stay pinned in VRAM for the session.
        pruneStaleInputViews();
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc{};
    inputViewDesc.FourCC = 0;
    inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputViewDesc.Texture2D.MipSlice = 0;
    inputViewDesc.Texture2D.ArraySlice = 0;

    ComPtr<ID3D11VideoProcessorInputView> inputView;
    const HRESULT hr = videoDevice_->CreateVideoProcessorInputView(
        texture, enumerator_.Get(), &inputViewDesc, &inputView);
    if (SUCCEEDED(hr)) {
        auto [it, inserted] = inputViews_.emplace(texture, std::move(inputView));
        (void)inserted;
        return it->second.Get();
    }

    useCopiedInput_ = true;
    inputViews_.clear();
    Logger::instance().warning(L"capture",
        L"Capture texture is not directly compatible with the D3D11 video processor (" +
        hresultToString(hr) + L"); using a pooled BGRA input copy");
    return copiedInputViewFor(device, texture);
}

void D3D11FrameScaler::pruneStaleInputViews() {
    for (auto it = inputViews_.begin(); it != inputViews_.end();) {
        ID3D11Texture2D* texture = it->first;
        // Read the live COM refcount with an AddRef/Release probe. The probe's
        // own reference is released before the count is read, so `refs` equals
        // the pre-probe count. Exactly one of those references belongs to the
        // stored view; a total below two means no capture frame, pool slot, or
        // shared texture still owns this surface and its VRAM can be
        // reclaimed. Entries with any external owner stay cached.
        texture->AddRef();
        const ULONG refs = texture->Release();
        if (refs < 2) {
            it = inputViews_.erase(it);
        } else {
            ++it;
        }
    }
}

ID3D11VideoProcessorInputView* D3D11FrameScaler::copiedInputViewFor(
    D3DDevice& device,
    ID3D11Texture2D* texture) {
    if (copiedInputTexture_) {
        // PQ<->scRGB switches reuse this path at the same resolution, and
        // CopyResource needs identical formats: rebuild the copy texture
        // instead of copying across formats.
        D3D11_TEXTURE2D_DESC sourceDesc{};
        texture->GetDesc(&sourceDesc);
        D3D11_TEXTURE2D_DESC copyDesc{};
        copiedInputTexture_->GetDesc(&copyDesc);
        if (copyDesc.Format != sourceDesc.Format) {
            copiedInputView_.Reset();
            copiedInputTexture_.Reset();
        }
    }
    if (!copiedInputTexture_) {
        D3D11_TEXTURE2D_DESC sourceDesc{};
        texture->GetDesc(&sourceDesc);

        D3D11_TEXTURE2D_DESC copyDesc{};
        copyDesc.Width = inputWidth_;
        copyDesc.Height = inputHeight_;
        copyDesc.MipLevels = 1;
        copyDesc.ArraySize = 1;
        copyDesc.Format = sourceDesc.Format;
        copyDesc.SampleDesc.Count = 1;
        copyDesc.Usage = D3D11_USAGE_DEFAULT;
        // Video-processor input textures must have no bind flags on strict
        // drivers (AMD). SHADER_RESOURCE here causes CreateVideoProcessorInputView
        // to fail with E_INVALIDARG. This texture is only used as VP blt input.
        copyDesc.BindFlags = GpuPoolBuilder::bindFlagsFor({});

        HRESULT hr = device.device()->CreateTexture2D(
            &copyDesc, nullptr, &copiedInputTexture_);
        if (FAILED(hr)) {
            Logger::instance().warning(L"capture",
                L"CreateTexture2D for video-processor input copy failed: " +
                hresultToString(hr));
            return nullptr;
        }

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc{};
        inputViewDesc.FourCC = 0;
        inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputViewDesc.Texture2D.MipSlice = 0;
        inputViewDesc.Texture2D.ArraySlice = 0;
        hr = videoDevice_->CreateVideoProcessorInputView(
            copiedInputTexture_.Get(),
            enumerator_.Get(),
            &inputViewDesc,
            &copiedInputView_);
        if (FAILED(hr)) {
            Logger::instance().warning(L"capture",
                L"CreateVideoProcessorInputView for copied texture failed: " +
                hresultToString(hr));
            copiedInputTexture_.Reset();
            return nullptr;
        }
    }

    device.context()->CopyResource(copiedInputTexture_.Get(), texture);
    return copiedInputView_.Get();
}

} // namespace backtrack
