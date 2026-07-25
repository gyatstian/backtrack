// BacktrackGameCapture.dll — DXGI Present vtable hook for exclusive game capture.

#include <Windows.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>

#include <detours.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "capture/game/GameCaptureGl.h"
#include "capture/game/GameCaptureProtocol.h"

using namespace backtrack::gamecap;

#ifndef D3D11_RESOURCE_MISC_SHARED_NTHANDLE
#define D3D11_RESOURCE_MISC_SHARED_NTHANDLE 0x800L
#endif

namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn =
    HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain*,
    UINT,
    UINT,
    UINT,
    DXGI_FORMAT,
    UINT);
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory*,
    IUnknown*,
    DXGI_SWAP_CHAIN_DESC*,
    IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*,
    IUnknown*,
    HWND,
    const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
    IDXGIOutput*,
    IDXGISwapChain1**);
using CreateSwapChainForCoreWindowFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using CreateSwapChainForCompositionFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);

PresentFn g_originalPresent = nullptr;
Present1Fn g_originalPresent1 = nullptr;
ResizeBuffersFn g_originalResizeBuffers = nullptr;
CreateSwapChainFn g_originalCreateSwapChain = nullptr;
CreateSwapChainForHwndFn g_originalCreateSwapChainForHwnd = nullptr;
CreateSwapChainForCoreWindowFn g_originalCreateSwapChainForCoreWindow = nullptr;
CreateSwapChainForCompositionFn g_originalCreateSwapChainForComposition = nullptr;

std::atomic<bool> g_active{false};
std::atomic<bool> g_started{false};
std::atomic_flag g_starting = ATOMIC_FLAG_INIT;
std::atomic<bool> g_resizeInFlight{false};

HANDLE g_mapping = nullptr;
GameCaptureSharedHeader* g_header = nullptr;
HANDLE g_event = nullptr;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11Texture2D* g_sharedTex[kGameCaptureMaxSharedTextures] = {};
IDXGIKeyedMutex* g_sharedMutex[kGameCaptureMaxSharedTextures] = {};
HANDLE g_sharedHandle[kGameCaptureMaxSharedTextures] = {};
bool g_sharedIsNtHandle[kGameCaptureMaxSharedTextures] = {};
ID3D11Texture2D* g_stagingTex = nullptr;
ID3D11Texture2D* g_resolveTex = nullptr;
HANDLE g_dataMapping = nullptr;
uint8_t* g_dataView = nullptr;
uint32_t g_dataMappingBytes = 0;
uint32_t g_dataGeneration = 0;
uint32_t g_nextDataBuffer = 0;
bool g_forceShmem = false;
uint32_t g_nextSharedSlot = 0;
uint64_t g_nextCaptureQpc = 0;
uint32_t g_texW = 0;
uint32_t g_texH = 0;
DXGI_FORMAT g_texFormat = DXGI_FORMAT_UNKNOWN;

ID3D12CommandQueue* g_d3d12Queue = nullptr;
ID3D12Device* g_d3d12Device = nullptr;
ID3D11Device* g_d3d11on12Device = nullptr;
ID3D11DeviceContext* g_d3d11on12Context = nullptr;
ID3D11On12Device* g_on12 = nullptr;
ID3D11Resource* g_wrappedBackBuffer = nullptr;
IDXGISwapChain* g_wrappedSwap = nullptr;
UINT g_wrappedBufferIndex = UINT_MAX;

// Detours rewrites the target function bodies in place, so a single attach per
// method covers every swapchain that shares the same vtable implementation.
// These flags track which trampolines are live so cleanup() can detach exactly
// what was attached.
bool g_presentAttached = false;
bool g_present1Attached = false;
bool g_resizeBuffersAttached = false;
bool g_createSwapChainAttached = false;
bool g_createSwapChainForHwndAttached = false;
bool g_createSwapChainForCoreWindowAttached = false;
bool g_createSwapChainForCompositionAttached = false;

HWND g_probeHwnd = nullptr;

void setHookError(const wchar_t* text, uint32_t code = 0) {
    if (!g_header) {
        return;
    }
    g_header->errorCode = code;
    g_header->state = GameCaptureSlotState::Error;
    if (text) {
        wcsncpy_s(g_header->errorText, text, _TRUNCATE);
    }
}

void setHookInfo(const wchar_t* text) {
    if (!g_header || !text) {
        return;
    }
    wcsncpy_s(g_header->errorText, text, _TRUNCATE);
}

void noteException(uint32_t code) {
    if (!g_header) {
        return;
    }
    g_header->lastExceptionCode = code;
    g_header->flags |= kGameCapFlagPublishException;
    setHookInfo(L"SEH in Present publish (suppressed)");
}

uint64_t qpcNow() {
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    return static_cast<uint64_t>(qpc.QuadPart);
}

void setHookInstallTiming(uint64_t startQpc, DWORD startTick) {
    if (!g_header) {
        return;
    }
    g_header->hookInstallMs = GetTickCount() - startTick;
    InterlockedExchange64(&g_header->hookInstallQpc, static_cast<LONGLONG>(qpcNow() - startQpc));
}

void releaseShared() {
    for (uint32_t i = 0; i < kGameCaptureMaxSharedTextures; ++i) {
        if (g_sharedMutex[i]) { g_sharedMutex[i]->Release(); g_sharedMutex[i] = nullptr; }
        if (g_sharedTex[i]) { g_sharedTex[i]->Release(); g_sharedTex[i] = nullptr; }
        if (g_sharedIsNtHandle[i] && g_sharedHandle[i]) CloseHandle(g_sharedHandle[i]);
        g_sharedHandle[i] = nullptr;
        g_sharedIsNtHandle[i] = false;
    }
    g_nextSharedSlot = 0;
    g_nextCaptureQpc = 0;
    g_texW = 0;
    g_texH = 0;
    g_texFormat = DXGI_FORMAT_UNKNOWN;
}

// GPU-side staging resources for the CPU-copy transport. Owned by ensureStaging.
void releaseStaging() {
    if (g_resolveTex) {
        g_resolveTex->Release();
        g_resolveTex = nullptr;
    }
    if (g_stagingTex) {
        g_stagingTex->Release();
        g_stagingTex = nullptr;
    }
}

