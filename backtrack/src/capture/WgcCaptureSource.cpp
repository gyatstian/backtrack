#include "capture/WgcCaptureSource.h"

#include "capture/D3DDevice.h"
#include "core/Logger.h"
#include "platform/Win32Util.h"

#include <Windows.h>
#include <d3d11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

namespace backtrack {

struct WgcCaptureSource::Impl {
    struct TextureSlot {
        ComPtr<ID3D11Texture2D> texture;
    };

    struct DirectFrameLease {
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame{nullptr};
    };

    D3DDevice* device = nullptr;
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session{nullptr};
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrtDevice{nullptr};
    winrt::event_token frameArrivedToken{};
    winrt::event_token closedToken{};
    HANDLE frameEvent = nullptr;
    std::mutex frameEventMutex;
    std::vector<std::shared_ptr<TextureSlot>> pool;
    uint64_t poolExhaustionDrops = 0;
    uint64_t zeroCopyLeaseDrops = 0;
    uint64_t frameIndex = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool zeroCopy = true;
    bool captureCursor = true;
    uint32_t texturePoolSize = 6;
    uint32_t framePoolSize = 5;
    // Live Direct3D11CaptureFrame leases held by in-flight GpuFrames (zero-copy).
    // shared_ptr so lease deleters stay valid if Impl is destroyed first.
    std::shared_ptr<std::atomic<uint32_t>> outstandingZeroCopyLeases =
        std::make_shared<std::atomic<uint32_t>>(0);
    std::atomic<bool> deviceLost{false};

    ~Impl() {
        shutdown();
    }

    bool initialize(D3DDevice& d3d, const AppSettings& settings, const CaptureTarget& target) {
        device = &d3d;
        deviceLost.store(false);
        outstandingZeroCopyLeases = std::make_shared<std::atomic<uint32_t>>(0);
        zeroCopyLeaseDrops = 0;
        zeroCopy = settings.gpu.wgcZeroCopy;
        captureCursor = settings.captureCursor;
        // +1 for lastFrame pin in captureLoop (keeps one copy-path pool slot in use).
        texturePoolSize = std::clamp<uint32_t>(settings.gpu.frameQueueLimit + 4, 5, 16);
        // Bound WGC pool so in-flight zero-copy leases cannot exhaust it.
        // Keep at least one free slot for the next FrameArrived.
        framePoolSize = std::clamp<uint32_t>(settings.gpu.frameQueueLimit + 2, 3, 8);
        frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!frameEvent) {
            Logger::instance().error(L"CreateEvent for WGC failed");
            return false;
        }

        if (!winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported()) {
            Logger::instance().warning(L"Windows Graphics Capture is not supported by this OS/session");
            return false;
        }

        HMONITOR monitor = target.monitor ? target.monitor : monitorFromIndex(target.monitorIndex);
        if (!monitor) {
            Logger::instance().warning(L"No monitor handle available for Windows Graphics Capture");
            return false;
        }

        try {
            auto factory = winrt::get_activation_factory<
                winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                IGraphicsCaptureItemInterop>();
            winrt::check_hresult(factory->CreateForMonitor(
                monitor,
                winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                winrt::put_abi(item)));

            ComPtr<IDXGIDevice> dxgiDevice;
            HRESULT hr = d3d.device()->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
            if (FAILED(hr)) {
                Logger::instance().error(L"WGC could not query IDXGIDevice: " + hresultToString(hr));
                return false;
            }

            winrt::com_ptr<IInspectable> inspectable;
            winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put()));
            winrtDevice = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

            const auto size = item.Size();
            width = static_cast<uint32_t>(size.Width);
            height = static_cast<uint32_t>(size.Height);
            if (!zeroCopy && !createTexturePool(width, height)) {
                return false;
            }

            framePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
                winrtDevice,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                static_cast<int32_t>(framePoolSize),
                size);
            frameArrivedToken = framePool.FrameArrived([this](auto&, auto&) {
                signalFrameEvent();
            });
            closedToken = item.Closed([this](auto&, auto&) {
                deviceLost.store(true);
                signalFrameEvent();
            });

            session = framePool.CreateCaptureSession(item);
            session.IsCursorCaptureEnabled(captureCursor);
            // Disabling the capture border is a best-effort cosmetic step. Keep it asynchronous so
            // capture startup does not depend on a WinRT permission operation completing inline.
            try {
                auto accessOperation =
                    winrt::Windows::Graphics::Capture::GraphicsCaptureAccess::RequestAccessAsync(
                        winrt::Windows::Graphics::Capture::GraphicsCaptureAccessKind::Borderless);
                auto capturedSession = session;
                accessOperation.Completed(
                    [capturedSession](
                        const winrt::Windows::Foundation::IAsyncOperation<
                            winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus>&
                            operation,
                        winrt::Windows::Foundation::AsyncStatus) {
                        try {
                            if (operation.GetResults() ==
                                winrt::Windows::Security::Authorization::AppCapabilityAccess::
                                    AppCapabilityAccessStatus::Allowed) {
                                capturedSession.IsBorderRequired(false);
                            } else {
                                Logger::instance().warning(
                                    L"Borderless capture access was not granted; Windows may show a capture border");
                            }
                        } catch (const winrt::hresult_error& error) {
                            Logger::instance().warning(
                                L"Could not disable Windows Graphics Capture border: " +
                                std::wstring(error.message().c_str()));
                        }
                    });
            } catch (const winrt::hresult_error& error) {
                Logger::instance().warning(
                    L"Could not request borderless Windows Graphics Capture: " +
                    std::wstring(error.message().c_str()));
            }
            session.StartCapture();
        } catch (const winrt::hresult_error& error) {
            Logger::instance().warning(L"Windows Graphics Capture initialization failed: " + std::wstring(error.message().c_str()));
            return false;
        }

        Logger::instance().debug(L"Windows Graphics Capture initialized at " + std::to_wstring(width) + L"x" + std::to_wstring(height));
        return true;
    }

    bool acquire(GpuFrame& output, uint32_t timeoutMs) {
        if (!framePool || deviceLost.load()) {
            return false;
        }

        const DWORD wait = WaitForSingleObject(frameEvent, timeoutMs);
        if (wait != WAIT_OBJECT_0) {
            return false;
        }

        try {
            auto frame = framePool.TryGetNextFrame();
            if (!frame) {
                return false;
            }

            const auto size = frame.ContentSize();
            if (size.Width <= 0 || size.Height <= 0) {
                return false;
            }
            if (static_cast<uint32_t>(size.Width) != width || static_cast<uint32_t>(size.Height) != height) {
                width = static_cast<uint32_t>(size.Width);
                height = static_cast<uint32_t>(size.Height);
                if (!zeroCopy && !createTexturePool(width, height)) {
                    deviceLost.store(true);
                    return false;
                }
                framePool.Recreate(
                    winrtDevice,
                    winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                    static_cast<int32_t>(framePoolSize),
                    size);
                return false;
            }

            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> capturedTexture;
            HRESULT hr = access->GetInterface(IID_PPV_ARGS(&capturedTexture));
            if (FAILED(hr)) {
                Logger::instance().warning(L"WGC surface did not expose ID3D11Texture2D: " + hresultToString(hr));
                return false;
            }

            if (zeroCopy) {
                // Keep at least one WGC pool slot free so TryGetNextFrame keeps working.
                // Each zero-copy lease holds a Direct3D11CaptureFrame until encode finishes.
                const uint32_t outstanding =
                    outstandingZeroCopyLeases->load(std::memory_order_acquire);
                // Keep ≥1 WGC pool slot free so FrameArrived / TryGetNextFrame keep working.
                const uint32_t maxLeases = framePoolSize > 1 ? framePoolSize - 1 : 1;
                if (outstanding >= maxLeases) {
                    ++zeroCopyLeaseDrops;
                    if (zeroCopyLeaseDrops == 1 || (zeroCopyLeaseDrops % 300) == 0) {
                        Logger::instance().warning(
                            L"WGC zero-copy lease limit reached; falling back to copy path count=" +
                            std::to_wstring(zeroCopyLeaseDrops) +
                            L" outstanding=" + std::to_wstring(outstanding) +
                            L" pool=" + std::to_wstring(framePoolSize));
                    }
                    // Force a copy so the Direct3D11CaptureFrame is released immediately.
                    if (pool.empty() && !createTexturePool(width, height)) {
                        return false;
                    }
                    auto slot = acquireSlot();
                    if (!slot) {
                        ++poolExhaustionDrops;
                        if (poolExhaustionDrops == 1 || (poolExhaustionDrops % 300) == 0) {
                            Logger::instance().warning(
                                L"WGC capture texture pool exhausted; dropping frame count=" +
                                std::to_wstring(poolExhaustionDrops));
                        }
                        return false;
                    }
                    poolExhaustionDrops = 0;
                    {
                        std::scoped_lock lock(device->immediateContextMutex());
                        device->context()->CopyResource(slot->texture.Get(), capturedTexture.Get());
                    }
                    // frame goes out of scope after this block → WGC pool slot freed.
                    output.texture = slot->texture;
                    output.lease = slot;
                    output.frameIndex = frameIndex++;
                    output.pts100ns = std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count();
                    output.width = width;
                    output.height = height;
                    output.format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    return true;
                }

                auto counter = outstandingZeroCopyLeases;
                auto lease = std::shared_ptr<DirectFrameLease>(
                    new DirectFrameLease{},
                    [counter](DirectFrameLease* ptr) {
                        delete ptr;
                        counter->fetch_sub(1, std::memory_order_acq_rel);
                    });
                lease->frame = frame;
                counter->fetch_add(1, std::memory_order_acq_rel);

                output.texture = capturedTexture;
                output.lease = std::move(lease);
                output.frameIndex = frameIndex++;
                output.pts100ns = std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count();
                output.width = width;
                output.height = height;
                output.format = DXGI_FORMAT_B8G8R8A8_UNORM;
                return true;
            }

            auto slot = acquireSlot();
            if (!slot) {
                ++poolExhaustionDrops;
                if (poolExhaustionDrops == 1 || (poolExhaustionDrops % 300) == 0) {
                    Logger::instance().warning(
                        L"WGC capture texture pool exhausted; dropping frame count=" +
                        std::to_wstring(poolExhaustionDrops));
                }
                return false;
            }
            poolExhaustionDrops = 0;

            {
                std::scoped_lock lock(device->immediateContextMutex());
                device->context()->CopyResource(slot->texture.Get(), capturedTexture.Get());
            }

            output.texture = slot->texture;
            output.lease = slot;
            output.frameIndex = frameIndex++;
            output.pts100ns = std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count();
            output.width = width;
            output.height = height;
            output.format = DXGI_FORMAT_B8G8R8A8_UNORM;
            return true;
        } catch (const winrt::hresult_error& error) {
            Logger::instance().warning(L"Windows Graphics Capture frame acquisition failed: " + std::wstring(error.message().c_str()));
            deviceLost.store(true);
            return false;
        }
    }

    void shutdown() {
        if (framePool && frameArrivedToken.value) {
            framePool.FrameArrived(frameArrivedToken);
        }
        if (item && closedToken.value) {
            item.Closed(closedToken);
        }
        if (session) {
            session.Close();
            session = nullptr;
        }
        if (framePool) {
            framePool.Close();
            framePool = nullptr;
        }
        item = nullptr;
        winrtDevice = nullptr;
        pool.clear();
        {
            std::scoped_lock lock(frameEventMutex);
            if (frameEvent) {
                CloseHandle(frameEvent);
                frameEvent = nullptr;
            }
        }
        device = nullptr;
    }

    void signalFrameEvent() {
        std::scoped_lock lock(frameEventMutex);
        if (frameEvent) {
            SetEvent(frameEvent);
        }
    }

    std::shared_ptr<TextureSlot> acquireSlot() {
        for (auto& slot : pool) {
            if (slot.use_count() == 1) {
                return slot;
            }
        }
        return {};
    }

    bool createTexturePool(uint32_t newWidth, uint32_t newHeight) {
        pool.clear();
        poolExhaustionDrops = 0;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = newWidth;
        desc.Height = newHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        for (uint32_t index = 0; index < texturePoolSize; ++index) {
            auto slot = std::make_shared<TextureSlot>();
            HRESULT hr = device->device()->CreateTexture2D(&desc, nullptr, &slot->texture);
            if (FAILED(hr)) {
                Logger::instance().error(L"CreateTexture2D for WGC pool failed: " + hresultToString(hr));
                pool.clear();
                return false;
            }
            pool.push_back(std::move(slot));
        }
        return true;
    }
};

WgcCaptureSource::WgcCaptureSource()
    : impl_(std::make_unique<Impl>()) {
}

WgcCaptureSource::~WgcCaptureSource() {
    shutdown();
}

bool WgcCaptureSource::initialize(D3DDevice& device, const AppSettings& settings, const CaptureTarget& target) {
    return impl_->initialize(device, settings, target);
}

bool WgcCaptureSource::acquireNextFrame(GpuFrame& frame, uint32_t timeoutMs) {
    return impl_->acquire(frame, timeoutMs);
}

void WgcCaptureSource::shutdown() {
    if (impl_) {
        impl_->shutdown();
    }
}

bool WgcCaptureSource::isDeviceLost() const {
    return impl_->deviceLost.load();
}

uint32_t WgcCaptureSource::width() const {
    return impl_->width;
}

uint32_t WgcCaptureSource::height() const {
    return impl_->height;
}

} // namespace backtrack
