#include "capture/DesktopDuplicationCapture.h"

#include "core/Logger.h"
#include "platform/Win32Util.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

namespace backtrack {

namespace {

constexpr uint32_t kMinPoolSize = 5;
constexpr uint32_t kMaxPoolSize = 16;

uint32_t captureTexturePoolSize(const AppSettings& settings) {
    // +1 for lastFrame pin in captureLoop (keeps one pool slot in use).
    return std::clamp<uint32_t>(settings.gpu.frameQueueLimit + 4, kMinPoolSize, kMaxPoolSize);
}

// Minimal quad pipeline. The vertex shader synthesises a triangle strip from
// SV_VertexID (no vertex/index buffers) and positions it using a clip-space
// rect supplied through the constant buffer. The pixel shader samples cursor
// and persistent desktop, handling normal alpha and AND/XOR cursor semantics.
constexpr char kCursorShaderHlsl[] =
    "cbuffer Params : register(b0) {\n"
    "    float4 rect;\n" // x = leftNdc, y = topNdc, z = widthNdc, w = heightNdc
    "    float2 desktopSize;\n"
    "    uint shapeType;\n"
    "    float padding;\n"
    "};\n"
    "struct VOut {\n"
    "    float4 pos : SV_Position;\n"
    "    float2 uv  : TEXCOORD0;\n"
    "};\n"
    "VOut VSMain(uint id : SV_VertexID) {\n"
    "    float2 uv = float2((float)(id & 1), (float)((id >> 1) & 1));\n"
    "    VOut o;\n"
    "    o.pos = float4(rect.x + uv.x * rect.z, rect.y - uv.y * rect.w, 0.0f, 1.0f);\n"
    "    o.uv = uv;\n"
    "    return o;\n"
    "}\n"
    "Texture2D gCursor : register(t0);\n"
    "Texture2D gDesktop : register(t1);\n"
    "SamplerState gSmp : register(s0);\n"
    "float4 PSMain(VOut input) : SV_Target {\n"
    "    float4 cursor = gCursor.Sample(gSmp, input.uv);\n"
    "    float4 desktop = gDesktop.Load(int3(int2(input.pos.xy), 0));\n"
    "    if (shapeType == 1) {\n"
    "        uint3 screen = (uint3)round(saturate(desktop.rgb) * 255.0f);\n"
    "        uint andMask = cursor.r >= 0.5f ? 255u : 0u;\n"
    "        uint xorMask = cursor.g >= 0.5f ? 255u : 0u;\n"
    "        return float4((screen & andMask) ^ xorMask, 255u) / 255.0f;\n"
    "    }\n"
    "    if (shapeType == 4) {\n"
    "        uint3 screen = (uint3)round(saturate(desktop.rgb) * 255.0f);\n"
    "        uint3 color = (uint3)round(saturate(cursor.rgb) * 255.0f);\n"
    "        uint3 result = cursor.a >= 0.5f ? (screen ^ color) : color;\n"
    "        return float4(result, 255u) / 255.0f;\n"
    "    }\n"
    "    return float4(lerp(desktop.rgb, cursor.rgb, cursor.a), 1.0f);\n"
    "}\n";

} // namespace

bool DesktopDuplicationCapture::initialize(D3DDevice& device, const AppSettings& settings, const CaptureTarget& target) {
    device_ = &device;
    deviceLost_ = false;
    frameIndex_ = 0;
    poolExhaustionDrops_ = 0;
    haveContent_ = false;
    captureCursor_ = settings.captureCursor;
    cursorVisible_ = false;
    cursorShapeValid_ = false;
    cursorShapeDirty_ = false;

    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = device.device()->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr)) {
        Logger::instance().error(L"capture", L"ID3D11Device does not expose IDXGIDevice: " + hresultToString(hr));
        return false;
    }

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) {
        Logger::instance().error(L"capture", L"IDXGIDevice::GetAdapter failed: " + hresultToString(hr));
        return false;
    }

    // Desktop Duplication is per-output and the output must belong to this
    // device's adapter. Never guess output 0: that silently records wrong monitor.
    const HMONITOR monitor = target.monitor ? target.monitor : monitorFromIndex(target.monitorIndex);
    const DxgiOutputLocation location = dxgiOutputForMonitor(monitor);
    if (!location.valid()) {
        Logger::instance().error(L"capture", L"Desktop Duplication could not find requested monitor on any DXGI adapter");
        return false;
    }

    // Verify full monitor ownership against active adapter. This is stronger
    // than adapter-index comparison because DXGI adapter ordering can change.
    const uint32_t outputIndex = dxgiOutputIndexForMonitor(adapter.Get(), monitor);
    if (outputIndex == UINT32_MAX) {
        Logger::instance().error(L"capture", L"Desktop Duplication target does not belong to active D3D adapter");
        return false;
    }

    ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(outputIndex, &output);
    if (FAILED(hr)) {
        Logger::instance().error(L"capture", L"No DXGI output available for desktop duplication: " + hresultToString(hr));
        return false;
    }

    DXGI_OUTPUT_DESC outputDesc{};
    hr = output->GetDesc(&outputDesc);
    if (FAILED(hr)) {
        Logger::instance().error(L"capture", L"IDXGIOutput::GetDesc failed: " + hresultToString(hr));
        return false;
    }
    width_ = static_cast<uint32_t>(outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left);
    height_ = static_cast<uint32_t>(outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top);

    ComPtr<IDXGIOutput1> output1;
    hr = output.As(&output1);
    if (FAILED(hr)) {
        Logger::instance().error(L"capture", L"IDXGIOutput1 unavailable: " + hresultToString(hr));
        return false;
    }

    // DuplicateOutput can transiently fail with DXGI_ERROR_NOT_CURRENTLY_AVAILABLE
    // during display mode changes, fullscreen transitions, or when another process
    // is briefly holding the duplication. Retry with backoff before giving up.
    constexpr int kMaxDuplicateAttempts = 5;
    constexpr DWORD kDuplicateRetryDelayMs = 100;
    for (int attempt = 0; attempt < kMaxDuplicateAttempts; ++attempt) {
        hr = output1->DuplicateOutput(device.device(), &duplication_);
        if (SUCCEEDED(hr)) {
            break;
        }
        if (hr != DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
            break;
        }
        Logger::instance().warning(L"capture",
            L"DuplicateOutput not currently available; retrying attempt " +
            std::to_wstring(attempt + 1) + L"/" + std::to_wstring(kMaxDuplicateAttempts));
        Sleep(kDuplicateRetryDelayMs);
    }
    if (FAILED(hr)) {
        Logger::instance().error(L"capture", L"DuplicateOutput failed: " + hresultToString(hr));
        return false;
    }

    poolSize_ = captureTexturePoolSize(settings);
    if (!createTexturePool(width_, height_, format_, poolSize_)) {
        return false;
    }

    Logger::instance().info(L"capture",
        L"Desktop Duplication initialized at " + std::to_wstring(width_) + L"x" + std::to_wstring(height_));
    return true;
}