// Shared pixel-data section for the CPU-copy transport. Owned by ensureDataMapping.
void releaseDataMapping() {
    if (g_dataView) {
        UnmapViewOfFile(g_dataView);
        g_dataView = nullptr;
    }
    if (g_dataMapping) {
        CloseHandle(g_dataMapping);
        g_dataMapping = nullptr;
    }
    g_dataMappingBytes = 0;
    g_dataGeneration = 0;
    g_nextDataBuffer = 0;
}

void releaseShmem() {
    releaseStaging();
    releaseDataMapping();
}

void releaseD3d12Bridge() {
    if (g_wrappedBackBuffer) { g_wrappedBackBuffer->Release(); g_wrappedBackBuffer = nullptr; }
    if (g_wrappedSwap) { g_wrappedSwap->Release(); g_wrappedSwap = nullptr; }
    g_wrappedBufferIndex = UINT_MAX;
    if (g_on12) { g_on12->Release(); g_on12 = nullptr; }
    if (g_d3d11on12Context) { g_d3d11on12Context->Release(); g_d3d11on12Context = nullptr; }
    if (g_d3d11on12Device) { g_d3d11on12Device->Release(); g_d3d11on12Device = nullptr; }
    if (g_d3d12Device) { g_d3d12Device->Release(); g_d3d12Device = nullptr; }
    if (g_d3d12Queue) { g_d3d12Queue->Release(); g_d3d12Queue = nullptr; }
}

bool isTargetSwap(IDXGISwapChain* swap) {
    if (!g_header || g_header->targetWindow == 0) return true;
    HWND hwnd = nullptr;
    IDXGISwapChain1* swap1 = nullptr;
    if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&swap1))) && swap1) {
        swap1->GetHwnd(&hwnd);
        swap1->Release();
    }
    if (!hwnd) {
        DXGI_SWAP_CHAIN_DESC desc{};
        if (SUCCEEDED(swap->GetDesc(&desc))) hwnd = desc.OutputWindow;
    }
    return hwnd && reinterpret_cast<uint64_t>(hwnd) == g_header->targetWindow;
}

void rememberD3d12Queue(IUnknown* deviceOrQueue) {
    if (!deviceOrQueue || g_d3d12Queue) return;
    ID3D12CommandQueue* queue = nullptr;
    if (SUCCEEDED(deviceOrQueue->QueryInterface(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue))) && queue) {
        ID3D12Device* device = nullptr;
        if (SUCCEEDED(queue->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) && device) {
            g_d3d12Queue = queue;
            g_d3d12Device = device;
        } else {
            queue->Release();
        }
    }
}

bool ensureD3d12Bridge() {
    if (g_on12) return true;
    if (!g_d3d12Device || !g_d3d12Queue) return false;
    IUnknown* queues[] = {g_d3d12Queue};
    HRESULT hr = D3D11On12CreateDevice(g_d3d12Device, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, queues, 1, 0,
        &g_d3d11on12Device, &g_d3d11on12Context, nullptr);
    if (FAILED(hr) || !g_d3d11on12Device ||
        FAILED(g_d3d11on12Device->QueryInterface(__uuidof(ID3D11On12Device), reinterpret_cast<void**>(&g_on12)))) {
        setHookInfo(L"D3D11On12 bridge create failed");
        return false;
    }
    return true;
}

DXGI_FORMAT normalizeFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_UNKNOWN:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return format;
    }
}

bool ensureShared(ID3D11Device* device, uint32_t w, uint32_t h, DXGI_FORMAT format) {
    format = normalizeFormat(format);
    if (g_sharedTex[0] && g_texW == w && g_texH == h && g_texFormat == format) {
        return true;
    }
    releaseShared();
    if (g_header) {
        InterlockedIncrement(&g_header->sharedTextureRecreateCount);
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    // Producer owns key 0 and host owns key 1. This prevents the host from
    // copying a shared texture while Present is writing its next frame.
    // Prefer NT handles. Legacy shared handles remain fallback for old drivers.
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    bool useNtHandles = true;
    for (uint32_t i = 0; i < kGameCaptureMaxSharedTextures; ++i) {
        ID3D11Texture2D* tex = nullptr;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &tex);
        if (FAILED(hr) || !tex) {
            if (!useNtHandles) { setHookError(L"CreateTexture2D shared failed", static_cast<uint32_t>(hr)); return false; }
            releaseShared();
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            useNtHandles = false;
            i = UINT32_MAX;
            continue;
        }
        HANDLE handle = nullptr;
        if (useNtHandles) {
            IDXGIResource1* resource1 = nullptr;
            hr = tex->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&resource1));
            if (SUCCEEDED(hr) && resource1) {
                hr = resource1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
                resource1->Release();
            }
        } else {
            IDXGIResource* resource = nullptr;
            hr = tex->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&resource));
            if (SUCCEEDED(hr) && resource) { hr = resource->GetSharedHandle(&handle); resource->Release(); }
        }
        IDXGIKeyedMutex* mutex = nullptr;
        if (SUCCEEDED(hr) && handle) hr = tex->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&mutex));
        if (FAILED(hr) || !handle || !mutex) {
            if (mutex) mutex->Release();
            if (useNtHandles && handle) CloseHandle(handle);
            tex->Release();
            if (useNtHandles) { releaseShared(); desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX; useNtHandles = false; i = UINT32_MAX; continue; }
            setHookError(L"Create shared texture handle failed", static_cast<uint32_t>(hr)); return false;
        }
        g_sharedTex[i] = tex;
        g_sharedMutex[i] = mutex;
        g_sharedHandle[i] = handle;
        g_sharedIsNtHandle[i] = useNtHandles;
    }
    g_texW = w;
    g_texH = h;
    g_texFormat = format;
    if (g_header) {
        if (useNtHandles) {
            g_header->flags |= kGameCapFlagNtHandle;
        } else {
            g_header->flags &= ~kGameCapFlagNtHandle;
        }
    }
    return true;
}

