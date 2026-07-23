#include "capture/D3D11FrameScaler.h"

#include "core/Logger.h"
#include "platform/Win32Util.h"

#include <algorithm>

namespace backtrack {

namespace {

constexpr size_t kMaxInputViewCacheEntries = 64;

bool shouldLogRepeatedDrop(uint64_t& count) {
    ++count;
    return count == 1 || (count % 300) == 0;
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

    const bool sizeMatches = input.width == outputWidth && input.height == outputHeight;
    DXGI_FORMAT outputFormat =
        preferredOutputFormat == DXGI_FORMAT_NV12 && nv12Unavailable_
            ? DXGI_FORMAT_B8G8R8A8_UNORM
            : preferredOutputFormat;

    for (uint32_t attempt = 0; attempt < 2; ++attempt) {
        if (sizeMatches && input.format == outputFormat && !forceOutputTexture) {
            output = input;
            return true;
        }

        bool conversionFailed = false;
        if (!ensureInitialized(
                device,
                input.width,
                input.height,
                outputWidth,
                outputHeight,
                outputFormat)) {
            conversionFailed = true;
        } else {
            auto slot = acquireSlot();
            if (!slot) {
                if (shouldLogRepeatedDrop(poolExhaustionDrops_)) {
                    Logger::instance().warning(
                        L"D3D11 scaler texture pool exhausted; dropping frame count=" +
                        std::to_wstring(poolExhaustionDrops_));
                }
                return false;
            }
            poolExhaustionDrops_ = 0;

            std::scoped_lock lock(device.immediateContextMutex());
            ID3D11VideoProcessorInputView* inputView =
                inputViewFor(device, input.texture.Get());
            if (!inputView) {
                conversionFailed = true;
            } else {
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
                    if (slot->renderTargetView) {
                        constexpr FLOAT black[] = {0.0f, 0.0f, 0.0f, 1.0f};
                        device.context()->ClearRenderTargetView(
                            slot->renderTargetView.Get(), black);
                    }
                }

                D3D11_VIDEO_COLOR background{};
                background.RGBA.A = 1.0f;
                videoContext_->VideoProcessorSetOutputBackgroundColor(
                    processor_.Get(), FALSE, &background);
                videoContext_->VideoProcessorSetStreamFrameFormat(
                    processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
                if (videoContext1_) {
                    videoContext1_->VideoProcessorSetStreamColorSpace1(
                        processor_.Get(),
                        0,
                        DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
                    videoContext1_->VideoProcessorSetOutputColorSpace1(
                        processor_.Get(),
                        outputFormat == DXGI_FORMAT_NV12
                            ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709
                            : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
                } else {
                    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColorSpace{};
                    inputColorSpace.RGB_Range = 0;
                    inputColorSpace.YCbCr_Matrix = 1;
                    videoContext_->VideoProcessorSetStreamColorSpace(
                        processor_.Get(), 0, &inputColorSpace);

                    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColorSpace{};
                    outputColorSpace.YCbCr_Matrix = 1;
                    outputColorSpace.Nominal_Range =
                        outputFormat == DXGI_FORMAT_NV12 ? 1 : 0;
                    videoContext_->VideoProcessorSetOutputColorSpace(
                        processor_.Get(), &outputColorSpace);
                }
                videoContext_->VideoProcessorSetStreamSourceRect(
                    processor_.Get(), 0, TRUE, &sourceRect);
                videoContext_->VideoProcessorSetStreamDestRect(
                    processor_.Get(), 0, TRUE, &targetRect);
                RECT outputRect{
                    0,
                    0,
                    static_cast<LONG>(outputWidth),
                    static_cast<LONG>(outputHeight),
                };
                videoContext_->VideoProcessorSetOutputTargetRect(
                    processor_.Get(), TRUE, &outputRect);

                D3D11_VIDEO_PROCESSOR_STREAM stream{};
                stream.Enable = TRUE;
                stream.pInputSurface = inputView;

                const HRESULT hr = videoContext_->VideoProcessorBlt(
                    processor_.Get(), slot->outputView.Get(), 0, 1, &stream);
                if (FAILED(hr)) {
                    Logger::instance().warning(
                        L"D3D11 VideoProcessorBlt failed: " + hresultToString(hr));
                    conversionFailed = true;
                } else {
                    output = input;
                    output.texture = slot->texture;
                    output.lease = slot;
                    output.width = outputWidth;
                    output.height = outputHeight;
                    output.format = outputFormat;
                    return true;
                }
            }
        }

        if (!conversionFailed || outputFormat != DXGI_FORMAT_NV12) {
            return false;
        }

        nv12Unavailable_ = true;
        Logger::instance().warning(
            L"Preferred NV12 encoder input failed at runtime; falling back to BGRA for this capture session");
        releaseResources();
        outputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    return false;
}

void D3D11FrameScaler::releaseResources() {
    inputViews_.clear();
    copiedInputView_.Reset();
    copiedInputTexture_.Reset();
    poolExhaustionDrops_ = 0;
    pool_.clear();
    processor_.Reset();
    enumerator_.Reset();
    videoContext1_.Reset();
    videoContext_.Reset();
    videoDevice_.Reset();
    inputWidth_ = 0;
    inputHeight_ = 0;
    outputWidth_ = 0;
    outputHeight_ = 0;
    outputFormat_ = DXGI_FORMAT_UNKNOWN;
}

void D3D11FrameScaler::reset() {
    releaseResources();
    useCopiedInput_ = false;
    nv12Unavailable_ = false;
}

void D3D11FrameScaler::setPoolSize(uint32_t poolSize) {
    // Min 5: captureLoop keeps one slot pinned via lastFrame (effective free = size−1).
    const uint32_t normalized = std::clamp<uint32_t>(poolSize, 5, 16);
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
    uint32_t outputWidth,
    uint32_t outputHeight,
    DXGI_FORMAT outputFormat) {
    if (processor_ &&
        inputWidth_ == inputWidth &&
        inputHeight_ == inputHeight &&
        outputWidth_ == outputWidth &&
        outputHeight_ == outputHeight &&
        outputFormat_ == outputFormat) {
        return true;
    }

    releaseResources();

    HRESULT hr = device.device()->QueryInterface(IID_PPV_ARGS(&videoDevice_));
    if (FAILED(hr)) {
        Logger::instance().warning(L"D3D11 video device is unavailable for scaling: " + hresultToString(hr));
        return false;
    }
    hr = device.context()->QueryInterface(IID_PPV_ARGS(&videoContext_));
    if (FAILED(hr)) {
        Logger::instance().warning(L"D3D11 video context is unavailable for scaling: " + hresultToString(hr));
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
        Logger::instance().warning(L"CreateVideoProcessorEnumerator failed: " + hresultToString(hr));
        releaseResources();
        return false;
    }

    UINT formatSupport = 0;
    hr = enumerator_->CheckVideoProcessorFormat(outputFormat, &formatSupport);
    if (FAILED(hr) || (formatSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
        Logger::instance().warning(
            L"D3D11 video processor does not support the requested output format: " +
            hresultToString(hr));
        releaseResources();
        return false;
    }

    hr = videoDevice_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_);
    if (FAILED(hr)) {
        Logger::instance().warning(L"CreateVideoProcessor failed: " + hresultToString(hr));
        releaseResources();
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = outputWidth;
    desc.Height = outputHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = outputFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (outputFormat == DXGI_FORMAT_B8G8R8A8_UNORM) {
        desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc{};
    outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputViewDesc.Texture2D.MipSlice = 0;

    for (uint32_t index = 0; index < poolSize_; ++index) {
        auto slot = std::make_shared<TextureSlot>();
        hr = device.device()->CreateTexture2D(&desc, nullptr, &slot->texture);
        if (FAILED(hr)) {
            Logger::instance().warning(L"CreateTexture2D for D3D11 scaler failed: " + hresultToString(hr));
            releaseResources();
            return false;
        }
        if (outputFormat == DXGI_FORMAT_B8G8R8A8_UNORM) {
            hr = device.device()->CreateRenderTargetView(slot->texture.Get(), nullptr, &slot->renderTargetView);
            if (FAILED(hr)) {
                Logger::instance().warning(L"CreateRenderTargetView for D3D11 scaler failed: " + hresultToString(hr));
                releaseResources();
                return false;
            }
        }
        hr = videoDevice_->CreateVideoProcessorOutputView(slot->texture.Get(), enumerator_.Get(), &outputViewDesc, &slot->outputView);
        if (FAILED(hr)) {
            Logger::instance().warning(L"CreateVideoProcessorOutputView failed: " + hresultToString(hr));
            releaseResources();
            return false;
        }
        pool_.push_back(std::move(slot));
    }

    inputWidth_ = inputWidth;
    inputHeight_ = inputHeight;
    outputWidth_ = outputWidth;
    outputHeight_ = outputHeight;
    outputFormat_ = outputFormat;
    Logger::instance().debug(
        L"D3D11 scaler initialized " +
        std::to_wstring(inputWidth) + L"x" + std::to_wstring(inputHeight) +
        L" -> " + std::to_wstring(outputWidth) + L"x" + std::to_wstring(outputHeight) +
        (outputFormat == DXGI_FORMAT_NV12 ? L" NV12" : L" BGRA"));
    return true;
}

std::shared_ptr<D3D11FrameScaler::TextureSlot> D3D11FrameScaler::acquireSlot() {
    for (auto& slot : pool_) {
        if (slot.use_count() == 1) {
            return slot;
        }
    }
    return {};
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
        inputViews_.clear();
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
    Logger::instance().warning(
        L"Capture texture is not directly compatible with the D3D11 video processor (" +
        hresultToString(hr) + L"); using a pooled BGRA input copy");
    return copiedInputViewFor(device, texture);
}

ID3D11VideoProcessorInputView* D3D11FrameScaler::copiedInputViewFor(
    D3DDevice& device,
    ID3D11Texture2D* texture) {
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
        copyDesc.BindFlags = 0;

        HRESULT hr = device.device()->CreateTexture2D(
            &copyDesc, nullptr, &copiedInputTexture_);
        if (FAILED(hr)) {
            Logger::instance().warning(
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
            Logger::instance().warning(
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
