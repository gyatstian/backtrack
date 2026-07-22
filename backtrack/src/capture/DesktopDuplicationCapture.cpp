#include "capture/DesktopDuplicationCapture.h"

#include "core/Logger.h"

#include <algorithm>

namespace backtrack {

namespace {

uint32_t captureTexturePoolSize(const AppSettings& settings) {
    return std::clamp<uint32_t>(settings.gpu.frameQueueLimit + 3, 4, 16);
}

} // namespace

bool DesktopDuplicationCapture::initialize(D3DDevice& device, const AppSettings& settings, const CaptureTarget& target) {
    device_ = &device;
    deviceLost_ = false;
    frameIndex_ = 0;
    poolExhaustionDrops_ = 0;

    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = device.device()->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr)) {
        Logger::instance().error(L"ID3D11Device does not expose IDXGIDevice: " + hresultToString(hr));
        return false;
    }

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) {
        Logger::instance().error(L"IDXGIDevice::GetAdapter failed: " + hresultToString(hr));
        return false;
    }

    ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(target.monitorIndex, &output);
    if (FAILED(hr)) {
        Logger::instance().warning(L"Requested monitor output not found; falling back to output 0");
        hr = adapter->EnumOutputs(0, &output);
    }
    if (FAILED(hr)) {
        Logger::instance().error(L"No DXGI output available for desktop duplication: " + hresultToString(hr));
        return false;
    }

    DXGI_OUTPUT_DESC outputDesc{};
    output->GetDesc(&outputDesc);
    width_ = static_cast<uint32_t>(outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left);
    height_ = static_cast<uint32_t>(outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top);

    ComPtr<IDXGIOutput1> output1;
    hr = output.As(&output1);
    if (FAILED(hr)) {
        Logger::instance().error(L"IDXGIOutput1 unavailable: " + hresultToString(hr));
        return false;
    }

    hr = output1->DuplicateOutput(device.device(), &duplication_);
    if (FAILED(hr)) {
        Logger::instance().error(L"DuplicateOutput failed: " + hresultToString(hr));
        return false;
    }

    if (!createTexturePool(width_, height_, format_, captureTexturePoolSize(settings))) {
        return false;
    }

    Logger::instance().info(
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
        Logger::instance().warning(L"Desktop duplication access lost; capture must be recreated");
        deviceLost_ = true;
        return false;
    }
    if (FAILED(hr)) {
        Logger::instance().warning(L"AcquireNextFrame failed: " + hresultToString(hr));
        return false;
    }

    ComPtr<ID3D11Texture2D> acquiredTexture;
    hr = resource.As(&acquiredTexture);
    if (FAILED(hr)) {
        duplication_->ReleaseFrame();
        Logger::instance().warning(L"Captured resource is not ID3D11Texture2D: " + hresultToString(hr));
        return false;
    }

    auto slot = acquireSlot();
    if (!slot) {
        duplication_->ReleaseFrame();
        ++poolExhaustionDrops_;
        if (poolExhaustionDrops_ == 1 || (poolExhaustionDrops_ % 300) == 0) {
            Logger::instance().warning(
                L"Capture texture pool exhausted; dropping frame count=" +
                std::to_wstring(poolExhaustionDrops_));
        }
        return false;
    }
    poolExhaustionDrops_ = 0;

    {
        std::scoped_lock lock(device_->immediateContextMutex());
        device_->context()->CopyResource(slot->texture.Get(), acquiredTexture.Get());
    }

    duplication_->ReleaseFrame();

    frame.texture = slot->texture;
    frame.lease = slot;
    frame.frameIndex = frameIndex_++;
    static LARGE_INTEGER qpcFrequency{};
    if (qpcFrequency.QuadPart == 0) {
        QueryPerformanceFrequency(&qpcFrequency);
    }
    frame.pts100ns = qpcFrequency.QuadPart > 0
        ? static_cast<int64_t>((static_cast<long double>(frameInfo.LastPresentTime.QuadPart) * kHundredNanosecondsPerSecond)
            / static_cast<long double>(qpcFrequency.QuadPart))
        : static_cast<int64_t>(frame.frameIndex * (kHundredNanosecondsPerSecond / 60));
    frame.width = width_;
    frame.height = height_;
    frame.format = format_;
    return true;
}

void DesktopDuplicationCapture::shutdown() {
    duplication_.Reset();
    pool_.clear();
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
    poolExhaustionDrops_ = 0;
    poolSize = std::clamp<uint32_t>(poolSize, 4, 16);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    for (uint32_t index = 0; index < poolSize; ++index) {
        auto slot = std::make_shared<TextureSlot>();
        HRESULT hr = device_->device()->CreateTexture2D(&desc, nullptr, &slot->texture);
        if (FAILED(hr)) {
            Logger::instance().error(L"CreateTexture2D for capture pool failed: " + hresultToString(hr));
            pool_.clear();
            return false;
        }
        pool_.push_back(std::move(slot));
    }

    return true;
}

} // namespace backtrack