bool ensureStaging(ID3D11Device* device, uint32_t w, uint32_t h, DXGI_FORMAT format) {
    if (!device) {
        return false;
    }
    if (g_stagingTex) {
        D3D11_TEXTURE2D_DESC existing{};
        g_stagingTex->GetDesc(&existing);
        if (existing.Width == w && existing.Height == h && existing.Format == format) {
            return true;
        }
    }
    releaseStaging();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    const HRESULT hr = device->CreateTexture2D(&desc, nullptr, &g_stagingTex);
    if (FAILED(hr) || !g_stagingTex) {
        setHookError(L"CreateTexture2D shmem staging failed", static_cast<uint32_t>(hr));
        return false;
    }
    return true;
}

bool ensureDataMapping(uint32_t stride, uint32_t height, uint32_t width, DXGI_FORMAT format) {
    if (!g_header || stride == 0 || height == 0 || width == 0) {
        return false;
    }
    const uint64_t bufferBytes64 = static_cast<uint64_t>(stride) * height;
    const uint64_t mappingBytes64 = bufferBytes64 * kGameCaptureMaxDataBuffers;
    if (bufferBytes64 > UINT32_MAX || mappingBytes64 > UINT32_MAX) {
        setHookError(L"Game capture shmem frame is too large");
        return false;
    }
    const uint32_t bufferBytes = static_cast<uint32_t>(bufferBytes64);
    const uint32_t mappingBytes = static_cast<uint32_t>(mappingBytes64);
    if (g_dataView && g_dataMappingBytes == mappingBytes &&
        g_header->dataStride == stride && g_header->dataWidth == width &&
        g_header->dataHeight == height && g_header->dataFormat == static_cast<uint32_t>(format)) {
        return true;
    }

    releaseDataMapping();
    const uint32_t generation = static_cast<uint32_t>(
        InterlockedCompareExchange(&g_header->dataMappingGeneration, 0, 0)) + 1;
    wchar_t name[64] = {};
    gameCaptureDataMappingName(name, generation);
    g_dataMapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, mappingBytes, name);
    if (!g_dataMapping || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_dataMapping) {
            CloseHandle(g_dataMapping);
            g_dataMapping = nullptr;
        }
        setHookError(L"CreateFileMapping for game capture shmem failed", GetLastError());
        return false;
    }
    g_dataView = static_cast<uint8_t*>(
        MapViewOfFile(g_dataMapping, FILE_MAP_ALL_ACCESS, 0, 0, mappingBytes));
    if (!g_dataView) {
        setHookError(L"MapViewOfFile for game capture shmem failed", GetLastError());
        CloseHandle(g_dataMapping);
        g_dataMapping = nullptr;
        return false;
    }
    g_dataMappingBytes = mappingBytes;
    g_dataGeneration = generation;
    g_nextDataBuffer = 0;

    // Publish layout before generation. A reader which observes this generation
    // can safely open its generation-suffixed section and use the layout.
    g_header->dataBufferCount = kGameCaptureMaxDataBuffers;
    g_header->dataStride = stride;
    g_header->dataBufferBytes = bufferBytes;
    g_header->dataMappingBytes = mappingBytes;
    g_header->dataWidth = width;
    g_header->dataHeight = height;
    g_header->dataFormat = static_cast<uint32_t>(format);
    InterlockedExchange(&g_header->latestDataBuffer, 0);
    InterlockedExchange(&g_header->dataMappingGeneration, static_cast<long>(generation));
    return true;
}

// Publishes the backbuffer to a shared GPU texture (zero-copy transport).
// Returns false only when shared-texture transport is unavailable in this
// process, signalling the caller to fall back to the CPU-copy path. A transient
// keyed-mutex contention drop still returns true: the transport works, this one
// frame is simply skipped. Never releases backBuffer; the caller owns it.
bool publishFrameShared(ID3D11Texture2D* backBuffer, const D3D11_TEXTURE2D_DESC& bbDesc,
                        DXGI_FORMAT format, bool wrapped) {
    if (!ensureShared(g_device, bbDesc.Width, bbDesc.Height, format)) {
        return false;
    }

    // Never block game's Present. Host drops frame if it cannot copy before
    // next Present; producer drops instead of stalling render thread.
    uint32_t sharedSlot = g_nextSharedSlot++ % kGameCaptureMaxSharedTextures;
    for (uint32_t i = 0; i < kGameCaptureMaxSharedTextures; ++i) {
        const uint32_t candidate = (sharedSlot + i) % kGameCaptureMaxSharedTextures;
        // WAIT_TIMEOUT is a non-failing HRESULT. It still means host owns this
        // slot, so only S_OK grants producer ownership.
        if (g_sharedMutex[candidate]->AcquireSync(0, 0) == S_OK) { sharedSlot = candidate; break; }
        InterlockedIncrement(&g_header->producerKeyedMutexMisses);
        if (i + 1 == kGameCaptureMaxSharedTextures) {
            InterlockedIncrement(&g_header->droppedByContention);
            return true;
        }
    }
    if (!g_sharedMutex[sharedSlot]) {
        return true;
    }
    const uint64_t copyStartQpc = qpcNow();
    if (wrapped) {
        ID3D11Resource* resource = g_wrappedBackBuffer;
        g_on12->AcquireWrappedResources(&resource, 1);
    }
    if (bbDesc.SampleDesc.Count > 1) {
        g_context->ResolveSubresource(g_sharedTex[sharedSlot], 0, backBuffer, 0, format);
    } else {
        g_context->CopyResource(g_sharedTex[sharedSlot], backBuffer);
    }
    if (wrapped) {
        ID3D11Resource* resource = g_wrappedBackBuffer;
        g_on12->ReleaseWrappedResources(&resource, 1);
        // D3D11On12 batches barriers/copies until Flush submits them to queue.
        g_context->Flush();
    }
    g_sharedMutex[sharedSlot]->ReleaseSync(1);
    InterlockedAdd64(&g_header->producerCopyQpc, static_cast<LONGLONG>(qpcNow() - copyStartQpc));

    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);

    InterlockedIncrement(&g_header->publishEpoch);
    g_header->transportMode = static_cast<uint32_t>(GameCaptureTransport::SharedTexture);
    auto& descriptor = g_header->frames[sharedSlot];
    descriptor.width = bbDesc.Width;
    descriptor.height = bbDesc.Height;
    descriptor.format = static_cast<uint32_t>(format);
    descriptor.sharedHandle = reinterpret_cast<uint64_t>(g_sharedHandle[sharedSlot]);
    descriptor.flags = g_sharedIsNtHandle[sharedSlot] ? kGameCapFlagNtHandle : 0;
    descriptor.presentQpc = qpc.QuadPart;
    g_header->latestReadySlot = sharedSlot;
    g_header->width = bbDesc.Width;
    g_header->height = bbDesc.Height;
    g_header->format = static_cast<uint32_t>(format);
    g_header->sharedHandle = descriptor.sharedHandle;
    g_header->lastPresentQpc = qpc.QuadPart;
    g_header->framesPublished += 1;
    g_header->sequence += 1;
    g_header->state = GameCaptureSlotState::Active;
    g_header->errorCode = 0;
    g_header->graphicsApi = static_cast<uint32_t>(wrapped ? GameCaptureGraphicsApi::D3D12 : GameCaptureGraphicsApi::D3D11);
    g_header->errorText[0] = L'\0';
    InterlockedIncrement(&g_header->publishEpoch);

    if (g_event) {
        SetEvent(g_event);
    }
    return true;
}