bool DesktopDuplicationCapture::acquireNextFrame(GpuFrame& frame, uint32_t timeoutMs) {
    if (!duplication_) {
        return false;
    }

    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = duplication_->AcquireNextFrame(timeoutMs, &frameInfo, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        Logger::instance().warning(L"capture", L"Desktop duplication access lost; capture must be recreated");
        deviceLost_ = true;
        return false;
    }
    if (FAILED(hr)) {
        Logger::instance().warning(L"capture", L"AcquireNextFrame failed: " + hresultToString(hr));
        return false;
    }

    ComPtr<ID3D11Texture2D> acquiredTexture;
    hr = resource.As(&acquiredTexture);
    if (FAILED(hr)) {
        duplication_->ReleaseFrame();
        Logger::instance().warning(L"capture", L"Captured resource is not ID3D11Texture2D: " + hresultToString(hr));
        return false;
    }

    // Desktop dimensions/format come from the acquired texture, not the output
    // desc: rotated displays swap width/height, and HDR outputs deliver 10-bit
    // or fp16 surfaces (e.g. R16G16B16A16_FLOAT) rather than the default BGRA8.
    // Recreate the pool whenever the source surface no longer matches.
    D3D11_TEXTURE2D_DESC acquiredDesc{};
    acquiredTexture->GetDesc(&acquiredDesc);
    if (acquiredDesc.Width != width_ || acquiredDesc.Height != height_ || acquiredDesc.Format != format_) {
        Logger::instance().info(L"capture",
            L"Capture surface changed to " + std::to_wstring(acquiredDesc.Width) + L"x" +
            std::to_wstring(acquiredDesc.Height) + L"; recreating texture pool");
        width_ = acquiredDesc.Width;
        height_ = acquiredDesc.Height;
        format_ = acquiredDesc.Format;
        if (!createTexturePool(width_, height_, format_, poolSize_)) {
            duplication_->ReleaseFrame();
            deviceLost_ = true;
            return false;
        }
    }

    // Track cursor position/visibility and pick up a new shape when DXGI signals
    // one. The pointer shape is only re-sent when it changes, so it is cached.
    updateCursorState(frameInfo);
    if (frameInfo.PointerShapeBufferSize > 0) {
        refreshCursorShape(frameInfo.PointerShapeBufferSize);
    }

    // Persist the desktop image. DXGI wakes us for desktop changes and for
    // pointer-only updates; only refresh the stored copy when the desktop itself
    // changed (or on the first frame, when we have no copy yet).
    const bool desktopChanged = frameInfo.LastPresentTime.QuadPart != 0 || !haveContent_;
    if (desktopChanged) {
        std::scoped_lock lock(device_->immediateContextMutex());
        device_->context()->CopyResource(desktopCopy_.Get(), acquiredTexture.Get());
        haveContent_ = true;
    }

    duplication_->ReleaseFrame();

    if (!haveContent_) {
        return false;
    }

    auto slot = acquireSlot();
    if (!slot) {
        ++poolExhaustionDrops_;
        if (poolExhaustionDrops_ == 1 || (poolExhaustionDrops_ % 300) == 0) {
            Logger::instance().warning(L"capture",
                L"Capture texture pool exhausted; dropping frame count=" +
                std::to_wstring(poolExhaustionDrops_));
        }
        return false;
    }
    poolExhaustionDrops_ = 0;

    // Copy the persistent desktop into the slot, then blend the cursor on top so
    // a moving pointer over a static desktop still yields fresh frames.
    const bool drawCursor = captureCursor_ && cursorVisible_ && cursorShapeValid_ && ensureCompositor();
    {
        std::scoped_lock lock(device_->immediateContextMutex());
        device_->context()->CopyResource(slot->texture.Get(), desktopCopy_.Get());
        if (drawCursor) {
            compositeCursor(slot);
        }
    }

    frame.texture = slot->texture;
    frame.lease = slot;
    frame.frameIndex = frameIndex_++;
    static LARGE_INTEGER qpcFrequency{};
    if (qpcFrequency.QuadPart == 0) {
        QueryPerformanceFrequency(&qpcFrequency);
    }
    const LONGLONG timestamp = frameInfo.LastPresentTime.QuadPart != 0
        ? frameInfo.LastPresentTime.QuadPart
        : frameInfo.LastMouseUpdateTime.QuadPart;
    frame.pts100ns = qpcFrequency.QuadPart > 0 && timestamp != 0
        ? static_cast<int64_t>((static_cast<long double>(timestamp) * kHundredNanosecondsPerSecond)
            / static_cast<long double>(qpcFrequency.QuadPart))
        : static_cast<int64_t>(frame.frameIndex * (kHundredNanosecondsPerSecond / 60));
    frame.width = width_;
    frame.height = height_;
    frame.format = format_;
    return true;
}

