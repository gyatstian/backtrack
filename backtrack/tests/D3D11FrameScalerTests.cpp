#include "capture/D3D11FrameScaler.h"
#include "capture/GpuPoolBuilder.h"

#include <wrl/implements.h>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace backtrack {

struct D3D11FrameScalerTestAccess {
    static void setDevice(D3DDevice& device, ID3D11Device* d3d, ID3D11DeviceContext* context) {
        device.device_ = d3d;
        device.context_ = context;
    }

    static void setInput(D3D11FrameScaler& scaler, ID3D11VideoDevice* videoDevice,
                         uint32_t width, uint32_t height) {
        scaler.videoDevice_ = videoDevice;
        scaler.inputWidth_ = width;
        scaler.inputHeight_ = height;
    }

    static void prepareNv12Attempt(D3D11FrameScaler& scaler, ID3D11VideoProcessor* processor,
                                   uint32_t width, uint32_t height) {
        scaler.processor_ = processor;
        scaler.inputFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
        scaler.outputWidth_ = width;
        scaler.outputHeight_ = height;
        scaler.outputFormat_ = DXGI_FORMAT_NV12;
        scaler.pool_.push_back(std::make_shared<D3D11FrameScaler::TextureSlot>());
    }

    static ID3D11VideoProcessorInputView* inputView(
        D3D11FrameScaler& scaler, D3DDevice& device, ID3D11Texture2D* texture) {
        std::scoped_lock lock(device.immediateContextMutex());
        return scaler.inputViewFor(device, texture);
    }

    static bool nv12Unavailable(const D3D11FrameScaler& scaler) {
        return scaler.nv12Unavailable_;
    }
};

} // namespace backtrack

