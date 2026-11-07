#pragma once

#include "capture/GpuPoolBuilder.h"

#include <utility>

namespace backtrack {

class WgcCopyPool {
    struct TextureSlot {
        ComPtr<ID3D11Texture2D> texture;
    };

public:
    void clear() {
        pool_.clear();
        width_ = 0;
        height_ = 0;
    }

    bool create(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t size) {
        clear();
        // Some NVIDIA drivers reject encoder registration without render-target binding.
        GpuPoolBuildOptions options;
        options.wantRtv = true;
        options.wantSrv = true;
        std::vector<ComPtr<ID3D11Texture2D>> textures;
        if (!GpuPoolBuilder::build(
                device, width, height, DXGI_FORMAT_B8G8R8A8_UNORM,
                options, size, textures, L"WGC pool")) {
            return false;
        }
        for (auto& texture : textures) {
            auto slot = std::make_shared<TextureSlot>();
            slot->texture = std::move(texture);
            pool_.push_back(std::move(slot));
        }
        width_ = width;
        height_ = height;
        pool_.setFootprintBytes(dxgiTextureBytes(width, height, DXGI_FORMAT_B8G8R8A8_UNORM) * pool_.size());
        Logger::instance().debug(L"capture",
            L"WGC texture pool initialized " + std::to_wstring(width) + L"x" +
            std::to_wstring(height) + L" pool=" + std::to_wstring(pool_.size()) +
            L" (" + std::to_wstring(pool_.footprintBytes() / 1024) + L" KiB)");
        return true;
    }

    // The caller holds the immediate-context mutex and has retired old encoder inputs on resize.
    bool emit(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* captured,
              uint32_t width, uint32_t height, uint32_t size, uint64_t& frameIndex,
              int64_t presentTime100ns, GpuFrame& output) {
        if ((pool_.empty() || width_ != width || height_ != height) &&
            !create(device, width, height, size)) {
            return false;
        }
        auto slot = pool_.acquireOrLog(L"WGC capture texture pool exhausted");
        if (!slot) {
            return false;
        }
        context->CopyResource(slot->texture.Get(), captured);
        output.texture = slot->texture;
        output.lease = slot;
        output.frameIndex = frameIndex++;
        output.pts100ns = presentTime100ns;
        output.width = width;
        output.height = height;
        output.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        output.hdr = false;
        return true;
    }

private:
    GpuTexturePool<TextureSlot> pool_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

} // namespace backtrack
