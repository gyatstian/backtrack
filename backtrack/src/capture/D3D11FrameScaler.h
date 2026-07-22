#pragma once

#include "capture/D3DDevice.h"
#include "core/Types.h"

#include <d3d11_1.h>

#include <memory>
#include <unordered_map>
#include <vector>

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
    void setPoolSize(uint32_t poolSize);
    void reset();

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
        uint32_t outputWidth,
        uint32_t outputHeight,
        DXGI_FORMAT outputFormat);
    void releaseResources();
    std::shared_ptr<TextureSlot> acquireSlot();
    ID3D11VideoProcessorInputView* inputViewFor(D3DDevice& device, ID3D11Texture2D* texture);
    ID3D11VideoProcessorInputView* copiedInputViewFor(D3DDevice& device, ID3D11Texture2D* texture);

    ComPtr<ID3D11VideoDevice> videoDevice_;
    ComPtr<ID3D11VideoContext> videoContext_;
    ComPtr<ID3D11VideoContext1> videoContext1_;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    ComPtr<ID3D11VideoProcessor> processor_;
    ComPtr<ID3D11Texture2D> copiedInputTexture_;
    ComPtr<ID3D11VideoProcessorInputView> copiedInputView_;
    std::vector<std::shared_ptr<TextureSlot>> pool_;
    std::unordered_map<ID3D11Texture2D*, ComPtr<ID3D11VideoProcessorInputView>> inputViews_;
    uint64_t poolExhaustionDrops_ = 0;
    bool useCopiedInput_ = false;
    bool nv12Unavailable_ = false;
    uint32_t inputWidth_ = 0;
    uint32_t inputHeight_ = 0;
    uint32_t outputWidth_ = 0;
    uint32_t outputHeight_ = 0;
    DXGI_FORMAT outputFormat_ = DXGI_FORMAT_UNKNOWN;
    uint32_t poolSize_ = 6;
};

} // namespace backtrack