// CPU-copy ("shmem") transport fallback. Resolves/copies the backbuffer into a
// CPU-readable staging texture, maps it, and memcpys the row-padded pixels into
// the next rotated buffer of the shared data section. The map blocks until the
// GPU copy completes; the CPU path is inherently synchronous and is only used
// when the zero-copy path is unavailable. Never releases backBuffer; the caller
// owns it.
void publishFrameShmem(ID3D11Texture2D* backBuffer, const D3D11_TEXTURE2D_DESC& bbDesc,
                       DXGI_FORMAT format, bool wrapped) {
    if (!ensureStaging(g_device, bbDesc.Width, bbDesc.Height, format)) {
        return;
    }

    const uint64_t copyStartQpc = qpcNow();
    if (wrapped) {
        ID3D11Resource* resource = g_wrappedBackBuffer;
        g_on12->AcquireWrappedResources(&resource, 1);
    }
    if (bbDesc.SampleDesc.Count > 1) {
        g_context->ResolveSubresource(g_stagingTex, 0, backBuffer, 0, format);
    } else {
        g_context->CopyResource(g_stagingTex, backBuffer);
    }
    if (wrapped) {
        ID3D11Resource* resource = g_wrappedBackBuffer;
        g_on12->ReleaseWrappedResources(&resource, 1);
        // D3D11On12 batches barriers/copies until Flush submits them to queue.
        g_context->Flush();
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = g_context->Map(g_stagingTex, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        setHookError(L"Map staging texture for shmem failed", static_cast<uint32_t>(hr));
        return;
    }
    const uint32_t stride = mapped.RowPitch;
    if (!ensureDataMapping(stride, bbDesc.Height, bbDesc.Width, format)) {
        g_context->Unmap(g_stagingTex, 0);
        return;
    }

    // Rotate to the next data buffer so the host can keep reading the previous
    // one until it observes the new latestDataBuffer under the seqlock.
    const uint32_t buffer = g_nextDataBuffer;
    g_nextDataBuffer = (g_nextDataBuffer + 1) % kGameCaptureMaxDataBuffers;
    uint8_t* dst = g_dataView + static_cast<size_t>(buffer) * g_header->dataBufferBytes;
    memcpy(dst, mapped.pData, g_header->dataBufferBytes);
    g_context->Unmap(g_stagingTex, 0);
    InterlockedAdd64(&g_header->producerCopyQpc, static_cast<LONGLONG>(qpcNow() - copyStartQpc));

    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);

    InterlockedIncrement(&g_header->publishEpoch);
    g_header->transportMode = static_cast<uint32_t>(GameCaptureTransport::SharedMemory);
    InterlockedExchange(&g_header->latestDataBuffer, static_cast<long>(buffer));
    g_header->width = bbDesc.Width;
    g_header->height = bbDesc.Height;
    g_header->format = static_cast<uint32_t>(format);
    g_header->lastPresentQpc = qpc.QuadPart;
    g_header->framesPublished += 1;
    g_header->sequence += 1;
    g_header->state = GameCaptureSlotState::Active;
    g_header->errorCode = 0;
    g_header->graphicsApi = static_cast<uint32_t>(wrapped ? GameCaptureGraphicsApi::D3D12 : GameCaptureGraphicsApi::D3D11);
    g_header->errorText[0] = L'\0';
    InterlockedIncrement(&g_header->publishEpoch);

    if (g_event) {
        SetEvent(g_event);
    }
}