void DesktopDuplicationCapture::shutdown() {
    duplication_.Reset();
    haveContent_ = false;
    pool_.clear();
    desktopCopy_.Reset();
    desktopCopySrv_.Reset();
    cursorTexture_.Reset();
    cursorSrv_.Reset();
    cursorVs_.Reset();
    cursorPs_.Reset();
    cursorConstantBuffer_.Reset();
    cursorBlendState_.Reset();
    cursorRasterizerState_.Reset();
    cursorSampler_.Reset();
    compositorReady_ = false;
    cursorShapeValid_ = false;
    cursorVisible_ = false;
    device_ = nullptr;
}

std::shared_ptr<DesktopDuplicationCapture::TextureSlot> DesktopDuplicationCapture::acquireSlot() {
    for (auto& slot : pool_) {
        if (slot.use_count() == 1) {
            return slot;
        }
    }
    return {};
}

bool DesktopDuplicationCapture::createTexturePool(uint32_t width, uint32_t height, DXGI_FORMAT format, uint32_t poolSize) {
    pool_.clear();
    desktopCopy_.Reset();
    desktopCopySrv_.Reset();
    haveContent_ = false;
    poolExhaustionDrops_ = 0;
    poolSize = std::clamp<uint32_t>(poolSize, kMinPoolSize, kMaxPoolSize);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    // RENDER_TARGET lets us blend the cursor into the slot; the downstream scaler
    // consumes slots as a video-processor input surface, so the extra bind flag is
    // free. SHADER_RESOURCE is retained for parity with the previous pool.
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    for (uint32_t index = 0; index < poolSize; ++index) {
        auto slot = std::make_shared<TextureSlot>();
        HRESULT hr = device_->device()->CreateTexture2D(&desc, nullptr, &slot->texture);
        if (FAILED(hr)) {
            Logger::instance().error(L"capture", L"CreateTexture2D for capture pool failed: " + hresultToString(hr));
            pool_.clear();
            return false;
        }
        hr = device_->device()->CreateRenderTargetView(slot->texture.Get(), nullptr, &slot->renderTargetView);
        if (FAILED(hr)) {
            Logger::instance().error(L"capture", L"CreateRenderTargetView for capture pool failed: " + hresultToString(hr));
            pool_.clear();
            return false;
        }
        pool_.push_back(std::move(slot));
    }

    // Persistent desktop image feeds both slot copies and cursor's destination
    // pixel logic, so it requires a shader-resource view.
    D3D11_TEXTURE2D_DESC copyDesc = desc;
    copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device_->device()->CreateTexture2D(&copyDesc, nullptr, &desktopCopy_);
    if (FAILED(hr)) {
        Logger::instance().error(L"capture", L"CreateTexture2D for desktop copy failed: " + hresultToString(hr));
        pool_.clear();
        desktopCopy_.Reset();
        return false;
    }
    hr = device_->device()->CreateShaderResourceView(desktopCopy_.Get(), nullptr, &desktopCopySrv_);
    if (FAILED(hr)) {
        Logger::instance().error(L"capture", L"CreateShaderResourceView for desktop copy failed: " + hresultToString(hr));
        pool_.clear();
        desktopCopy_.Reset();
        desktopCopySrv_.Reset();
        return false;
    }

    return true;
}

