#pragma once

#include "capture/GpuTexturePool.h"
#include "core/Logger.h"
#include "core/Types.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace backtrack {

// Pool-build options. Only texture BindFlags are derived from these; views
// stay with the owner (RTV for cursor/BGRA-clear, VP output views need the
// video device, desktopCopy/SRV is a DD sidecar).
struct GpuPoolBuildOptions {
    bool wantRtv = false;
    bool wantSrv = true;
    // Video-processor output: implies RTV bind. The view itself is built by
    // the owner from the returned textures.
    bool wantOutputView = false;
};

class GpuPoolBuilder {
public:
    // Bind-flag matrix. New formats (e.g. P010) need exactly one edit:
    // dxgiTextureBytes for their footprint; flags stay options-driven.
    //   WGC copy pool / DD pool      {rtv, srv}       -> SRV|RTV
    //   Scaler output / copy pool    {outputView, srv iff BGRA} -> RTV (+SRV)
    //   VP input copy texture        {}               -> 0 (strict drivers
    //                                                reject SRV with E_INVALIDARG)
    static UINT bindFlagsFor(const GpuPoolBuildOptions& options) {
        UINT flags = 0;
        if (options.wantSrv) {
            flags |= D3D11_BIND_SHADER_RESOURCE;
        }
        if (options.wantRtv || options.wantOutputView) {
            flags |= D3D11_BIND_RENDER_TARGET;
        }
        return flags;
    }

    // Single clamp for every texture pool (covers the capture loop's lastFrame
    // pin, so the floor is above the queue limit's minimum).
    static uint32_t normalizePoolSize(uint32_t count) {
        return std::clamp<uint32_t>(count, kMinTexturePoolSize, kMaxTexturePoolSize);
    }

    // Creates `count` (normalized) textures into `out`; false on failure with
    // `out` cleared. Views and footprint stay with the owner.
    static bool build(
        ID3D11Device* device,
        uint32_t width,
        uint32_t height,
        DXGI_FORMAT format,
        const GpuPoolBuildOptions& options,
        uint32_t count,
        std::vector<ComPtr<ID3D11Texture2D>>& out,
        const wchar_t* poolLogName) {
        out.clear();
        const uint32_t normalized = normalizePoolSize(count);

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = bindFlagsFor(options);

        for (uint32_t index = 0; index < normalized; ++index) {
            ComPtr<ID3D11Texture2D> texture;
            const HRESULT hr = device->CreateTexture2D(&desc, nullptr, &texture);
            if (FAILED(hr)) {
                Logger::instance().warning(L"capture",
                    std::wstring(L"CreateTexture2D for ") + poolLogName +
                    L" failed: " + hresultToString(hr));
                out.clear();
                return false;
            }
            out.push_back(std::move(texture));
        }
        return true;
    }
};

} // namespace backtrack