void publishFrame(IDXGISwapChain* swap) {
    if (!g_header || !g_active.load(std::memory_order_relaxed)) {
        return;
    }
    if (g_resizeInFlight.load(std::memory_order_relaxed)) {
        return;
    }
    if (!isTargetSwap(swap)) {
        return;
    }
    if (g_header->state == GameCaptureSlotState::Stopping) {
        g_active.store(false, std::memory_order_relaxed);
        return;
    }

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const uint32_t fps = g_header->captureFps ? g_header->captureFps : 60;
    if (g_nextCaptureQpc && static_cast<uint64_t>(now.QuadPart) < g_nextCaptureQpc) {
        InterlockedIncrement(&g_header->droppedByCadence);
        return;
    }
    g_nextCaptureQpc = static_cast<uint64_t>(now.QuadPart) + static_cast<uint64_t>(frequency.QuadPart) / fps;

    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    bool wrapped = false;
    if (FAILED(hr) || !backBuffer) {
        rememberD3d12Queue(nullptr);
        if (!ensureD3d12Bridge()) {
            setHookInfo(L"D3D12 swapchain found; waiting for command queue");
            return;
        }
        IDXGISwapChain3* swap3 = nullptr;
        UINT index = 0;
        if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&swap3))) && swap3) {
            index = swap3->GetCurrentBackBufferIndex();
            swap3->Release();
        }
        if (g_wrappedSwap != swap || g_wrappedBufferIndex != index) {
            if (g_wrappedBackBuffer) { g_wrappedBackBuffer->Release(); g_wrappedBackBuffer = nullptr; }
            if (g_wrappedSwap) { g_wrappedSwap->Release(); g_wrappedSwap = nullptr; }
            ID3D12Resource* resource = nullptr;
            hr = swap->GetBuffer(index, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&resource));
            if (SUCCEEDED(hr) && resource) {
                D3D11_RESOURCE_FLAGS flags{};
                hr = g_on12->CreateWrappedResource(resource, &flags, D3D12_RESOURCE_STATE_PRESENT,
                    D3D12_RESOURCE_STATE_PRESENT, __uuidof(ID3D11Resource), reinterpret_cast<void**>(&g_wrappedBackBuffer));
                resource->Release();
            }
            if (FAILED(hr) || !g_wrappedBackBuffer) return;
            swap->AddRef();
            g_wrappedSwap = swap;
            g_wrappedBufferIndex = index;
        }
        hr = g_wrappedBackBuffer->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
        if (FAILED(hr) || !backBuffer) return;
        wrapped = true;
    }

    D3D11_TEXTURE2D_DESC bbDesc{};
    backBuffer->GetDesc(&bbDesc);

    ID3D11Device* device = nullptr;
    backBuffer->GetDevice(&device);
    if (!device) {
        backBuffer->Release();
        return;
    }

    if (g_device != device) {
        if (g_context) {
            g_context->Release();
            g_context = nullptr;
        }
        if (g_device) {
            g_device->Release();
        }
        g_device = device;
        g_device->AddRef();
        g_device->GetImmediateContext(&g_context);
        releaseShared();
    }
    device->Release();

    if (!g_context) {
        backBuffer->Release();
        return;
    }

    const DXGI_FORMAT format = normalizeFormat(bbDesc.Format);
    // Prefer zero-copy shared-texture transport. If it is unavailable in this
    // process (shared NT-handle creation unsupported), latch g_forceShmem and
    // fall back to the CPU-copy path for the rest of the session.
    bool published = false;
    if (!g_forceShmem) {
        if (publishFrameShared(backBuffer, bbDesc, format, wrapped)) {
            published = true;
        } else {
            g_forceShmem = true;
        }
    }
    if (!published) {
        publishFrameShmem(backBuffer, bbDesc, format, wrapped);
    }
    backBuffer->Release();
}

// SEH wrapper: no C++ objects with destructors in this function.
void publishFrameSeh(IDXGISwapChain* swap) {
    __try {
        publishFrame(swap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        noteException(GetExceptionCode());
    }
}

bool shouldPublish(UINT flags) {
    if (flags & DXGI_PRESENT_TEST) {
        return false;
    }
    return g_active.load(std::memory_order_relaxed);
}

HRESULT STDMETHODCALLTYPE hookedPresent(IDXGISwapChain* swap, UINT sync, UINT flags) {
    if (!swap || !g_originalPresent) {
        return DXGI_ERROR_INVALID_CALL;
    }
    const HRESULT hr = g_originalPresent(swap, sync, flags);
    if (SUCCEEDED(hr) && shouldPublish(flags)) {
        publishFrameSeh(swap);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hookedPresent1(
    IDXGISwapChain1* swap,
    UINT sync,
    UINT flags,
    const DXGI_PRESENT_PARAMETERS* params) {
    HRESULT hr = DXGI_ERROR_INVALID_CALL;
    if (swap && g_originalPresent1) {
        hr = g_originalPresent1(swap, sync, flags, params);
    } else if (swap && g_originalPresent) {
        hr = g_originalPresent(swap, sync, flags);
    } else {
        return DXGI_ERROR_INVALID_CALL;
    }
    if (SUCCEEDED(hr) && shouldPublish(flags)) {
        publishFrameSeh(swap);
    }
    return hr;
}

void releaseSharedSeh() {
    __try {
        releaseShared();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        for (uint32_t i = 0; i < kGameCaptureMaxSharedTextures; ++i) {
            g_sharedTex[i] = nullptr;
            g_sharedMutex[i] = nullptr;
            g_sharedHandle[i] = nullptr;
            g_sharedIsNtHandle[i] = false;
        }
        g_texW = 0;
        g_texH = 0;
        g_texFormat = DXGI_FORMAT_UNKNOWN;
        noteException(GetExceptionCode());
    }
}

HRESULT STDMETHODCALLTYPE hookedResizeBuffers(
    IDXGISwapChain* swap,
    UINT bufferCount,
    UINT width,
    UINT height,
    DXGI_FORMAT newFormat,
    UINT swapChainFlags) {
    g_resizeInFlight.store(true, std::memory_order_release);
    if (g_header) {
        InterlockedIncrement(&g_header->resizeCount);
    }
    releaseSharedSeh();
    HRESULT hr = DXGI_ERROR_INVALID_CALL;
    if (swap && g_originalResizeBuffers) {
        hr = g_originalResizeBuffers(swap, bufferCount, width, height, newFormat, swapChainFlags);
    }
    g_resizeInFlight.store(false, std::memory_order_release);
    return hr;
}

// Detours trampoline attach. *original must already hold the real target
// function address; on success Detours rewrites it to point at the trampoline
// so the hook can call through to the original.
bool detourAttach(void** original, void* detour) {
    if (!original || !*original || !detour) {
        return false;
    }
    if (DetourTransactionBegin() != NO_ERROR) {
        return false;
    }
    DetourUpdateThread(GetCurrentThread());
    if (DetourAttach(original, detour) != NO_ERROR) {
        DetourTransactionAbort();
        return false;
    }
    return DetourTransactionCommit() == NO_ERROR;
}

// Detours trampoline detach. *original must hold the trampoline pointer set by
// detourAttach; on success Detours restores the original function address.
bool detourDetach(void** original, void* detour) {
    if (!original || !*original || !detour) {
        return false;
    }
    if (DetourTransactionBegin() != NO_ERROR) {
        return false;
    }
    DetourUpdateThread(GetCurrentThread());
    if (DetourDetach(original, detour) != NO_ERROR) {
        DetourTransactionAbort();
        return false;
    }
    return DetourTransactionCommit() == NO_ERROR;
}

void destroyProbeWindow() {
    if (g_probeHwnd) {
        DestroyWindow(g_probeHwnd);
        g_probeHwnd = nullptr;
    }
}

HWND createProbeWindow() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BacktrackGameCapProbe";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        wc.lpszClassName,
        L"",
        WS_POPUP,
        0,
        0,
        2,
        2,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr);
    return hwnd;
}

void detectGraphicsApi() {
    if (!g_header) {
        return;
    }
    if (GetModuleHandleW(L"d3d12.dll")) {
        g_header->graphicsApi = static_cast<uint32_t>(GameCaptureGraphicsApi::D3D12);
    } else if (GetModuleHandleW(L"d3d11.dll")) {
        g_header->graphicsApi = static_cast<uint32_t>(GameCaptureGraphicsApi::D3D11);
    } else if (GetModuleHandleW(L"opengl32.dll")) {
        g_header->graphicsApi = static_cast<uint32_t>(GameCaptureGraphicsApi::OpenGL);
    } else if (GetModuleHandleW(L"vulkan-1.dll")) {
        g_header->graphicsApi = static_cast<uint32_t>(GameCaptureGraphicsApi::Vulkan);
    } else if (GetModuleHandleW(L"d3d9.dll")) {
        g_header->graphicsApi = static_cast<uint32_t>(GameCaptureGraphicsApi::D3D9);
    }
}

bool installPresentHooksFromSwap(IDXGISwapChain* swap) {
    if (!swap) {
        return false;
    }
    // Detours patches the shared function body, so a single attach covers every
    // swapchain that shares the same vtable. Skip once Present is attached.
    if (g_presentAttached) {
        return true;
    }
    void** vtable = *reinterpret_cast<void***>(swap);

    // Read the real functions out of the vtable so Detours has the target
    // addresses; DetourAttach rewrites these globals into trampolines.
    g_originalPresent = reinterpret_cast<PresentFn>(vtable[8]); // Present
    if (!detourAttach(
            reinterpret_cast<void**>(&g_originalPresent),
            reinterpret_cast<void*>(&hookedPresent))) {
        g_originalPresent = nullptr;
        setHookError(L"Present detour attach failed");
        return false;
    }
    g_presentAttached = true;

    g_originalResizeBuffers = reinterpret_cast<ResizeBuffersFn>(vtable[13]); // ResizeBuffers
    if (detourAttach(
            reinterpret_cast<void**>(&g_originalResizeBuffers),
            reinterpret_cast<void*>(&hookedResizeBuffers))) {
        g_resizeBuffersAttached = true;
    } else {
        g_originalResizeBuffers = nullptr;
    }

    IDXGISwapChain1* swap1 = nullptr;
    if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&swap1))) &&
        swap1) {
        void** v1 = *reinterpret_cast<void***>(swap1);
        g_originalPresent1 = reinterpret_cast<Present1Fn>(v1[22]); // Present1
        if (detourAttach(
                reinterpret_cast<void**>(&g_originalPresent1),
                reinterpret_cast<void*>(&hookedPresent1))) {
            g_present1Attached = true;
        } else {
            g_originalPresent1 = nullptr;
        }
        swap1->Release();
    }
    if (g_header) {
        g_header->flags |= kGameCapFlagPresentHooked;
    }
    return g_originalPresent != nullptr;
}

