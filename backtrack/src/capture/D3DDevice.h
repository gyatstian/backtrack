#pragma once

#include "core/Types.h"

#include <mutex>

namespace backtrack {

class D3DDevice {
public:
    bool initialize(uint32_t adapterIndex = 0);

    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }
    IDXGIAdapter1* adapter() const { return adapter_.Get(); }
    const std::wstring& adapterName() const { return adapterName_; }
    GpuVendor vendor() const { return vendor_; }
    uint32_t vendorId() const { return vendorId_; }

    std::mutex& immediateContextMutex() { return contextMutex_; }

private:
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIAdapter1> adapter_;
    std::wstring adapterName_;
    GpuVendor vendor_ = GpuVendor::Unknown;
    uint32_t vendorId_ = 0;
    std::mutex contextMutex_;
};

} // namespace backtrack