bool DesktopDuplicationCapture::ensureCompositor() {
    if (compositorReady_) {
        return true;
    }
    if (!device_) {
        return false;
    }

    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;
    ComPtr<ID3DBlob> errorBlob;
    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    HRESULT hr = D3DCompile(kCursorShaderHlsl, sizeof(kCursorShaderHlsl) - 1, "cursor.hlsl", nullptr,
        nullptr, "VSMain", "vs_4_0", compileFlags, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        Logger::instance().error(L"capture", L"Cursor vertex shader compile failed: " + hresultToString(hr));
        return false;
    }
    hr = D3DCompile(kCursorShaderHlsl, sizeof(kCursorShaderHlsl) - 1, "cursor.hlsl", nullptr,
        nullptr, "PSMain", "ps_4_0", compileFlags, 0, &psBlob, &errorBlob);
    if (FAILED(hr)) {
        Logger::instance().error(L"capture", L"Cursor pixel shader compile failed: " + hresultToString(hr));
        return false;
    }

    hr = device_->device()->CreateVertexShader(
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &cursorVs_);
    if (FAILED(hr)) {
        Logger::instance().error(L"capture", L"CreateVertexShader failed: " + hresultToString(hr));
        return false;
    }
    hr = device_->device()->CreatePixelShader(
        psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &cursorPs_);
    if (FAILED(hr)) {
        Logger::instance().error(L"capture", L"CreatePixelShader failed: " + hresultToString(hr));
        return false;
    }

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = 32; // rect, desktop size, pointer shape type
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = device_->device()->CreateBuffer(&cbDesc, nullptr, &cursorConstantBuffer_);
    if (FAILED(hr)) {
        Logger::instance().error(L"capture", L"Cursor constant buffer creation failed: " + hresultToString(hr));
        return false;
    }

    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device_->device()->CreateBlendState(&blendDesc, &cursorBlendState_);
    if (FAILED(hr)) {
        Logger::instance().error(L"capture", L"Cursor blend state creation failed: " + hresultToString(hr));
        return false;
    }

    D3D11_RASTERIZER_DESC rasterizerDesc{};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;
    hr = device_->device()->CreateRasterizerState(&rasterizerDesc, &cursorRasterizerState_);
    if (FAILED(hr)) {
        Logger::instance().error(L"capture", L"Cursor rasterizer state creation failed: " + hresultToString(hr));
        return false;
    }

    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device_->device()->CreateSamplerState(&sampDesc, &cursorSampler_);
    if (FAILED(hr)) {
        Logger::instance().error(L"capture", L"Cursor sampler creation failed: " + hresultToString(hr));
        return false;
    }

    compositorReady_ = true;
    return true;
}