namespace {

using namespace backtrack;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

constexpr uint32_t kWidth = 16;
constexpr uint32_t kHeight = 8;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class FakeInputView final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ID3D11VideoProcessorInputView> {
public:
    FakeInputView(ID3D11Resource* resource, const D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC& desc)
        : resource_(resource), desc_(desc) {}

    void STDMETHODCALLTYPE GetDevice(ID3D11Device** device) override { *device = nullptr; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    void STDMETHODCALLTYPE GetResource(ID3D11Resource** resource) override { resource_.CopyTo(resource); }
    void STDMETHODCALLTYPE GetDesc(D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC* desc) override { *desc = desc_; }

private:
    ComPtr<ID3D11Resource> resource_;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc_{};
};

class FakeVideoDevice final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ID3D11VideoDevice> {
public:
    ID3D11Resource* rejectedSource = nullptr;
    bool rejectCopied = false;
    int directAttempts = 0;
    int copiedAttempts = 0;
    D3D11_TEXTURE2D_DESC copiedDesc{};
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC viewDesc{};

    HRESULT STDMETHODCALLTYPE CreateVideoProcessorInputView(
        ID3D11Resource* resource, ID3D11VideoProcessorEnumerator*,
        const D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC* desc,
        ID3D11VideoProcessorInputView** view) override {
        *view = nullptr;
        if (resource == rejectedSource) {
            ++directAttempts;
            return E_INVALIDARG;
        }
        ++copiedAttempts;
        static_cast<ID3D11Texture2D*>(resource)->GetDesc(&copiedDesc);
        viewDesc = *desc;
        if (directAttempts == 0 || rejectCopied || copiedDesc.BindFlags != 0) {
            return E_INVALIDARG;
        }
        auto result = Make<FakeInputView>(resource, *desc);
        if (!result) {
            return E_OUTOFMEMORY;
        }
        *view = result.Detach();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateVideoDecoder(const D3D11_VIDEO_DECODER_DESC*, const D3D11_VIDEO_DECODER_CONFIG*, ID3D11VideoDecoder**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateVideoProcessor(ID3D11VideoProcessorEnumerator*, UINT, ID3D11VideoProcessor**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateAuthenticatedChannel(D3D11_AUTHENTICATED_CHANNEL_TYPE, ID3D11AuthenticatedChannel**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateCryptoSession(const GUID*, const GUID*, const GUID*, ID3D11CryptoSession**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateVideoDecoderOutputView(ID3D11Resource*, const D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC*, ID3D11VideoDecoderOutputView**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateVideoProcessorOutputView(ID3D11Resource*, ID3D11VideoProcessorEnumerator*, const D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC*, ID3D11VideoProcessorOutputView**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateVideoProcessorEnumerator(const D3D11_VIDEO_PROCESSOR_CONTENT_DESC*, ID3D11VideoProcessorEnumerator**) override { return E_NOTIMPL; }
    UINT STDMETHODCALLTYPE GetVideoDecoderProfileCount() override { return 0; }
    HRESULT STDMETHODCALLTYPE GetVideoDecoderProfile(UINT, GUID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CheckVideoDecoderFormat(const GUID*, DXGI_FORMAT, BOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetVideoDecoderConfigCount(const D3D11_VIDEO_DECODER_DESC*, UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetVideoDecoderConfig(const D3D11_VIDEO_DECODER_DESC*, UINT, D3D11_VIDEO_DECODER_CONFIG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetContentProtectionCaps(const GUID*, const GUID*, D3D11_VIDEO_CONTENT_PROTECTION_CAPS*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CheckCryptoKeyExchange(const GUID*, const GUID*, UINT, GUID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
};

class FakeProcessor final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ID3D11VideoProcessor> {
public:
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** device) override { *device = nullptr; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    void STDMETHODCALLTYPE GetContentDesc(D3D11_VIDEO_PROCESSOR_CONTENT_DESC* desc) override { *desc = {}; }
    void STDMETHODCALLTYPE GetRateConversionCaps(D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS* caps) override { *caps = {}; }
};

void makeWarpDevice(D3DDevice& device) {
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID3D11DeviceContext> context;
    expect(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &d3d, nullptr, &context)), "create WARP device failed");
    D3D11FrameScalerTestAccess::setDevice(device, d3d.Get(), context.Get());
}

ComPtr<ID3D11Texture2D> makeTexture(ID3D11Device* device, uint32_t color) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const std::vector<uint32_t> pixels(kWidth * kHeight, color);
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixels.data();
    initial.SysMemPitch = kWidth * static_cast<UINT>(sizeof(uint32_t));
    ComPtr<ID3D11Texture2D> texture;
    expect(SUCCEEDED(device->CreateTexture2D(&desc, &initial, &texture)), "create source texture failed");
    return texture;
}

void expectCopiedPixels(D3DDevice& device, ID3D11VideoProcessorInputView* view, uint32_t color) {
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> copied;
    expect(SUCCEEDED(resource.As(&copied)), "copied view has no texture");
    D3D11_TEXTURE2D_DESC desc{};
    copied->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    expect(SUCCEEDED(device.device()->CreateTexture2D(&desc, nullptr, &staging)), "create staging texture failed");
    std::scoped_lock lock(device.immediateContextMutex());
    device.context()->CopyResource(staging.Get(), copied.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    expect(SUCCEEDED(device.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "map staging texture failed");
    bool matches = true;
    for (uint32_t y = 0; y < kHeight; ++y) {
        const auto* row = reinterpret_cast<const uint32_t*>(
            static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch);
        for (uint32_t x = 0; x < kWidth; ++x) {
            matches = matches && row[x] == color;
        }
    }
    device.context()->Unmap(staging.Get(), 0);
    expect(matches, "copied view did not contain the source pixels");
}

void testDirectRejectionUsesUnboundInputCopy() {
    expect(GpuPoolBuilder::bindFlagsFor({}) == static_cast<UINT>(D3D11_BIND_SHADER_RESOURCE),
           "default pool options changed");
    GpuPoolBuildOptions outputOptions;
    outputOptions.wantOutputView = true;
    expect(GpuPoolBuilder::bindFlagsFor(outputOptions) ==
               static_cast<UINT>(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET),
           "output pool options changed");

    D3DDevice device;
    makeWarpDevice(device);
    auto source = makeTexture(device.device(), 0xff112233);
    auto videoDevice = Make<FakeVideoDevice>();
    expect(videoDevice != nullptr, "create fake video device failed");
    videoDevice->rejectedSource = source.Get();
    D3D11FrameScaler scaler;
    D3D11FrameScalerTestAccess::setInput(scaler, videoDevice.Get(), kWidth, kHeight);

    auto* view = D3D11FrameScalerTestAccess::inputView(scaler, device, source.Get());
    expect(videoDevice->directAttempts == 1 && videoDevice->copiedAttempts == 1,
           "direct and copied input views were not attempted in order");
    const auto& desc = videoDevice->copiedDesc;
    expect(desc.Width == kWidth && desc.Height == kHeight &&
               desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM &&
               desc.MipLevels == 1 && desc.ArraySize == 1 &&
               desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0 &&
               desc.Usage == D3D11_USAGE_DEFAULT && desc.BindFlags == 0 &&
               desc.CPUAccessFlags == 0 && desc.MiscFlags == 0,
           "copied input texture descriptor is incorrect");
    expect(view != nullptr, "direct rejection did not produce a copied input view");
    expect(videoDevice->viewDesc.ViewDimension == D3D11_VPIV_DIMENSION_TEXTURE2D &&
               videoDevice->viewDesc.Texture2D.MipSlice == 0 &&
               videoDevice->viewDesc.Texture2D.ArraySlice == 0,
           "copied input view descriptor is incorrect");
    expect(!D3D11FrameScalerTestAccess::nv12Unavailable(scaler), "input rejection latched NV12 unavailable");
    expectCopiedPixels(device, view, 0xff112233);

    auto next = makeTexture(device.device(), 0xffaabbcc);
    auto* nextView = D3D11FrameScalerTestAccess::inputView(scaler, device, next.Get());
    expect(nextView == view && videoDevice->directAttempts == 1 && videoDevice->copiedAttempts == 1,
           "copied input view was not reused");
    expectCopiedPixels(device, nextView, 0xffaabbcc);
}

void testInputFailureDoesNotDowngradeNv12() {
    D3DDevice device;
    makeWarpDevice(device);
    auto source = makeTexture(device.device(), 0xff445566);
    auto videoDevice = Make<FakeVideoDevice>();
    auto processor = Make<FakeProcessor>();
    expect(videoDevice != nullptr && processor != nullptr, "create fake video processor failed");
    videoDevice->rejectedSource = source.Get();
    videoDevice->rejectCopied = true;
    D3D11FrameScaler scaler;
    D3D11FrameScalerTestAccess::setInput(scaler, videoDevice.Get(), kWidth, kHeight);
    D3D11FrameScalerTestAccess::prepareNv12Attempt(scaler, processor.Get(), kWidth, kHeight);
    GpuFrame input;
    input.texture = source;
    input.width = kWidth;
    input.height = kHeight;
    input.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    GpuFrame output;
    expect(!scaler.scale(device, input, kWidth, kHeight, output, false, false, DXGI_FORMAT_NV12),
           "failed input views unexpectedly produced a frame");
    expect(videoDevice->directAttempts == 1 && videoDevice->copiedAttempts == 1,
           "input view failures were not exercised");
    expect(!D3D11FrameScalerTestAccess::nv12Unavailable(scaler),
           "input view failure incorrectly latched NV12 unavailable");

    videoDevice->rejectCopied = false;
    auto* view = D3D11FrameScalerTestAccess::inputView(scaler, device, source.Get());
    expect(view != nullptr && videoDevice->directAttempts == 1 && videoDevice->copiedAttempts == 2,
           "copied input view did not recover after a transient failure");
    expectCopiedPixels(device, view, 0xff445566);
}

} // namespace

int main() {
    try {
        testDirectRejectionUsesUnboundInputCopy();
        testInputFailureDoesNotDowngradeNv12();
    } catch (const std::exception& error) {
        std::cerr << "D3D11FrameScalerTests: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