HRESULT STDMETHODCALLTYPE hookedCreateSwapChain(
    IDXGIFactory* factory,
    IUnknown* device,
    DXGI_SWAP_CHAIN_DESC* desc,
    IDXGISwapChain** out) {
    if (!g_originalCreateSwapChain) {
        return DXGI_ERROR_INVALID_CALL;
    }
    const HRESULT hr = g_originalCreateSwapChain(factory, device, desc, out);
    if (SUCCEEDED(hr) && out && *out) {
        rememberD3d12Queue(device);
        installPresentHooksFromSwap(*out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hookedCreateSwapChainForHwnd(
    IDXGIFactory2* factory,
    IUnknown* device,
    HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fsDesc,
    IDXGIOutput* restrictTo,
    IDXGISwapChain1** out) {
    if (!g_originalCreateSwapChainForHwnd) {
        return DXGI_ERROR_INVALID_CALL;
    }
    const HRESULT hr =
        g_originalCreateSwapChainForHwnd(factory, device, hwnd, desc, fsDesc, restrictTo, out);
    if (SUCCEEDED(hr) && out && *out) {
        rememberD3d12Queue(device);
        installPresentHooksFromSwap(*out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hookedCreateSwapChainForCoreWindow(
    IDXGIFactory2* factory, IUnknown* device, IUnknown* window,
    const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrictTo, IDXGISwapChain1** out) {
    if (!g_originalCreateSwapChainForCoreWindow) return DXGI_ERROR_INVALID_CALL;
    const HRESULT hr = g_originalCreateSwapChainForCoreWindow(factory, device, window, desc, restrictTo, out);
    if (SUCCEEDED(hr) && out && *out) {
        rememberD3d12Queue(device);
        installPresentHooksFromSwap(*out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hookedCreateSwapChainForComposition(
    IDXGIFactory2* factory, IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc,
    IDXGIOutput* restrictTo, IDXGISwapChain1** out) {
    if (!g_originalCreateSwapChainForComposition) return DXGI_ERROR_INVALID_CALL;
    const HRESULT hr = g_originalCreateSwapChainForComposition(factory, device, desc, restrictTo, out);
    if (SUCCEEDED(hr) && out && *out) {
        rememberD3d12Queue(device);
        installPresentHooksFromSwap(*out);
    }
    return hr;
}

bool installFactoryHooks() {
    IDXGIFactory* factory = nullptr;
    HRESULT hr = CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) {
        return false;
    }
    void** vtable = *reinterpret_cast<void***>(factory);
    g_originalCreateSwapChain = reinterpret_cast<CreateSwapChainFn>(vtable[10]); // CreateSwapChain
    const bool ok = detourAttach(
        reinterpret_cast<void**>(&g_originalCreateSwapChain),
        reinterpret_cast<void*>(&hookedCreateSwapChain));
    if (ok) {
        g_createSwapChainAttached = true;
    } else {
        g_originalCreateSwapChain = nullptr;
    }

    IDXGIFactory2* factory2 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory2))) &&
        factory2) {
        void** v2 = *reinterpret_cast<void***>(factory2);
        // IDXGIFactory2::CreateSwapChainForHwnd is slot 15 (after Factory1 methods).
        g_originalCreateSwapChainForHwnd = reinterpret_cast<CreateSwapChainForHwndFn>(v2[15]);
        if (detourAttach(
                reinterpret_cast<void**>(&g_originalCreateSwapChainForHwnd),
                reinterpret_cast<void*>(&hookedCreateSwapChainForHwnd))) {
            g_createSwapChainForHwndAttached = true;
        } else {
            g_originalCreateSwapChainForHwnd = nullptr;
        }
        g_originalCreateSwapChainForCoreWindow = reinterpret_cast<CreateSwapChainForCoreWindowFn>(v2[16]);
        if (detourAttach(
                reinterpret_cast<void**>(&g_originalCreateSwapChainForCoreWindow),
                reinterpret_cast<void*>(&hookedCreateSwapChainForCoreWindow))) {
            g_createSwapChainForCoreWindowAttached = true;
        } else {
            g_originalCreateSwapChainForCoreWindow = nullptr;
        }
        g_originalCreateSwapChainForComposition = reinterpret_cast<CreateSwapChainForCompositionFn>(v2[17]);
        if (detourAttach(
                reinterpret_cast<void**>(&g_originalCreateSwapChainForComposition),
                reinterpret_cast<void*>(&hookedCreateSwapChainForComposition))) {
            g_createSwapChainForCompositionAttached = true;
        } else {
            g_originalCreateSwapChainForComposition = nullptr;
        }
        factory2->Release();
    }
    factory->Release();
    if (ok && g_header) {
        g_header->flags |= kGameCapFlagFactoryHooked;
    }
    return ok;
}

bool installHooks() {
    const DWORD startTick = GetTickCount();
    const uint64_t startQpc = qpcNow();
    detectGraphicsApi();

    const bool hasGl = processHasOpenGl() || GetModuleHandleW(L"opengl32.dll") != nullptr;
    const bool hasDxgi =
        GetModuleHandleW(L"d3d11.dll") != nullptr || GetModuleHandleW(L"dxgi.dll") != nullptr;
    const bool hasVk = GetModuleHandleW(L"vulkan-1.dll") != nullptr;

    // OpenGL-only: IAT hook only (never LoadLibrary / prologue smash).
    if (hasGl && !hasDxgi) {
        if (!installGlCapture()) {
            return false;
        }
        g_active.store(true);
        if (g_header) {
            g_header->state = GameCaptureSlotState::Active;
            g_header->errorCode = 0;
            setHookInstallTiming(startQpc, startTick);
            g_header->graphicsApi = static_cast<uint32_t>(GameCaptureGraphicsApi::OpenGL);
        }
        return true;
    }

    if (hasVk && !hasDxgi && !hasGl) {
        setHookError(L"Vulkan game capture is not supported");
        return false;
    }
    // Hybrid targets commonly load d3d11.dll for video, overlays, or launchers
    // while rendering through OpenGL. Keep the GL hook installed independently.
    const bool glHooked = hasGl && installGlCapture();
    if (hasGl) {
        if (!glHooked) {
            setHookInfo(L"OpenGL hook unavailable; trying DXGI hooks");
        }
    }

    // Factory hooks catch future swapchains created after inject.
    installFactoryHooks();

    g_probeHwnd = createProbeWindow();
    if (!g_probeHwnd) {
        setHookError(L"Probe window create failed");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferDesc.Width = 2;
    scd.BufferDesc.Height = 2;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = g_probeHwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* swap = nullptr;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &scd,
        &swap,
        &device,
        &level,
        &ctx);
    if (FAILED(hr) || !swap) {
        destroyProbeWindow();
        // Factory hook alone may still catch game CreateSwapChain.
        if (g_originalCreateSwapChain) {
            setHookInfo(L"probe device failed; factory hook only");
            g_active.store(true);
            if (g_header) {
                g_header->state = GameCaptureSlotState::Active;
                g_header->errorCode = 0;
                setHookInstallTiming(startQpc, startTick);
            }
            return true;
        }
        setHookError(L"D3D11CreateDeviceAndSwapChain probe failed", static_cast<uint32_t>(hr));
        return false;
    }

    if (!installPresentHooksFromSwap(swap)) {
        swap->Release();
        if (ctx) {
            ctx->Release();
        }
        if (device) {
            device->Release();
        }
        destroyProbeWindow();
        return false;
    }

    wchar_t apiNote[128] = L"hooked Present";
    if (glHooked) {
        wcsncpy_s(apiNote, L"hooked OpenGL swap + Present", _TRUNCATE);
    }
    if (GetModuleHandleW(L"d3d12.dll")) {
        wcsncpy_s(apiNote, L"hooked Present; d3d12.dll loaded", _TRUNCATE);
    } else if (GetModuleHandleW(L"d3d11.dll")) {
        wcsncpy_s(apiNote, L"hooked Present; d3d11.dll loaded", _TRUNCATE);
    } else if (GetModuleHandleW(L"d3d9.dll")) {
        wcsncpy_s(apiNote, L"hooked Present; d3d9.dll loaded (need D3D9 hook)", _TRUNCATE);
    }
    if (g_originalCreateSwapChain) {
        wcsncat_s(apiNote, L"; factory hooked", _TRUNCATE);
    }
    setHookInfo(apiNote);

    swap->Release();
    if (ctx) {
        ctx->Release();
    }
    if (device) {
        device->Release();
    }
    destroyProbeWindow();

    g_active.store(true);
    if (g_header) {
        g_header->state = GameCaptureSlotState::Active;
        g_header->errorCode = 0;
        setHookInstallTiming(startQpc, startTick);
    }
    return true;
}

bool openSharedFromHost() {
    if (g_header) {
        return true;
    }
    for (int i = 0; i < 100 && !g_mapping; ++i) {
        g_mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kGameCaptureShmName);
        if (!g_mapping) {
            Sleep(20);
        }
    }
    if (!g_mapping) {
        return false;
    }
    g_header = static_cast<GameCaptureSharedHeader*>(
        MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GameCaptureSharedHeader)));
    if (!g_header) {
        return false;
    }
    if (g_header->magic != kGameCaptureMagic || g_header->version != kGameCaptureProtocolVersion) {
        setHookError(L"Game capture protocol mismatch");
        return false;
    }
    g_header->targetPid = GetCurrentProcessId();
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    g_header->qpcFrequency = static_cast<uint64_t>(frequency.QuadPart);
    if (!g_event) {
        g_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kGameCaptureEventName);
    }
    return true;
}