void DesktopDuplicationCapture::updateCursorState(const DXGI_OUTDUPL_FRAME_INFO& frameInfo) {
    // PointerPosition is only meaningful when the mouse was updated this frame.
    if (frameInfo.LastMouseUpdateTime.QuadPart != 0) {
        cursorVisible_ = frameInfo.PointerPosition.Visible != FALSE;
        cursorPosition_ = frameInfo.PointerPosition.Position;
    }
}

bool DesktopDuplicationCapture::refreshCursorShape(uint32_t bufferSize) {
    cursorShapeBuffer_.resize(bufferSize);
    UINT requiredSize = 0;
    HRESULT hr = duplication_->GetFramePointerShape(
        bufferSize, cursorShapeBuffer_.data(), &requiredSize, &cursorShapeInfo_);
    if (FAILED(hr)) {
        Logger::instance().warning(L"capture", L"GetFramePointerShape failed: " + hresultToString(hr));
        return false;
    }
    cursorShapeDirty_ = true;
    return buildCursorTexture();
}

bool DesktopDuplicationCapture::buildCursorTexture() {
    if (!cursorShapeDirty_) {
        return cursorShapeValid_;
    }

    const uint32_t srcWidth = cursorShapeInfo_.Width;
    const uint32_t srcPitch = cursorShapeInfo_.Pitch;
    const uint8_t* src = cursorShapeBuffer_.data();

    uint32_t outWidth = srcWidth;
    uint32_t outHeight = cursorShapeInfo_.Height;
    std::vector<uint32_t> bgra; // 0xAARRGGBB packed little-endian => B,G,R,A bytes

    if (srcWidth == 0 || outHeight == 0 || srcPitch == 0) {
        cursorShapeDirty_ = false;
        cursorShapeValid_ = false;
        return false;
    }

    if (cursorShapeInfo_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
        // Buffer holds a 1bpp AND mask followed by a 1bpp XOR mask, so the actual
        // cursor height is half the reported height.
        outHeight = cursorShapeInfo_.Height / 2;
        bgra.resize(static_cast<size_t>(outWidth) * outHeight);
        for (uint32_t y = 0; y < outHeight; ++y) {
            for (uint32_t x = 0; x < outWidth; ++x) {
                const uint32_t byteIndex = y * srcPitch + (x / 8);
                const uint8_t mask = static_cast<uint8_t>(0x80 >> (x % 8));
                const bool andBit = (src[byteIndex] & mask) != 0;
                const bool xorBit = (src[(outHeight * srcPitch) + byteIndex] & mask) != 0;
                // Shader uses R/G as AND/XOR selectors against desktop pixels.
                const uint32_t pixel = (andBit ? 0x00FF0000u : 0u) | (xorBit ? 0x0000FF00u : 0u);
                bgra[static_cast<size_t>(y) * outWidth + x] = pixel;
            }
        }
    } else if (cursorShapeInfo_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
        bgra.resize(static_cast<size_t>(outWidth) * outHeight);
        for (uint32_t y = 0; y < outHeight; ++y) {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(src + static_cast<size_t>(y) * srcPitch);
            std::memcpy(&bgra[static_cast<size_t>(y) * outWidth], row, static_cast<size_t>(outWidth) * 4);
        }
    } else if (cursorShapeInfo_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
        // Alpha 0 replaces desktop RGB; alpha 0xFF XORs it. Both paths run in
        // the compositor because fixed-function blending cannot express XOR.
        bgra.resize(static_cast<size_t>(outWidth) * outHeight);
        for (uint32_t y = 0; y < outHeight; ++y) {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(src + static_cast<size_t>(y) * srcPitch);
            for (uint32_t x = 0; x < outWidth; ++x) {
                const uint32_t texel = row[x];
                bgra[static_cast<size_t>(y) * outWidth + x] = texel;
            }
        }
    } else {
        Logger::instance().warning(L"capture", L"Unknown cursor shape type; skipping composite");
        cursorShapeDirty_ = false;
        cursorShapeValid_ = false;
        return false;
    }

    // (Re)create the cursor texture when the dimensions change.
    if (!cursorTexture_ || cursorTexWidth_ != outWidth || cursorTexHeight_ != outHeight) {
        cursorTexture_.Reset();
        cursorSrv_.Reset();

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = outWidth;
        desc.Height = outHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initData{};
        initData.pSysMem = bgra.data();
        initData.SysMemPitch = outWidth * 4;

        HRESULT hr = device_->device()->CreateTexture2D(&desc, &initData, &cursorTexture_);
        if (FAILED(hr)) {
            Logger::instance().warning(L"capture", L"Cursor texture creation failed: " + hresultToString(hr));
            cursorShapeValid_ = false;
            return false;
        }
        hr = device_->device()->CreateShaderResourceView(cursorTexture_.Get(), nullptr, &cursorSrv_);
        if (FAILED(hr)) {
            Logger::instance().warning(L"capture", L"Cursor SRV creation failed: " + hresultToString(hr));
            cursorTexture_.Reset();
            cursorShapeValid_ = false;
            return false;
        }
        cursorTexWidth_ = outWidth;
        cursorTexHeight_ = outHeight;
    } else {
        std::scoped_lock lock(device_->immediateContextMutex());
        device_->context()->UpdateSubresource(
            cursorTexture_.Get(), 0, nullptr, bgra.data(), outWidth * 4, 0);
    }

    cursorShapeDirty_ = false;
    cursorShapeType_ = cursorShapeInfo_.Type;
    cursorShapeValid_ = true;
    return true;
}

