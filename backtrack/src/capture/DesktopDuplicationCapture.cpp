#include "capture/DesktopDuplicationCapture.h"

#include "capture/GpuPoolBuilder.h"
#include "core/Logger.h"
#include "platform/Win32Util.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace backtrack {

namespace {


// Short pool-format note for the init line so a log shows the session's
// interpretation at a glance (BGRA = SDR path, scRGB/R10 = pinned HDR pool).
const wchar_t* poolFormatName(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return L"R10G10B10A2";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return L"scRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return L"BGRA";
    default:
        return L"unknown";
    }
}

// Minimal quad pipeline. The vertex shader synthesises a triangle strip from
// SV_VertexID (no vertex/index buffers) and positions it using a clip-space
// rect supplied through the constant buffer. The pixel shader samples cursor
// and persistent desktop, handling normal alpha and AND/XOR cursor semantics.
// hdrBypass stays linear on HDR targets: 8-bit quantize plus the 0.5 selector
// thresholds clip and band PQ/scRGB highlights, so monochrome keeps AND/XOR in
// linear and everything else falls back to plain alpha blend.
constexpr char kCursorShaderHlsl[] =
    "cbuffer Params : register(b0) {\n"
    "    float4 rect;\n" // x = leftNdc, y = topNdc, z = widthNdc, w = heightNdc
    "    float2 desktopSize;\n"
    "    uint shapeType;\n"
    "    uint hdrBypass;\n"
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
    "    if (hdrBypass != 0) {\n"
    "        if (shapeType == 1) {\n"
    "            float3 kept = desktop.rgb * cursor.r;\n"
    "            return float4(lerp(kept, 1.0f - kept, cursor.g), 1.0f);\n"
    "        }\n"
    "        return float4(lerp(desktop.rgb, cursor.rgb, cursor.a), 1.0f);\n"
    "    }\n"
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

// SDR->scRGB bridge for a pinned HDR session. A fullscreen triangle loads the
// staged SDR surface and normalizes it to scRGB-linear with the exact transfer
// HdrStability.h mirrors on CPU (srgbChannelToLinear, unit-tested): black and
// paper white pass through bit-identical, so a bridged frame encodes
// identically to a native HDR frame of the same desktop and the surface
// oscillation cannot flicker. Alpha is opaque; only .rgb crosses the bridge.
constexpr char kBridgeShaderHlsl[] =
    "Texture2D gSrc : register(t0);\n"
    "float4 VSMain(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 p = float2(id == 1 ? 3.0f : -1.0f, id == 2 ? -3.0f : 1.0f);\n"
    "    return float4(p, 0.0f, 1.0f);\n"
    "}\n"
    "float srgbToLinear(float c) {\n"
    "    return c <= 0.04045f ? c / 12.92f : pow((c + 0.055f) / 1.055f, 2.4f);\n"
    "}\n"
    "float4 PSMain(float4 pos : SV_Position) : SV_Target {\n"
    "    float3 c = gSrc.Load(int3(int2(pos.xy), 0)).rgb;\n"
    "    return float4(srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b), 1.0f);\n"
    "}\n";

} // namespace

void DesktopDuplicationCapture::releaseHeldFrame() {
    if (frameHeld_ && duplication_) {
        duplication_->ReleaseFrame();
    }
    frameHeld_ = false;
}

void DesktopDuplicationCapture::logAcquireFailureOnce(const wchar_t* reason, HRESULT hr) {
    const auto now = std::chrono::steady_clock::now();
    constexpr auto kLogCooldown = std::chrono::seconds(2);
    if (hr == lastLoggedAcquireHr_ &&
        lastAcquireLogAt_.time_since_epoch().count() != 0 &&
        now - lastAcquireLogAt_ < kLogCooldown) {
        ++suppressedAcquireLogs_;
        return;
    }
    std::wstring message = reason ? reason : L"AcquireNextFrame failed";
    message += L": " + hresultToString(hr);
    if (suppressedAcquireLogs_ > 0) {
        message += L" (suppressed " + std::to_wstring(suppressedAcquireLogs_) + L" repeats)";
        suppressedAcquireLogs_ = 0;
    }
    Logger::instance().warning(L"capture", message);
    lastLoggedAcquireHr_ = hr;
    lastAcquireLogAt_ = now;
}