// SEH-safe install: no C++ objects with dtors in this function.
bool installHooksSeh() {
    bool ok = false;
    __try {
        ok = installHooks();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        noteException(GetExceptionCode());
        setHookError(L"SEH during hook install", GetExceptionCode());
        ok = false;
    }
    return ok;
}

bool startCaptureInternal() {
    if (g_started.load(std::memory_order_acquire)) {
        g_active.store(true);
        if (g_header) {
            g_header->state = GameCaptureSlotState::Active;
            setHookInfo(glCaptureInstalled()
                ? L"GL hook resumed; waiting for frames"
                : L"Present hook resumed; waiting for frames");
        }
        return true;
    }
    if (g_starting.test_and_set(std::memory_order_acquire)) {
        return false;
    }

    const auto clearStarting = [] {
        g_starting.clear(std::memory_order_release);
    };
    if (!openSharedFromHost()) {
        clearStarting();
        return false;
    }
    if (g_header->state == GameCaptureSlotState::Error ||
        g_header->state == GameCaptureSlotState::Stopping) {
        g_header->state = GameCaptureSlotState::Idle;
        g_header->errorCode = 0;
        g_header->errorText[0] = L'\0';
        g_header->lastExceptionCode = 0;
    }
    g_header->targetPid = GetCurrentProcessId();

    if (!g_originalPresent && !g_originalCreateSwapChain && !glCaptureInstalled()) {
        if (!installHooksSeh()) {
            clearStarting();
            return false;
        }
    } else {
        g_active.store(true);
        g_header->state = GameCaptureSlotState::Active;
        if (glCaptureInstalled()) {
            installGlCapture();
            setHookInfo(L"GL hook already installed; waiting for frames");
        } else {
            setHookInfo(L"Present hook already installed; waiting for frames");
        }
    }
    g_started.store(true, std::memory_order_release);
    clearStarting();
    return true;
}

