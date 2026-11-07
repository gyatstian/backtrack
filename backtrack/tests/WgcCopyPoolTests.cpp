#include "capture/WgcCopyPool.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

backtrack::ComPtr<ID3D11Texture2D> makeTexture(
    ID3D11Device* device, uint32_t width, uint32_t height, const std::vector<uint32_t>& pixels) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixels.data();
    initial.SysMemPitch = width * static_cast<UINT>(sizeof(uint32_t));
    backtrack::ComPtr<ID3D11Texture2D> texture;
    expect(SUCCEEDED(device->CreateTexture2D(&desc, &initial, &texture)), "create input texture failed");
    return texture;
}

void expectTexture(ID3D11Device* device, ID3D11DeviceContext* context,
                   const backtrack::GpuFrame& frame, uint32_t width, uint32_t height,
                   const std::vector<uint32_t>& pixels) {
    D3D11_TEXTURE2D_DESC desc{};
    frame.texture->GetDesc(&desc);
    expect(frame.width == width && frame.height == height, "frame metadata has the wrong size");
    expect(desc.Width == width && desc.Height == height, "texture size disagrees with frame metadata");
    expect(frame.format == desc.Format && !frame.hdr, "frame format metadata is incorrect");

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    backtrack::ComPtr<ID3D11Texture2D> staging;
    expect(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)), "create staging texture failed");
    context->CopyResource(staging.Get(), frame.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    expect(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "map staging texture failed");
    bool matches = true;
    for (uint32_t y = 0; y < height; ++y) {
        const auto* row = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch;
        if (std::memcmp(row, pixels.data() + y * width, width * sizeof(uint32_t)) != 0) {
            matches = false;
            break;
        }
    }
    context->Unmap(staging.Get(), 0);
    expect(matches, "fallback copy did not contain the resized source pixels");
}

void testResizeThenFallback() {
    backtrack::ComPtr<ID3D11Device> device;
    backtrack::ComPtr<ID3D11DeviceContext> context;
    expect(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &device, nullptr, &context)), "create WARP device failed");

    backtrack::WgcCopyPool pool;
    uint64_t index = 0;
    const std::vector<uint32_t> pixelsA(12 * 8, 0xff112233);
    auto inputA = makeTexture(device.Get(), 12, 8, pixelsA);
    backtrack::GpuFrame frameA;
    expect(pool.emit(device.Get(), context.Get(), inputA.Get(), 12, 8, 5, index, 10, frameA),
           "first fallback failed");
    expectTexture(device.Get(), context.Get(), frameA, 12, 8, pixelsA);

    // Keep the old lease alive while forcing the first fallback at the new size.
    const std::vector<uint32_t> pixelsB(20 * 10, 0xffaabbcc);
    auto inputB = makeTexture(device.Get(), 20, 10, pixelsB);
    backtrack::GpuFrame frameB;
    expect(pool.emit(device.Get(), context.Get(), inputB.Get(), 20, 10, 5, index, 20, frameB),
           "fallback after resize failed");
    expect(frameB.frameIndex == 1 && frameB.pts100ns == 20, "fallback metadata did not advance");
    expectTexture(device.Get(), context.Get(), frameB, 20, 10, pixelsB);
    expectTexture(device.Get(), context.Get(), frameA, 12, 8, pixelsA);
}

} // namespace

int main() {
    try {
        testResizeThenFallback();
    } catch (const std::exception& error) {
        std::cerr << "WgcCopyPoolTests: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