void DesktopDuplicationCapture::compositeCursor(const std::shared_ptr<TextureSlot>& slot) {
    if (!slot->renderTargetView || !cursorSrv_) {
        return;
    }

    auto* ctx = device_->context();

    // DXGI reports output-local coordinates for the cursor image's top-left.
    // ShapeInfo::HotSpot is informational and must not affect placement.
    const float left = static_cast<float>(cursorPosition_.x);
    const float top = static_cast<float>(cursorPosition_.y);
    const float w = static_cast<float>(cursorTexWidth_);
    const float h = static_cast<float>(cursorTexHeight_);
    const float leftNdc = (left / static_cast<float>(width_)) * 2.0f - 1.0f;
    const float topNdc = 1.0f - (top / static_cast<float>(height_)) * 2.0f;
    const float widthNdc = (w / static_cast<float>(width_)) * 2.0f;
    const float heightNdc = (h / static_cast<float>(height_)) * 2.0f;
    struct CursorConstants {
        float rect[4];
        float desktopSize[2];
        uint32_t shapeType;
        float padding;
    } constants{{leftNdc, topNdc, widthNdc, heightNdc}, {static_cast<float>(width_), static_cast<float>(height_)},
        cursorShapeType_, 0.0f};
    ctx->UpdateSubresource(cursorConstantBuffer_.Get(), 0, nullptr, &constants, 0, 0);

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width_);
    viewport.Height = static_cast<float>(height_);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &viewport);

    ID3D11RenderTargetView* rtv = slot->renderTargetView.Get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);

    const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ctx->OMSetBlendState(cursorBlendState_.Get(), blendFactor, 0xFFFFFFFF);

    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->RSSetState(cursorRasterizerState_.Get());
    ctx->VSSetShader(cursorVs_.Get(), nullptr, 0);
    ID3D11Buffer* cb = cursorConstantBuffer_.Get();
    ctx->VSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetShader(cursorPs_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = {cursorSrv_.Get(), desktopCopySrv_.Get()};
    ctx->PSSetShaderResources(0, 2, srvs);
    ID3D11SamplerState* samp = cursorSampler_.Get();
    ctx->PSSetSamplers(0, 1, &samp);

    ctx->Draw(4, 0);

    // Unbind the render target so the slot can be read as an input surface later.
    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
    ID3D11ShaderResourceView* nullSrvs[] = {nullptr, nullptr};
    ctx->PSSetShaderResources(0, 2, nullSrvs);
}

} // namespace backtrack
