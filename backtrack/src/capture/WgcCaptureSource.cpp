#include "capture/WgcCaptureSource.h"

#include "capture/D3DDevice.h"
#include "capture/GpuPoolBuilder.h"
#include "capture/GpuTexturePool.h"
#include "core/Logger.h"
#include "platform/Win32Util.h"

#include <Windows.h>
#include <d3d11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
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
    // High-resolution cadence timer. Lets acquire() honor sub-15ms timeouts even
    // if the global timer period was not raised (timeBeginPeriod denied). nullptr
    // falls back to the coarse WaitForSingleObject path.
    HANDLE cadenceTimer = nullptr;
    GpuTexturePool<TextureSlot> pool;
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
    // In-place content resizes handled via FramePool.Recreate. The capture
    // loop watches this counter to force an encoder IDR across size jumps.
    std::atomic<uint64_t> resolutionChanges{0};
    // Pending in-place resize: Recreate is deferred until every frame of the
    // old pool has been released (see completePendingPoolRecreate).
    winrt::Windows::Graphics::SizeInt32 pendingResize{};
    bool recreatePending = false;
    uint32_t recreateDeferrals = 0;

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
        texturePoolSize = texturePoolSizeForQueueLimit(settings.gpu.frameQueueLimit);
        // Bound WGC pool so in-flight zero-copy leases cannot exhaust it.
        // Keep at least one free slot for the next FrameArrived.
        framePoolSize = wgcFramePoolSizeForQueueLimit(settings.gpu.frameQueueLimit);
        // FrameArrived can fire while capture work is running. Keep notification
        // asserted until the consumer has drained the frame pool.
        frameEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!frameEvent) {
            Logger::instance().error(L"capture", L"CreateEvent for WGC failed");
            return false;
        }
        // Best-effort; nullptr just means acquire() uses the coarse wait path.
        cadenceTimer = createHighResolutionWaitableTimer();

        if (!winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported()) {
            Logger::instance().warning(L"capture", L"Windows Graphics Capture is not supported by this OS/session");
            return false;
        }

        HMONITOR monitor = target.monitor ? target.monitor : monitorFromIndex(target.monitorIndex);
        const HWND window = (target.window && IsWindow(target.window)) ? target.window : nullptr;
        if (!window && !monitor) {
            Logger::instance().warning(L"capture", L"No monitor/window handle available for Windows Graphics Capture");
            return false;
        }

        try {
            auto factory = winrt::get_activation_factory<
                winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                IGraphicsCaptureItemInterop>();
            if (window) {
                Logger::instance().debug(L"capture", L"WGC CreateForWindow");
                winrt::check_hresult(factory->CreateForWindow(
                    window,
                    winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                    winrt::put_abi(item)));
            } else {
                Logger::instance().debug(L"capture", L"WGC CreateForMonitor");
                winrt::check_hresult(factory->CreateForMonitor(
                    monitor,
                    winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                    winrt::put_abi(item)));
            }

            ComPtr<IDXGIDevice> dxgiDevice;
            HRESULT hr = d3d.device()->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
            if (FAILED(hr)) {
                Logger::instance().error(L"capture", L"WGC could not query IDXGIDevice: " + hresultToString(hr));
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
            Logger::instance().debug(L"capture",
                L"WGC item ready: " + std::to_wstring(width) + L"x" + std::to_wstring(height));

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
                                Logger::instance().warning(L"capture",
                                    L"Borderless capture access was not granted; Windows may show a capture border");
                            }
                        } catch (const winrt::hresult_error& error) {
                            Logger::instance().warning(L"capture",
                                L"Could not disable Windows Graphics Capture border: " +
                                std::wstring(error.message().c_str()));
                        }
                    });
            } catch (const winrt::hresult_error& error) {
                Logger::instance().warning(L"capture",
                    L"Could not request borderless Windows Graphics Capture: " +
                    std::wstring(error.message().c_str()));
            }
            Logger::instance().debug(L"capture", L"WGC StartCapture");
            session.StartCapture();
        } catch (const winrt::hresult_error& error) {
            Logger::instance().warning(L"capture", L"Windows Graphics Capture initialization failed: " + std::wstring(error.message().c_str()));
            return false;
        }

        Logger::instance().info(L"capture",
            L"Windows Graphics Capture initialized at " + std::to_wstring(width) + L"x" +
            std::to_wstring(height) + L", zeroCopy=" + (zeroCopy ? L"yes" : L"no"));
        return true;
    }

    bool acquire(GpuFrame& output, uint32_t timeoutMs) {
        if (!framePool || deviceLost.load()) {
            return false;
        }

        // A deferred pool recreate retries here first: Recreate can only make
        // progress once every frame of the old pool has been released.
        if (recreatePending) {
            completePendingPoolRecreate();
            if (recreatePending) {
                return false;
            }
        }

        // Wait for the next frame. When a high-res cadence timer is available,
        // wait on both it and frameEvent so sub-15ms timeouts are honored even if
        // the global timer period was not raised. Timer expiry == cadence timeout.
        if (cadenceTimer && timeoutMs > 0) {
            LARGE_INTEGER due;
            // Negative = relative, in 100ns units.
            due.QuadPart = -static_cast<int64_t>(timeoutMs) * 10'000LL;
            if (SetWaitableTimer(cadenceTimer, &due, 0, nullptr, nullptr, FALSE)) {
                const HANDLE handles[2] = {frameEvent, cadenceTimer};
                const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                CancelWaitableTimer(cadenceTimer);
                if (wait != WAIT_OBJECT_0) {
                    return false;
                }
            } else {
                const DWORD wait = WaitForSingleObject(frameEvent, timeoutMs);
                if (wait != WAIT_OBJECT_0) {
                    return false;
                }
            }
        } else {
            const DWORD wait = WaitForSingleObject(frameEvent, timeoutMs);
            if (wait != WAIT_OBJECT_0) {
                return false;
            }
        }

        try {
            ResetEvent(frameEvent);
            auto frame = framePool.TryGetNextFrame();
            if (!frame) {
                return false;
            }

            // WGC pools frames while capture/scaling is busy. Consume all ready
            // frames and retain newest one; encoding stale cursor positions only
            // adds latency and auto-reset events used to lose these wakeups.
            while (auto newerFrame = framePool.TryGetNextFrame()) {
                frame = std::move(newerFrame);
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
                // Recreate requires every frame from the old pool to be released.
                // Zero-copy frames still leased by the encoder would otherwise
                // stall the new pool; defer instead of recreating under them.
                frame = nullptr;
                pendingResize = size;
                recreatePending = true;
                recreateDeferrals = 0;
                resolutionChanges.fetch_add(1, std::memory_order_release);
                Logger::instance().info(L"capture",
                    L"WGC content resized in place; recreating frame pool at " +
                    std::to_wstring(width) + L"x" + std::to_wstring(height));
                completePendingPoolRecreate();
                return false;
            }

            // True presentation time (DWM vsync instant), not drain-time now().
            // SystemRelativeTime is QPC-based 100ns units -- same epoch as MSVC
            // steady_clock -- so it stays comparable with the emit-grid clock and
            // gives an evenly-spaced cadence. Drain-time now() carried callback +
            // scheduling jitter, which is what made the emit grid stutter. Zero
            // means the frame has no timestamp yet; fall back to now() for it.
            int64_t presentTime100ns = frame.SystemRelativeTime().count();
            if (presentTime100ns <= 0) {
                presentTime100ns = std::chrono::duration_cast<
                                       std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count();
            }

            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> capturedTexture;
            HRESULT hr = access->GetInterface(IID_PPV_ARGS(&capturedTexture));
            if (FAILED(hr)) {
                Logger::instance().warning(L"capture", L"WGC surface did not expose ID3D11Texture2D: " + hresultToString(hr));
                return false;
            }

            if (zeroCopy) {
                // Each zero-copy lease holds a Direct3D11CaptureFrame until encode
                // finishes; keep one WGC pool slot free so FrameArrived /
                // TryGetNextFrame keep working.
                const uint32_t outstanding =
                    outstandingZeroCopyLeases->load(std::memory_order_acquire);
                const uint32_t maxLeases = wgcMaxZeroCopyLeases(framePoolSize);
                if (outstanding >= maxLeases) {
                    if (gpuDropShouldLog(zeroCopyLeaseDrops)) {
                        Logger::instance().warning(L"capture",
                            L"WGC zero-copy lease limit reached; falling back to copy path count=" +
                            std::to_wstring(zeroCopyLeaseDrops) +
                            L" outstanding=" + std::to_wstring(outstanding) +
                            L" pool=" + std::to_wstring(framePoolSize));
                    }
                    // Force a copy so the Direct3D11CaptureFrame is released immediately.
                    // frame goes out of scope after this block → WGC pool slot freed.
                    return emitCopyFrame(output, capturedTexture.Get(), presentTime100ns);
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
                output.pts100ns = presentTime100ns;
                output.width = width;
                output.height = height;
                output.format = DXGI_FORMAT_B8G8R8A8_UNORM;
                // DWM pre-tonemaps WGC to SDR; scaler must not tone-map again.
                output.hdr = false;
                return true;
            }

            return emitCopyFrame(output, capturedTexture.Get(), presentTime100ns);
        } catch (const winrt::hresult_error& error) {
            Logger::instance().warning(L"capture", L"Windows Graphics Capture frame acquisition failed: " + std::wstring(error.message().c_str()));
            deviceLost.store(true);
            return false;
        }
    }

    void shutdown() {
        // Revoke callbacks first so free-threaded FrameArrived cannot re-enter
        // while session/framePool are closing (known WinRT hang class).
        try {
            if (framePool && frameArrivedToken.value) {
                framePool.FrameArrived(frameArrivedToken);
                frameArrivedToken = {};
            }
            if (item && closedToken.value) {
                item.Closed(closedToken);
                closedToken = {};
            }
        } catch (const winrt::hresult_error& error) {
            Logger::instance().warning(L"capture",
                L"WGC event revoke failed during shutdown: " + std::wstring(error.message().c_str()));
        }

        // Best-effort drain of zero-copy Direct3D11CaptureFrame leases. Closing
        // the session while frames are still leased can hang indefinitely.
        if (outstandingZeroCopyLeases) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (outstandingZeroCopyLeases->load(std::memory_order_acquire) > 0 &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            const uint32_t remaining =
                outstandingZeroCopyLeases->load(std::memory_order_acquire);
            if (remaining > 0) {
                Logger::instance().warning(L"capture",
                    L"WGC shutdown proceeding with outstanding zero-copy leases=" +
                    std::to_wstring(remaining));
            }
        }

        try {
            if (session) {
                session.Close();
                session = nullptr;
            }
        } catch (const winrt::hresult_error& error) {
            Logger::instance().warning(L"capture",
                L"WGC session.Close failed: " + std::wstring(error.message().c_str()));
            session = nullptr;
        }
        try {
            if (framePool) {
                framePool.Close();
                framePool = nullptr;
            }
        } catch (const winrt::hresult_error& error) {
            Logger::instance().warning(L"capture",
                L"WGC framePool.Close failed: " + std::wstring(error.message().c_str()));
            framePool = nullptr;
        }
        item = nullptr;
        winrtDevice = nullptr;
        recreatePending = false;
        pendingResize = {};
        pool.clear();
        {
            std::scoped_lock lock(frameEventMutex);
            if (frameEvent) {
                CloseHandle(frameEvent);
                frameEvent = nullptr;
            }
        }
        if (cadenceTimer) {
            CloseHandle(cadenceTimer);
            cadenceTimer = nullptr;
        }
        device = nullptr;
        deviceLost.store(true);
    }

    // Completes a deferred FramePool.Recreate. Old-pool frames may still be
    // leased to the encoder (zero-copy GpuFrames); calling Recreate while they
    // are in flight is documented to stall capture. Wait briefly for the
    // leases to drain; if the encoder has not released them in time, leave
    // recreatePending set and retry on a later acquire() call instead of
    // recreating under live frames.
    void completePendingPoolRecreate() {
        if (!recreatePending || !framePool) {
            recreatePending = false;
            return;
        }

        constexpr auto kLeaseDrainWait = std::chrono::milliseconds(150);
        const auto deadline = std::chrono::steady_clock::now() + kLeaseDrainWait;
        while (outstandingZeroCopyLeases->load(std::memory_order_acquire) > 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                ++recreateDeferrals;
                if (recreateDeferrals == 1 || (recreateDeferrals % 50) == 0) {
                    Logger::instance().warning(L"capture",
                        L"WGC frame pool recreate deferred; encoder still holds " +
                        std::to_wstring(outstandingZeroCopyLeases->load(std::memory_order_acquire)) +
                        L" zero-copy frame(s), deferral=" + std::to_wstring(recreateDeferrals));
                }
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        recreateDeferrals = 0;
        try {
            framePool.Recreate(
                winrtDevice,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                static_cast<int32_t>(framePoolSize),
                pendingResize);
        } catch (const winrt::hresult_error& error) {
            Logger::instance().warning(L"capture",
                L"WGC frame pool recreate failed: " + std::wstring(error.message().c_str()));
            deviceLost.store(true);
        }
        recreatePending = false;
    }

    void signalFrameEvent() {
        std::scoped_lock lock(frameEventMutex);
        if (frameEvent) {
            SetEvent(frameEvent);
        }
    }

    // Applies a cursor-capture preference change to the live session so a
    // settings toggle takes effect immediately instead of waiting for the
    // next backend rebind.
    void setCursorCaptureEnabled(bool enabled) {
        if (captureCursor == enabled) {
            return;
        }
        captureCursor = enabled;
        try {
            if (session) {
                session.IsCursorCaptureEnabled(enabled);
            }
        } catch (const winrt::hresult_error& error) {
            Logger::instance().warning(L"capture",
                L"Could not update WGC cursor capture on the live session: " +
                std::wstring(error.message().c_str()));
        }
    }

    // Shared copy path for the no-zero-copy mode and the zero-copy lease-cap
    // fallback: copies the captured surface into a pooled texture so the
    // Direct3D11CaptureFrame is released on return.
    bool emitCopyFrame(GpuFrame& output, ID3D11Texture2D* captured, int64_t presentTime100ns) {
        // Lazy for the lease-cap fallback, which never built the pool at init.
        if (pool.empty() && !createTexturePool(width, height)) {
            return false;
        }
        auto slot = pool.acquireOrLog(L"WGC capture texture pool exhausted");
        if (!slot) {
            return false;
        }
        {
            std::scoped_lock lock(device->immediateContextMutex());
            device->context()->CopyResource(slot->texture.Get(), captured);
        }
        output.texture = slot->texture;
        output.lease = slot;
        output.frameIndex = frameIndex++;
        output.pts100ns = presentTime100ns;
        output.width = width;
        output.height = height;
        output.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        // DWM pre-tonemaps WGC to SDR; scaler must not tone-map again.
        output.hdr = false;
        return true;
    }

    bool createTexturePool(uint32_t newWidth, uint32_t newHeight) {
        pool.clear();

        // RENDER_TARGET is required by some NVIDIA drivers before NVENC accepts
        // the texture as a registered input (NV_ENC_ERR_RESOURCE_REGISTER_FAILED
        // otherwise); SHADER_RESOURCE keeps the downstream scaler happy. This
        // mirrors the other capture pools.
        GpuPoolBuildOptions options;
        options.wantRtv = true;
        options.wantSrv = true;
        std::vector<ComPtr<ID3D11Texture2D>> textures;
        if (!GpuPoolBuilder::build(
                device->device(), newWidth, newHeight,
                DXGI_FORMAT_B8G8R8A8_UNORM, options, texturePoolSize,
                textures, L"WGC pool")) {
            return false;
        }
        for (auto& texture : textures) {
            auto slot = std::make_shared<TextureSlot>();
            slot->texture = std::move(texture);
            pool.push_back(std::move(slot));
        }
        pool.setFootprintBytes(
            dxgiTextureBytes(newWidth, newHeight, DXGI_FORMAT_B8G8R8A8_UNORM) * pool.size());
        Logger::instance().debug(L"capture",
            L"WGC texture pool initialized " +
            std::to_wstring(newWidth) + L"x" + std::to_wstring(newHeight) +
            L" pool=" + std::to_wstring(pool.size()) +
            L" (" + std::to_wstring(pool.footprintBytes() / 1024) + L" KiB)");
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

uint64_t WgcCaptureSource::resolutionChangeCount() const {
    return impl_->resolutionChanges.load(std::memory_order_acquire);
}

void WgcCaptureSource::setCursorCaptureEnabled(bool enabled) {
    impl_->setCursorCaptureEnabled(enabled);
}

} // namespace backtrack