void DesktopDuplicationCapture::markLost(const wchar_t* reason, HRESULT hr) {
    if (!deviceLost_) {
        logAcquireFailureOnce(reason, hr);
    }
    releaseHeldFrame();
    deviceLost_ = true;
    duplication_.Reset();
    frameHeld_ = false;
}

bool DesktopDuplicationCapture::queryAdvancedColor() {
    if (colorProbe_) {
        DXGI_OUTPUT_DESC1 desc1{};
        if (SUCCEEDED(colorProbe_->GetDesc1(&desc1))) {
            // Same predicate as monitorHasAdvancedColor: PQ or scRGB.
            return desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        }
        return hdrPollState_; // transient failure: hold the last state, never flap.
    }
    return monitorHasAdvancedColor(monitor_);
}

void DesktopDuplicationCapture::applyHdrUpgrade() {
    // Ratcheted pin, called once when an HDR surface upgrades an SDR-pinned
    // session (HdrSessionPin already moved). An interpretation change reads
    // like a resize downstream, so it opens a new GOP through the same hook.
    ++hdrChangeCount_;
    ++resolutionChanges_;
    Logger::instance().info(L"capture",
        L"Desktop Duplication HDR pinned mid-session; single tone-map mapping from here (no flips)");
}

bool DesktopDuplicationCapture::initialize(D3DDevice& device, const AppSettings& settings, const CaptureTarget& target) {
    device_ = &device;
    deviceLost_ = false;
    frameIndex_ = 0;
    cursorOnlyFrames_ = 0;
    resolutionChanges_ = 0;
    hdrChangeCount_ = 0;
    hdrPin_.reset();
    hdrPollState_ = false;
    bridgedFrames_ = 0;
    colorProbe_.Reset();
    pool_.clear();
    desktopCopy_.Reset();
    desktopCopySrv_.Reset();
    desktopCopyRtv_.Reset();
    bridgeStaging_.Reset();
    bridgeStagingSrv_.Reset();
    bridgeStagingFormat_ = DXGI_FORMAT_UNKNOWN;
    bridgeVs_.Reset();
    bridgePs_.Reset();
    cursorOnlyFrameInterval_ = std::chrono::nanoseconds(
        1'000'000'000 / std::max<uint32_t>(1, settings.video.fps));
    nextCursorOnlyFrameAt_ = {};
    haveContent_ = false;
    frameHeld_ = false;
    lastLoggedAcquireHr_ = S_OK;
    suppressedAcquireLogs_ = 0;
    lastAcquireLogAt_ = {};
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
    monitor_ = monitor;
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
    // Cache the output for cheap per-frame HDR polling (GetDesc1 needs no
    // factory walk). Failure is non-fatal: queries fall back to
    // monitorHasAdvancedColor.
    output.As(&colorProbe_);
    hdrPollState_ = queryAdvancedColor();
    // The pool stays empty until the first surface pins the session
    // (HdrSessionPin::begin), so an HDR start never pays a BGRA recreate.

    ComPtr<IDXGIOutput1> output1;
    hr = output.As(&output1);
    if (FAILED(hr)) {
        Logger::instance().error(L"capture", L"IDXGIOutput1 unavailable: " + hresultToString(hr));
        return false;
    }

    // Non-blocking rebind: a few short attempts only. Outer capture loop owns
    // WaitStable cooldown so we do not sleep seconds on the capture thread.
    constexpr int kMaxDuplicateAttempts = 4;
    for (int attempt = 0; attempt < kMaxDuplicateAttempts; ++attempt) {
        duplication_.Reset();
        hr = output1->DuplicateOutput(device.device(), &duplication_);
        if (SUCCEEDED(hr)) {
            break;
        }
        if (hr != DXGI_ERROR_NOT_CURRENTLY_AVAILABLE &&
            hr != DXGI_ERROR_ACCESS_LOST &&
            hr != E_ACCESSDENIED) {
            break;
        }
        if (attempt + 1 < kMaxDuplicateAttempts) {
            Sleep(40);
        }
    }
    if (FAILED(hr)) {
        Logger::instance().warning(L"capture", L"DuplicateOutput failed: " + hresultToString(hr));
        duplication_.Reset();
        return false;
    }
    frameHeld_ = false;

    poolSize_ = texturePoolSizeForQueueLimit(settings.gpu.frameQueueLimit);
    // The pool stays empty until the first acquired surface reveals its real
    // format; eagerly creating BGRA here forces a recreate on every HDR start.
    lastHdrPollAt_ = std::chrono::steady_clock::now();

    Logger::instance().info(L"capture",
        L"Desktop Duplication initialized at " + std::to_wstring(width_) + L"x" + std::to_wstring(height_) +
        (hdrPollState_ ? L" (HDR reported; first surface decides the pool)" : L""));
    return true;
}

bool DesktopDuplicationCapture::acquireNextFrame(GpuFrame& frame, uint32_t timeoutMs) {
    if (deviceLost_ || !duplication_) {
        return false;
    }

    // HDR can toggle without a mode/format change, so re-poll cheaply (cached
    // output, at most 1 Hz) instead of trusting the init-time flag.
    const auto hdrPollNow = std::chrono::steady_clock::now();
    if (lastHdrPollAt_.time_since_epoch().count() == 0 ||
        hdrPollNow - lastHdrPollAt_ >= std::chrono::seconds(1)) {
        lastHdrPollAt_ = hdrPollNow;
        // Raw poll only: publishing waits for the surface format to agree over
        // consecutive frames below, so a poll/format race holds instead of
        // flipping mid-GOP.
        hdrPollState_ = queryAdvancedColor();
    }

    // DXGI requires ReleaseFrame before the next AcquireNextFrame.
    if (frameHeld_) {
        releaseHeldFrame();
    }

    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = duplication_->AcquireNextFrame(timeoutMs, &frameInfo, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false;
    }
    // ACCESS_LOST / INVALID_CALL / device loss: stop all further acquires.
    if (hr == DXGI_ERROR_ACCESS_LOST ||
        hr == DXGI_ERROR_INVALID_CALL ||
        hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_DEVICE_RESET ||
        hr == E_INVALIDARG) {
        markLost(L"Desktop duplication lost", hr);
        return false;
    }
    if (FAILED(hr)) {
        logAcquireFailureOnce(L"AcquireNextFrame failed", hr);
        // Unknown failures often mean the object is unusable under exclusive FS.
        markLost(L"Desktop duplication unusable", hr);
        return false;
    }

    frameHeld_ = true;

    ComPtr<ID3D11Texture2D> acquiredTexture;
    hr = resource.As(&acquiredTexture);
    if (FAILED(hr)) {
        releaseHeldFrame();
        logAcquireFailureOnce(L"Captured resource is not ID3D11Texture2D", hr);
        return false;
    }

    D3D11_TEXTURE2D_DESC acquiredDesc{};
    acquiredTexture->GetDesc(&acquiredDesc);
    const DXGI_FORMAT arrivedFormat = acquiredDesc.Format;
    const bool sizeChanged =
        acquiredDesc.Width != width_ || acquiredDesc.Height != height_;
    if (pool_.empty()) {
        // First surface decides pool and pin (HdrSessionPin::begin). The OS
        // report never pre-pins: an HDR poll with all-BGRA surfaces is a
        // whole SDR session, and an scRGB pool would force every one of its
        // frames through the HDR tone-map. A later HDR surface still
        // upgrades once through noteSurfaceFormat below.
        width_ = acquiredDesc.Width;
        height_ = acquiredDesc.Height;
        format_ = hdrPin_.begin(arrivedFormat);
        if (!createTexturePool(width_, height_, format_, poolSize_)) {
            markLost(L"Texture pool create failed", E_FAIL);
            return false;
        }
    } else if (sizeChanged || arrivedFormat != format_) {
        width_ = acquiredDesc.Width;
        height_ = acquiredDesc.Height;
        hdrPollState_ = queryAdvancedColor();
        if (!hdrPin_.pinnedHdr() && isHdrCaptureFormat(arrivedFormat)) {
            // SDR->HDR upgrade: the one direction the ratchet allows. Adopt
            // the HDR pool once; every later SDR surface bridges instead.
            hdrPin_.noteSurfaceFormat(arrivedFormat);
            format_ = arrivedFormat;
            applyHdrUpgrade();
            Logger::instance().info(L"capture",
                L"Capture surface changed to " + std::to_wstring(acquiredDesc.Width) + L"x" +
                std::to_wstring(acquiredDesc.Height) + L"; recreating texture pool for pinned HDR");
            if (!createTexturePool(width_, height_, format_, poolSize_)) {
                markLost(L"Texture pool recreate failed", E_FAIL);
                return false;
            }
        } else if (hdrPin_.pinnedHdr() && isHdrCaptureFormat(arrivedFormat) &&
            isHdrCaptureFormat(format_)) {
            // Genuine HDR<->HDR switch (PQ<->scRGB), rare: adopt it. An
            // SDR<->HDR flap never reaches here; it bridges with no recreate.
            format_ = arrivedFormat;
            Logger::instance().info(L"capture",
                L"Capture surface changed to " + std::to_wstring(acquiredDesc.Width) + L"x" +
                std::to_wstring(acquiredDesc.Height) + L"; recreating texture pool");
            if (!createTexturePool(width_, height_, format_, poolSize_)) {
                markLost(L"Texture pool recreate failed", E_FAIL);
                return false;
            }
            ++resolutionChanges_;
        } else if (hdrPin_.pinnedHdr() && !isHdrCaptureFormat(arrivedFormat)) {
            // SDR surface in a pinned HDR session: bridge it below. No pool
            // recreate, no GOP break; a resize still rebuilds at the pool format.
            if (sizeChanged) {
                Logger::instance().info(L"capture",
                    L"Capture surface changed to " + std::to_wstring(acquiredDesc.Width) + L"x" +
                    std::to_wstring(acquiredDesc.Height) + L"; recreating texture pool");
                if (!createTexturePool(width_, height_, format_, poolSize_)) {
                    markLost(L"Texture pool recreate failed", E_FAIL);
                    return false;
                }
                ++resolutionChanges_;
            }
        } else {
            // SDR session, SDR arrival (defensive format adopt; DD is BGRA).
            format_ = arrivedFormat;
            Logger::instance().info(L"capture",
                L"Capture surface changed to " + std::to_wstring(acquiredDesc.Width) + L"x" +
                std::to_wstring(acquiredDesc.Height) + L"; recreating texture pool");
            if (!createTexturePool(width_, height_, format_, poolSize_)) {
                markLost(L"Texture pool recreate failed", E_FAIL);
                return false;
            }
            // In-place resize/format switch opens a new GOP via the app's
            // existing resolutionChangeCount hook.
            ++resolutionChanges_;
        }
    }
    // Any remaining mismatch is an SDR surface in a pinned HDR session and
    // flows through the bridge; the pool format never flaps mid-session.
    const bool bridged = arrivedFormat != format_;

    const bool cursorUpdated = captureCursor_ && frameInfo.LastMouseUpdateTime.QuadPart != 0;
    const bool cursorShapeUpdated = captureCursor_ && frameInfo.PointerShapeBufferSize > 0;
    if (captureCursor_) {
        updateCursorState(frameInfo);
        if (cursorShapeUpdated) {
            refreshCursorShape(frameInfo.PointerShapeBufferSize);
        }
    }

    const bool desktopChanged = frameInfo.LastPresentTime.QuadPart != 0 || !haveContent_;
    // DuplicateOutput can signal without a desktop or cursor update. Returning the
    // cached texture in that case makes the recorder treat stale desktop content as
    // a fresh game frame.
    if (!desktopChanged && !cursorUpdated && !cursorShapeUpdated) {
        releaseHeldFrame();
        return false;
    }
    if (!desktopChanged) {
        const auto now = std::chrono::steady_clock::now();
        if (nextCursorOnlyFrameAt_.time_since_epoch().count() != 0 &&
            now < nextCursorOnlyFrameAt_) {
            releaseHeldFrame();
            return false;
        }
        nextCursorOnlyFrameAt_ = now + cursorOnlyFrameInterval_;
        ++cursorOnlyFrames_;
    }
    // Bridge resources build outside the context lock (device work only),
    // mirroring ensureCompositor below.
    if (desktopChanged && bridged && !ensureBridgeResources(width_, height_, arrivedFormat)) {
        releaseHeldFrame();
        return false;
    }
    if (desktopChanged) {
        std::scoped_lock lock(device_->immediateContextMutex());
        if (bridged) {
            // SDR surface in a pinned HDR session: stage it, then normalize
            // into the HDR desktop copy. Same mapping as native frames.
            device_->context()->CopyResource(bridgeStaging_.Get(), acquiredTexture.Get());
            normalizeIntoDesktopCopy();
        } else {
            device_->context()->CopyResource(desktopCopy_.Get(), acquiredTexture.Get());
        }
        haveContent_ = true;
    }

    releaseHeldFrame();

    if (!haveContent_) {
        return false;
    }

    auto slot = pool_.acquireOrLog(L"Capture texture pool exhausted");
    if (!slot) {
        return false;
    }

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
    // Prefer the real QPC present/mouse timestamps (same epoch as MSVC
    // steady_clock). When neither exists, fall back to the current steady
    // clock instead of fabricating evenly spaced 60 FPS stamps: fake cadences
    // drift against real audio/video timelines and corrupt clip durations.
    frame.pts100ns = qpcFrequency.QuadPart > 0 && timestamp != 0
        ? static_cast<int64_t>((static_cast<long double>(timestamp) * kHundredNanosecondsPerSecond)
            / static_cast<long double>(qpcFrequency.QuadPart))
        : std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count();
    frame.width = width_;
    frame.height = height_;
    frame.format = format_;
    // Pinned session, single mapping: every frame carries the pool format and
    // the pin, so the scaler programs its tone-map state once and never
    // alternates interpretations mid-GOP.
    frame.hdr = hdrPin_.pinnedHdr() && isHdrCaptureFormat(format_);
    if (bridged) {
        ++bridgedFrames_;
    }
    return true;
}

void DesktopDuplicationCapture::shutdown() {
    releaseHeldFrame();
    duplication_.Reset();
    haveContent_ = false;
    frameHeld_ = false;
    deviceLost_ = true;
    hdrPin_.reset();
    hdrPollState_ = false;
    bridgedFrames_ = 0;
    colorProbe_.Reset();
    lastHdrPollAt_ = {};
    monitor_ = nullptr;
    pool_.clear();
    desktopCopy_.Reset();
    desktopCopySrv_.Reset();
    desktopCopyRtv_.Reset();
    bridgeStaging_.Reset();
    bridgeStagingSrv_.Reset();
    bridgeStagingFormat_ = DXGI_FORMAT_UNKNOWN;
    bridgeVs_.Reset();
    bridgePs_.Reset();
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

bool DesktopDuplicationCapture::createTexturePool(uint32_t width, uint32_t height, DXGI_FORMAT format, uint32_t poolSize) {
    pool_.clear();
    desktopCopy_.Reset();
    desktopCopySrv_.Reset();
    haveContent_ = false;

    // RENDER_TARGET lets us blend the cursor into the slot; the downstream scaler
    // consumes slots as a video-processor input surface, so the extra bind flag is
    // free. SHADER_RESOURCE is retained for parity with the previous pool.
    GpuPoolBuildOptions poolOptions;
    poolOptions.wantRtv = true;
    poolOptions.wantSrv = true;
    std::vector<ComPtr<ID3D11Texture2D>> textures;
    if (!GpuPoolBuilder::build(device_->device(), width, height, format,
            poolOptions, poolSize, textures, L"capture pool")) {
        return false;
    }
    for (auto& texture : textures) {
        auto slot = std::make_shared<TextureSlot>();
        HRESULT hr = device_->device()->CreateRenderTargetView(texture.Get(), nullptr, &slot->renderTargetView);
        if (FAILED(hr)) {
            Logger::instance().error(L"capture", L"CreateRenderTargetView for capture pool failed: " + hresultToString(hr));
            pool_.clear();
            return false;
        }
        slot->texture = std::move(texture);
        pool_.push_back(std::move(slot));
    }

    // Persistent desktop image feeds both slot copies and cursor's destination
    // pixel logic, so it requires a shader-resource view.
    D3D11_TEXTURE2D_DESC copyDesc{};
    copyDesc.Width = width;
    copyDesc.Height = height;
    copyDesc.MipLevels = 1;
    copyDesc.ArraySize = 1;
    copyDesc.Format = format;
    copyDesc.SampleDesc.Count = 1;
    copyDesc.Usage = D3D11_USAGE_DEFAULT;
    // SHADER_RESOURCE feeds the cursor compositor; RENDER_TARGET receives the
    // SDR bridge normalization in a pinned HDR session. Both views coexist;
    // the bridge unbinds inputs before rendering (D3D11 forbids input+output).
    copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
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
    hr = device_->device()->CreateRenderTargetView(desktopCopy_.Get(), nullptr, &desktopCopyRtv_);
    if (FAILED(hr)) {
        Logger::instance().error(L"capture", L"CreateRenderTargetView for desktop copy failed: " + hresultToString(hr));
        pool_.clear();
        desktopCopy_.Reset();
        desktopCopySrv_.Reset();
        desktopCopyRtv_.Reset();
        return false;
    }

    pool_.setFootprintBytes(dxgiTextureBytes(width, height, format) * pool_.size());
    Logger::instance().debug(L"capture",
        L"Desktop Duplication texture pool initialized " +
        std::to_wstring(width) + L"x" + std::to_wstring(height) +
        L" " + poolFormatName(format) +
        L" pool=" + std::to_wstring(pool_.size()) +
        L" (" + std::to_wstring(pool_.footprintBytes() / 1024) + L" KiB)");

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

bool DesktopDuplicationCapture::ensureBridgeResources(uint32_t width, uint32_t height, DXGI_FORMAT sdrFormat) {
    if (!bridgeVs_ || !bridgePs_) {
        ComPtr<ID3DBlob> vsBlob;
        ComPtr<ID3DBlob> psBlob;
        ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3DCompile(kBridgeShaderHlsl, sizeof(kBridgeShaderHlsl) - 1, "bridge.hlsl", nullptr,
            nullptr, "VSMain", "vs_4_0", 0, 0, &vsBlob, &errorBlob);
        if (FAILED(hr)) {
            Logger::instance().error(L"capture", L"Bridge vertex shader compile failed: " + hresultToString(hr));
            return false;
        }
        hr = D3DCompile(kBridgeShaderHlsl, sizeof(kBridgeShaderHlsl) - 1, "bridge.hlsl", nullptr,
            nullptr, "PSMain", "ps_4_0", 0, 0, &psBlob, &errorBlob);
        if (FAILED(hr)) {
            Logger::instance().error(L"capture", L"Bridge pixel shader compile failed: " + hresultToString(hr));
            return false;
        }
        hr = device_->device()->CreateVertexShader(
            vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &bridgeVs_);
        if (FAILED(hr)) {
            Logger::instance().error(L"capture", L"Bridge CreateVertexShader failed: " + hresultToString(hr));
            return false;
        }
        hr = device_->device()->CreatePixelShader(
            psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &bridgePs_);
        if (FAILED(hr)) {
            Logger::instance().error(L"capture", L"Bridge CreatePixelShader failed: " + hresultToString(hr));
            bridgeVs_.Reset();
            return false;
        }
    }
    bool stagingFits = bridgeStaging_ && bridgeStagingFormat_ == sdrFormat;
    if (stagingFits) {
        D3D11_TEXTURE2D_DESC staged{};
        bridgeStaging_->GetDesc(&staged);
        stagingFits = staged.Width == width && staged.Height == height;
    }
    if (!stagingFits) {
        bridgeStaging_.Reset();
        bridgeStagingSrv_.Reset();
        // One texture, not a pool: a format wobble recreates this instead of
        // the 500 MB capture pool.
        D3D11_TEXTURE2D_DESC staged{};
        staged.Width = width;
        staged.Height = height;
        staged.MipLevels = 1;
        staged.ArraySize = 1;
        staged.Format = sdrFormat;
        staged.SampleDesc.Count = 1;
        staged.Usage = D3D11_USAGE_DEFAULT;
        staged.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device_->device()->CreateTexture2D(&staged, nullptr, &bridgeStaging_);
        if (FAILED(hr)) {
            Logger::instance().warning(L"capture", L"Bridge staging texture creation failed: " + hresultToString(hr));
            return false;
        }
        hr = device_->device()->CreateShaderResourceView(bridgeStaging_.Get(), nullptr, &bridgeStagingSrv_);
        if (FAILED(hr)) {
            Logger::instance().warning(L"capture", L"Bridge staging SRV creation failed: " + hresultToString(hr));
            bridgeStaging_.Reset();
            return false;
        }
        bridgeStagingFormat_ = sdrFormat;
    }
    return true;
}

void DesktopDuplicationCapture::normalizeIntoDesktopCopy() {
    // Caller holds the immediate-context mutex. Unbind shader inputs first:
    // desktopCopy_ is about to become a render target while its SRV may be
    // bound from the previous cursor composite.
    auto* ctx = device_->context();
    ID3D11ShaderResourceView* nullSrvs[] = {nullptr, nullptr};
    ctx->PSSetShaderResources(0, 2, nullSrvs);

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width_);
    viewport.Height = static_cast<float>(height_);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &viewport);

    ID3D11RenderTargetView* rtv = desktopCopyRtv_.Get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);

    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(bridgeVs_.Get(), nullptr, 0);
    ctx->PSSetShader(bridgePs_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = {bridgeStagingSrv_.Get()};
    ctx->PSSetShaderResources(0, 1, srvs);

    ctx->Draw(3, 0);

    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
    ctx->PSSetShaderResources(0, 1, nullSrvs);
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
        uint32_t hdrBypass; // 1 on pinned-HDR targets: the pool format (and the scaler mapping) never flaps mid-session, so cursor and tone-map always agree
    } constants{{leftNdc, topNdc, widthNdc, heightNdc}, {static_cast<float>(width_), static_cast<float>(height_)},
        cursorShapeType_,
        (hdrPin_.pinnedHdr() && isHdrCaptureFormat(format_)) ? 1u : 0u};
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