void cleanup() {
    g_active.store(false);
    g_resizeInFlight.store(false);

    uninstallGlCapture();

    if (g_presentAttached) {
        detourDetach(reinterpret_cast<void**>(&g_originalPresent), reinterpret_cast<void*>(&hookedPresent));
        g_presentAttached = false;
    }
    if (g_present1Attached) {
        detourDetach(reinterpret_cast<void**>(&g_originalPresent1), reinterpret_cast<void*>(&hookedPresent1));
        g_present1Attached = false;
    }
    if (g_resizeBuffersAttached) {
        detourDetach(reinterpret_cast<void**>(&g_originalResizeBuffers), reinterpret_cast<void*>(&hookedResizeBuffers));
        g_resizeBuffersAttached = false;
    }
    if (g_createSwapChainAttached) {
        detourDetach(reinterpret_cast<void**>(&g_originalCreateSwapChain), reinterpret_cast<void*>(&hookedCreateSwapChain));
        g_createSwapChainAttached = false;
    }
    if (g_createSwapChainForHwndAttached) {
        detourDetach(reinterpret_cast<void**>(&g_originalCreateSwapChainForHwnd), reinterpret_cast<void*>(&hookedCreateSwapChainForHwnd));
        g_createSwapChainForHwndAttached = false;
    }
    if (g_createSwapChainForCoreWindowAttached) {
        detourDetach(reinterpret_cast<void**>(&g_originalCreateSwapChainForCoreWindow), reinterpret_cast<void*>(&hookedCreateSwapChainForCoreWindow));
        g_createSwapChainForCoreWindowAttached = false;
    }
    if (g_createSwapChainForCompositionAttached) {
        detourDetach(reinterpret_cast<void**>(&g_originalCreateSwapChainForComposition), reinterpret_cast<void*>(&hookedCreateSwapChainForComposition));
        g_createSwapChainForCompositionAttached = false;
    }

    releaseShared();
    releaseShmem();
    if (g_context) {
        g_context->Release();
        g_context = nullptr;
    }
    if (g_device) {
        g_device->Release();
        g_device = nullptr;
    }
    releaseD3d12Bridge();
    destroyProbeWindow();
    if (g_header) {
        UnmapViewOfFile(g_header);
        g_header = nullptr;
    }
    if (g_mapping) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
    }
    if (g_event) {
        CloseHandle(g_event);
        g_event = nullptr;
    }
    g_started.store(false);
    g_starting.clear(std::memory_order_release);
}

} // namespace

extern "C" __declspec(dllexport) DWORD WINAPI BacktrackGameCapture_Start(LPVOID) {
    return startCaptureInternal() ? 1 : 0;
}

extern "C" __declspec(dllexport) DWORD WINAPI BacktrackGameCapture_Stop(LPVOID) {
    g_active.store(false);
    // DLL stays loaded in target after host shutdown. Permit a later Backtrack
    // session to bind its new shared-memory header and resume installed hooks.
    g_started.store(false, std::memory_order_release);
    resetGlCaptureIpc();
    if (g_header) {
        g_header->state = GameCaptureSlotState::Stopping;
        UnmapViewOfFile(g_header);
        g_header = nullptr;
    }
    if (g_mapping) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
    }
    if (g_event) {
        CloseHandle(g_event);
        g_event = nullptr;
    }
    return 1;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
