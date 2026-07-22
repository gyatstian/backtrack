#include "capture/D3DDevice.h"

#include "core/Logger.h"

#include <d3d11_4.h>

namespace backtrack {

bool D3DDevice::initialize(uint32_t adapterIndex) {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        Logger::instance().error(L"CreateDXGIFactory1 failed: " + hresultToString(hr));
        return false;
    }

    hr = factory->EnumAdapters1(adapterIndex, &adapter_);
    if (FAILED(hr)) {
        Logger::instance().warning(L"Requested adapter not found; using default hardware adapter");
        adapter_.Reset();
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    const D3D_FEATURE_LEVEL requestedLevels[] = {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    D3D_FEATURE_LEVEL createdLevel = D3D_FEATURE_LEVEL_11_0;
    hr = D3D11CreateDevice(
        adapter_.Get(),
        adapter_ ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        requestedLevels,
        ARRAYSIZE(requestedLevels),
        D3D11_SDK_VERSION,
        &device_,
        &createdLevel,
        &context_);

#if defined(_DEBUG)
    if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            adapter_.Get(),
            adapter_ ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            requestedLevels,
            ARRAYSIZE(requestedLevels),
            D3D11_SDK_VERSION,
            &device_,
            &createdLevel,
            &context_);
    }
#endif

    if (FAILED(hr)) {
        Logger::instance().error(L"D3D11CreateDevice failed: " + hresultToString(hr));
        return false;
    }

    if (!adapter_) {
        ComPtr<IDXGIDevice> dxgiDevice;
        if (SUCCEEDED(device_.As(&dxgiDevice))) {
            ComPtr<IDXGIAdapter> baseAdapter;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&baseAdapter))) {
                baseAdapter.As(&adapter_);
            }
        }
    }

    if (adapter_) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter_->GetDesc1(&desc);
        adapterName_ = desc.Description;
        vendorId_ = desc.VendorId;
        switch (desc.VendorId) {
        case 0x10DE:
            vendor_ = GpuVendor::Nvidia;
            break;
        case 0x1002:
        case 0x1022:
            vendor_ = GpuVendor::Amd;
            break;
        case 0x8086:
            vendor_ = GpuVendor::Intel;
            break;
        default:
            vendor_ = GpuVendor::Unknown;
            break;
        }
    }

    Logger::instance().info(L"D3D11 device initialized on adapter: " + adapterName_);
    return true;
}

} // namespace backtrack
