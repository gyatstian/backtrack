// OpenGL game capture: IAT hook on wglSwapBuffers (no prologue smash) + glReadPixels.

#include "capture/game/GameCaptureGl.h"
#include "capture/game/GameCaptureProtocol.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <gl/GL.h>
#include <Psapi.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

// Windows' legacy gl/GL.h omits OpenGL 3.2 sync declarations.
using GLsync = void*;
using GLbitfield = unsigned int;
using GLuint64 = uint64_t;

#ifndef D3D11_RESOURCE_MISC_SHARED_NTHANDLE
#define D3D11_RESOURCE_MISC_SHARED_NTHANDLE 0x800L
#endif

using namespace backtrack::gamecap;

namespace {

using WglSwapBuffersFn = BOOL(WINAPI*)(HDC);
using GdiSwapBuffersFn = BOOL(WINAPI*)(HDC);
using WglGetCurrentContextFn = HGLRC(WINAPI*)();
using GlReadPixelsFn = void(APIENTRY*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
using GlGetIntegervFn = void(APIENTRY*)(GLenum, GLint*);
using GlPixelStoreiFn = void(APIENTRY*)(GLenum, GLint);
using GlGetErrorFn = GLenum(APIENTRY*)();
using GlGenBuffersFn = void(APIENTRY*)(GLsizei, GLuint*);
using GlDeleteBuffersFn = void(APIENTRY*)(GLsizei, const GLuint*);
using GlBindBufferFn = void(APIENTRY*)(GLenum, GLuint);
using GlBufferDataFn = void(APIENTRY*)(GLenum, ptrdiff_t, const void*, GLenum);
using GlMapBufferFn = void*(APIENTRY*)(GLenum, GLenum);
using GlUnmapBufferFn = GLboolean(APIENTRY*)(GLenum);
using GlFenceSyncFn = GLsync(APIENTRY*)(GLenum, GLbitfield);
using GlClientWaitSyncFn = GLenum(APIENTRY*)(GLsync, GLbitfield, GLuint64);
using GlDeleteSyncFn = void(APIENTRY*)(GLsync);

constexpr GLenum kGlBgra = 0x80E1;
constexpr GLenum kGlPackAlignment = 0x0D05;
constexpr GLenum kGlViewport = 0x0BA2;
constexpr GLenum kGlPixelPackBuffer = 0x88EB;
constexpr GLenum kGlStreamRead = 0x88E1;
constexpr GLenum kGlReadOnly = 0x88B8;
constexpr GLenum kGlSyncGpuCommandsComplete = 0x9117;
constexpr GLenum kGlAlreadySignaled = 0x911A;
constexpr GLenum kGlConditionSatisfied = 0x911C;

std::atomic<bool> g_glInstalled{false};
std::atomic<bool> g_glActive{false};
// Stop runs on an injected remote thread without a GL context. Defer deletion
// of GL objects and host-session shared resources until the next game swap.
std::atomic<bool> g_glTransportResetPending{false};

WglSwapBuffersFn g_originalWglSwapBuffers = nullptr;
GdiSwapBuffersFn g_originalGdiSwapBuffers = nullptr;
WglGetCurrentContextFn g_wglGetCurrentContext = nullptr;
GlReadPixelsFn g_glReadPixels = nullptr;
GlGetIntegervFn g_glGetIntegerv = nullptr;
GlPixelStoreiFn g_glPixelStorei = nullptr;
GlGetErrorFn g_glGetError = nullptr;
GlGenBuffersFn g_glGenBuffers = nullptr;
GlDeleteBuffersFn g_glDeleteBuffers = nullptr;
GlBindBufferFn g_glBindBuffer = nullptr;
GlBufferDataFn g_glBufferData = nullptr;
GlMapBufferFn g_glMapBuffer = nullptr;
GlUnmapBufferFn g_glUnmapBuffer = nullptr;
GlFenceSyncFn g_glFenceSync = nullptr;
GlClientWaitSyncFn g_glClientWaitSync = nullptr;
GlDeleteSyncFn g_glDeleteSync = nullptr;

// Restored IAT slots on uninstall (module base, thunk pointer, original).
struct IatPatch {
    void** thunk = nullptr;
    void* original = nullptr;
};
constexpr int kMaxIatPatches = 256;
IatPatch g_iatPatches[kMaxIatPatches] = {};
int g_iatPatchCount = 0;

ID3D11Device* g_glDevice = nullptr;
ID3D11DeviceContext* g_glContext = nullptr;
ID3D11Texture2D* g_glSharedTex = nullptr;
IDXGIKeyedMutex* g_glSharedMutex = nullptr;
ID3D11Texture2D* g_glStaging = nullptr;
HANDLE g_glSharedHandle = nullptr;
bool g_glSharedNt = false;
uint32_t g_glW = 0;
uint32_t g_glH = 0;
std::vector<uint8_t> g_glPixels;
std::atomic<bool> g_captureBusy{false};
struct GlPbo { GLuint buffer = 0; GLsync fence = nullptr; uint32_t width = 0; uint32_t height = 0; };
GlPbo g_pbos[3] = {};
uint32_t g_nextPbo = 0;
uint64_t g_lastCaptureQpc = 0;
GameCaptureSharedHeader* g_glHeader = nullptr;
HANDLE g_glMapping = nullptr;
HANDLE g_glEvent = nullptr;

bool resolveGlExtensions();

GameCaptureSharedHeader* header() {
    if (g_glHeader) {
        return g_glHeader;
    }
    for (int i = 0; i < 50 && !g_glMapping; ++i) {
        g_glMapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kGameCaptureShmName);
        if (!g_glMapping) {
            Sleep(10);
        }
    }
    if (!g_glMapping) {
        return nullptr;
    }
    g_glHeader = static_cast<GameCaptureSharedHeader*>(
        MapViewOfFile(g_glMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GameCaptureSharedHeader)));
    return g_glHeader;
}

HANDLE frameEvent() {
    if (!g_glEvent) {
        g_glEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, kGameCaptureEventName);
    }
    return g_glEvent;
}

void setGlInfo(const wchar_t* text) {
    auto* h = header();
    if (h && text) {
        wcsncpy_s(h->errorText, text, _TRUNCATE);
    }
}

void setGlError(const wchar_t* text, uint32_t code = 0) {
    auto* h = header();
    if (!h) {
        return;
    }
    h->errorCode = code;
    h->state = GameCaptureSlotState::Error;
    if (text) {
        wcsncpy_s(h->errorText, text, _TRUNCATE);
    }
}

void releaseGlShared() {
    if (g_glStaging) {
        g_glStaging->Release();
        g_glStaging = nullptr;
    }
    if (g_glSharedTex) {
        g_glSharedTex->Release();
        g_glSharedTex = nullptr;
    }
    if (g_glSharedMutex) {
        g_glSharedMutex->Release();
        g_glSharedMutex = nullptr;
    }
    if (g_glSharedNt && g_glSharedHandle) {
        CloseHandle(g_glSharedHandle);
    }
    g_glSharedHandle = nullptr;
    g_glSharedNt = false;
    g_glW = 0;
    g_glH = 0;
}

void releasePbos() {
    for (auto& pbo : g_pbos) {
        if (pbo.fence && g_glDeleteSync) g_glDeleteSync(pbo.fence);
        if (pbo.buffer && g_glDeleteBuffers) g_glDeleteBuffers(1, &pbo.buffer);
        pbo = {};
    }
    g_nextPbo = 0;
}

bool ensureGlD3d() {
    if (g_glDevice && g_glContext) {
        return true;
    }
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &g_glDevice,
        &level,
        &g_glContext);
    if (FAILED(hr) || !g_glDevice) {
        return false;
    }
    return true;
}

bool ensureGlShared(uint32_t w, uint32_t h) {
    if (!ensureGlD3d()) {
        return false;
    }
    if (g_glSharedTex && g_glW == w && g_glH == h) {
        return true;
    }
    releaseGlShared();
    releasePbos();
    if (auto* h = header()) {
        InterlockedIncrement(&h->sharedTextureRecreateCount);
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    // Producer owns key 0 and host owns key 1. The host must never copy while
    // the GL bridge writes its shared texture.
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = g_glDevice->CreateTexture2D(&desc, nullptr, &tex);
    bool nt = true;
    if (FAILED(hr)) {
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        hr = g_glDevice->CreateTexture2D(&desc, nullptr, &tex);
        nt = false;
    }
    if (FAILED(hr) || !tex) {
        return false;
    }

    HANDLE handle = nullptr;
    if (nt) {
        IDXGIResource1* r1 = nullptr;
        if (SUCCEEDED(tex->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&r1))) &&
            r1) {
            hr = r1->CreateSharedHandle(
                nullptr,
                DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                nullptr,
                &handle);
            r1->Release();
        }
        if (FAILED(hr) || !handle) {
            tex->Release();
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            hr = g_glDevice->CreateTexture2D(&desc, nullptr, &tex);
            nt = false;
            if (FAILED(hr) || !tex) {
                return false;
            }
        }
    }
    if (!nt) {
        IDXGIResource* r = nullptr;
        hr = tex->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&r));
        if (FAILED(hr) || !r) {
            tex->Release();
            return false;
        }
        hr = r->GetSharedHandle(&handle);
        r->Release();
        if (FAILED(hr) || !handle) {
            tex->Release();
            return false;
        }
    }

    IDXGIKeyedMutex* mutex = nullptr;
    hr = tex->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&mutex));
    if (FAILED(hr) || !mutex) {
        tex->Release();
        if (nt && handle) {
            CloseHandle(handle);
        }
        return false;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_DYNAMIC;
    stagingDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    stagingDesc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    hr = g_glDevice->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr) || !staging) {
        mutex->Release();
        tex->Release();
        if (nt && handle) {
            CloseHandle(handle);
        }
        return false;
    }

    g_glSharedTex = tex;
    g_glSharedMutex = mutex;
    g_glStaging = staging;
    g_glSharedHandle = handle;
    g_glSharedNt = nt;
    g_glW = w;
    g_glH = h;
    g_glPixels.resize(static_cast<size_t>(w) * h * 4);
    return true;
}

void publishGlFrame(uint32_t width, uint32_t height) {
    auto* hdr = header();
    if (!hdr || !g_glActive.load(std::memory_order_relaxed)) {
        return;
    }
    if (hdr->state == GameCaptureSlotState::Stopping) {
        g_glActive.store(false, std::memory_order_relaxed);
        return;
    }
    if (!ensureGlShared(width, height) || !g_glStaging || !g_glContext || !g_glSharedMutex) {
        return;
    }

    // AcquireSync returns WAIT_TIMEOUT (a successful HRESULT value) when key is
    // owned by host. Only S_OK means this producer owns key 0.
    if (g_glSharedMutex->AcquireSync(0, 0) != S_OK) {
        InterlockedIncrement(&hdr->producerKeyedMutexMisses);
        InterlockedIncrement(&hdr->droppedByContention);
        return;
    }

    LARGE_INTEGER copyStart{};
    QueryPerformanceCounter(&copyStart);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = g_glContext->Map(g_glStaging, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        g_glSharedMutex->ReleaseSync(0);
        return;
    }
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* src = g_glPixels.data() + static_cast<size_t>(height - 1 - y) * rowBytes;
        uint8_t* dst = static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch;
        std::memcpy(dst, src, rowBytes);
    }
    g_glContext->Unmap(g_glStaging, 0);
    g_glContext->CopyResource(g_glSharedTex, g_glStaging);
    // Submit copy before handing key 1 to host. Some drivers reject a keyed
    // mutex release while copy remains deferred on this independent device.
    g_glContext->Flush();
    const HRESULT releaseHr = g_glSharedMutex->ReleaseSync(1);
    if (FAILED(releaseHr)) {
        wchar_t error[128] = {};
        swprintf_s(error, L"OpenGL shared texture mutex release failed (0x%08X)",
            static_cast<uint32_t>(releaseHr));
        setGlError(error, static_cast<uint32_t>(releaseHr));
        return;
    }
    LARGE_INTEGER copyEnd{};
    QueryPerformanceCounter(&copyEnd);
    InterlockedAdd64(&hdr->producerCopyQpc, copyEnd.QuadPart - copyStart.QuadPart);

    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    InterlockedIncrement(&hdr->publishEpoch);
    auto& descriptor = hdr->frames[0];
    descriptor.width = width;
    descriptor.height = height;
    descriptor.format = static_cast<uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM);
    descriptor.sharedHandle = reinterpret_cast<uint64_t>(g_glSharedHandle);
    descriptor.flags = g_glSharedNt ? kGameCapFlagNtHandle : 0;
    descriptor.presentQpc = qpc.QuadPart;
    hdr->latestReadySlot = 0;
    hdr->width = width;
    hdr->height = height;
    hdr->format = static_cast<uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM);
    hdr->sharedHandle = reinterpret_cast<uint64_t>(g_glSharedHandle);
    hdr->lastPresentQpc = qpc.QuadPart;
    hdr->framesPublished += 1;
    hdr->sequence += 1;
    hdr->state = GameCaptureSlotState::Active;
    hdr->errorCode = 0;
    hdr->graphicsApi = static_cast<uint32_t>(GameCaptureGraphicsApi::OpenGL);
    if (g_glSharedNt) {
        hdr->flags |= kGameCapFlagNtHandle;
    }
    hdr->flags |= kGameCapFlagPresentHooked;
    hdr->errorText[0] = L'\0';
    InterlockedIncrement(&hdr->publishEpoch);
    if (HANDLE evt = frameEvent()) {
        SetEvent(evt);
    }
}

void captureGlFramebuffer(HDC hdc) {
    if (!g_glReadPixels || !g_glGetIntegerv || !g_glPixelStorei) {
        return;
    }
    if (!g_glActive.load(std::memory_order_relaxed)) {
        return;
    }
    if (g_wglGetCurrentContext && !g_wglGetCurrentContext()) {
        return;
    }
    if (!resolveGlExtensions()) {
        setGlError(L"OpenGL extensions unavailable on render context");
        return;
    }
    if (g_captureBusy.exchange(true, std::memory_order_acq_rel)) {
        if (auto* h = header()) {
            InterlockedIncrement(&h->droppedByContention);
        }
        return;
    }

    if (g_glTransportResetPending.exchange(false, std::memory_order_acq_rel)) {
        releasePbos();
        releaseGlShared();
        g_glPixels.clear();
        g_nextPbo = 0;
        g_lastCaptureQpc = 0;
    }

    uint32_t w = 0;
    uint32_t h = 0;
    GLint vp[4] = {};
    g_glGetIntegerv(kGlViewport, vp);
    if (vp[2] > 0 && vp[3] > 0) {
        w = static_cast<uint32_t>(vp[2]);
        h = static_cast<uint32_t>(vp[3]);
    }
    if ((w == 0 || h == 0) && hdc) {
        HWND hwnd = WindowFromDC(hdc);
        if (hwnd) {
            RECT rc{};
            if (GetClientRect(hwnd, &rc)) {
                w = static_cast<uint32_t>(rc.right - rc.left);
                h = static_cast<uint32_t>(rc.bottom - rc.top);
            }
        }
    }
    if (w == 0 || h == 0 || w > 7680 || h > 4320) {
        g_captureBusy.store(false, std::memory_order_release);
        return;
    }

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const uint32_t fps = header() && header()->captureFps ? header()->captureFps : 60;
    if (g_lastCaptureQpc && now.QuadPart - g_lastCaptureQpc < frequency.QuadPart / fps) {
        if (auto* h = header()) {
            InterlockedIncrement(&h->droppedByCadence);
        }
        g_captureBusy.store(false, std::memory_order_release);
        return;
    }

    GLint oldPackAlignment = 4;
    g_glGetIntegerv(kGlPackAlignment, &oldPackAlignment);
    g_glPixelStorei(kGlPackAlignment, 1);
    const size_t need = static_cast<size_t>(w) * h * 4;
    GlPbo& ready = g_pbos[(g_nextPbo + 1) % _countof(g_pbos)];
    const GLenum readyStatus = ready.fence && g_glClientWaitSync
        ? g_glClientWaitSync(ready.fence, 0, 0)
        : 0;
    if ((readyStatus == kGlAlreadySignaled || readyStatus == kGlConditionSatisfied) &&
        ready.width == w && ready.height == h) {
        g_glBindBuffer(kGlPixelPackBuffer, ready.buffer);
        void* pixels = g_glMapBuffer(kGlPixelPackBuffer, kGlReadOnly);
        if (pixels) {
            g_glPixels.resize(need);
            std::memcpy(g_glPixels.data(), pixels, need);
            g_glUnmapBuffer(kGlPixelPackBuffer);
            g_glDeleteSync(ready.fence);
            ready.fence = nullptr;
            publishGlFrame(w, h);
        }
        g_glBindBuffer(kGlPixelPackBuffer, 0);
    }
    GlPbo& pbo = g_pbos[g_nextPbo];
    if (!pbo.fence) {
        if (!pbo.buffer) g_glGenBuffers(1, &pbo.buffer);
        g_glBindBuffer(kGlPixelPackBuffer, pbo.buffer);
        g_glBufferData(kGlPixelPackBuffer, need, nullptr, kGlStreamRead);
        // Queue default-framebuffer readback. Fence is polled on later swaps;
        // never wait for GPU completion on game render thread.
        g_glReadPixels(
            vp[0], vp[1], static_cast<GLsizei>(w), static_cast<GLsizei>(h),
            kGlBgra, GL_UNSIGNED_BYTE, nullptr);
        pbo.fence = g_glFenceSync(kGlSyncGpuCommandsComplete, 0);
        pbo.width = w;
        pbo.height = h;
        g_nextPbo = (g_nextPbo + 1) % _countof(g_pbos);
    }
    g_glBindBuffer(kGlPixelPackBuffer, 0);
    g_glPixelStorei(kGlPackAlignment, oldPackAlignment);
    g_lastCaptureQpc = now.QuadPart;
    g_captureBusy.store(false, std::memory_order_release);
}

void captureGlSeh(HDC hdc) {
    __try {
        captureGlFramebuffer(hdc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_captureBusy.store(false, std::memory_order_release);
        auto* h = header();
        if (h) {
            h->lastExceptionCode = GetExceptionCode();
            h->flags |= kGameCapFlagPublishException;
        }
    }
}

BOOL WINAPI hookedWglSwapBuffers(HDC hdc) {
    // Capture back buffer BEFORE swap (after swap BACK is undefined).
    // Always call original — never skip game present even if capture faults.
    if (g_glActive.load(std::memory_order_relaxed)) {
        captureGlSeh(hdc);
    }
    WglSwapBuffersFn original = g_originalWglSwapBuffers;
    if (!original) {
        if (HMODULE gl = GetModuleHandleW(L"opengl32.dll")) {
            original = reinterpret_cast<WglSwapBuffersFn>(GetProcAddress(gl, "wglSwapBuffers"));
        }
    }
    if (original && original != &hookedWglSwapBuffers) {
        return original(hdc);
    }
    return FALSE;
}

BOOL WINAPI hookedGdiSwapBuffers(HDC hdc) {
    if (g_glActive.load(std::memory_order_relaxed)) {
        captureGlSeh(hdc);
    }
    GdiSwapBuffersFn original = g_originalGdiSwapBuffers;
    if (!original) {
        if (HMODULE gdi = GetModuleHandleW(L"gdi32.dll")) {
            original = reinterpret_cast<GdiSwapBuffersFn>(GetProcAddress(gdi, "SwapBuffers"));
        }
    }
    if (original && original != &hookedGdiSwapBuffers) {
        return original(hdc);
    }
    return FALSE;
}

bool patchIatInModule(HMODULE module, const char* importDll, void* original, void* detour, void** firstOriginal) {
    if (!module || !importDll || !original || !detour || !firstOriginal || g_iatPatchCount >= kMaxIatPatches) {
        return false;
    }
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<uint8_t*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) {
        return false;
    }
    auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        reinterpret_cast<uint8_t*>(module) + dir.VirtualAddress);
    bool any = false;
    for (; imp->Name; ++imp) {
        const char* dllName =
            reinterpret_cast<const char*>(reinterpret_cast<uint8_t*>(module) + imp->Name);
        if (_stricmp(dllName, importDll) != 0) {
            continue;
        }
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            reinterpret_cast<uint8_t*>(module) + imp->FirstThunk);
        for (; thunk->u1.Function; ++thunk) {
            if (reinterpret_cast<void*>(thunk->u1.Function) != original) {
                continue;
            }
            void** slot = reinterpret_cast<void**>(&thunk->u1.Function);
            DWORD old = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
                continue;
            }
            if (*firstOriginal == nullptr) {
                *firstOriginal = *slot;
            }
            *slot = detour;
            VirtualProtect(slot, sizeof(void*), old, &old);
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
            if (g_iatPatchCount < kMaxIatPatches) {
                g_iatPatches[g_iatPatchCount++] = IatPatch{slot, original};
            }
            any = true;
        }
    }
    return any;
}

int installIatHooks(const char* importDll, void* original, void* detour, void** firstOriginal) {
    HMODULE modules[512] = {};
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
        return 0;
    }
    const DWORD count = needed / sizeof(HMODULE);
    int patched = 0;
    wchar_t name[MAX_PATH] = {};
    for (DWORD i = 0; i < count && i < 512; ++i) {
        if (GetModuleBaseNameW(GetCurrentProcess(), modules[i], name, MAX_PATH) == 0) {
            continue;
        }
        // Skip system / self — patch game + engine modules.
        if (_wcsicmp(name, L"opengl32.dll") == 0 ||
            _wcsicmp(name, L"BacktrackGameCapture.dll") == 0 ||
            _wcsicmp(name, L"ntdll.dll") == 0 ||
            _wcsicmp(name, L"kernel32.dll") == 0 ||
            _wcsicmp(name, L"kernelbase.dll") == 0) {
            continue;
        }
        if (patchIatInModule(modules[i], importDll, original, detour, firstOriginal)) {
            ++patched;
        }
    }
    return patched;
}

void uninstallIatHooks() {
    for (int i = 0; i < g_iatPatchCount; ++i) {
        if (!g_iatPatches[i].thunk || !g_iatPatches[i].original) {
            continue;
        }
        DWORD old = 0;
        if (VirtualProtect(g_iatPatches[i].thunk, sizeof(void*), PAGE_READWRITE, &old)) {
            *g_iatPatches[i].thunk = g_iatPatches[i].original;
            VirtualProtect(g_iatPatches[i].thunk, sizeof(void*), old, &old);
            FlushInstructionCache(GetCurrentProcess(), g_iatPatches[i].thunk, sizeof(void*));
        }
        g_iatPatches[i] = {};
    }
    g_iatPatchCount = 0;
}

bool resolveGlProcs(HMODULE gl) {
    g_glReadPixels = reinterpret_cast<GlReadPixelsFn>(GetProcAddress(gl, "glReadPixels"));
    g_glGetIntegerv = reinterpret_cast<GlGetIntegervFn>(GetProcAddress(gl, "glGetIntegerv"));
    g_glPixelStorei = reinterpret_cast<GlPixelStoreiFn>(GetProcAddress(gl, "glPixelStorei"));
    g_glGetError = reinterpret_cast<GlGetErrorFn>(GetProcAddress(gl, "glGetError"));
    g_wglGetCurrentContext =
        reinterpret_cast<WglGetCurrentContextFn>(GetProcAddress(gl, "wglGetCurrentContext"));
    // Remote injection runs without a current GL context. Extension entry points
    // must therefore be resolved by the game's render thread on first swap.
    return g_glReadPixels && g_glGetIntegerv && g_glPixelStorei;
}

bool resolveGlExtensions() {
    if (g_glGenBuffers && g_glDeleteBuffers && g_glBindBuffer && g_glBufferData &&
        g_glMapBuffer && g_glUnmapBuffer && g_glFenceSync && g_glClientWaitSync &&
        g_glDeleteSync) {
        return true;
    }
    g_glGenBuffers = reinterpret_cast<GlGenBuffersFn>(wglGetProcAddress("glGenBuffers"));
    g_glDeleteBuffers = reinterpret_cast<GlDeleteBuffersFn>(wglGetProcAddress("glDeleteBuffers"));
    g_glBindBuffer = reinterpret_cast<GlBindBufferFn>(wglGetProcAddress("glBindBuffer"));
    g_glBufferData = reinterpret_cast<GlBufferDataFn>(wglGetProcAddress("glBufferData"));
    g_glMapBuffer = reinterpret_cast<GlMapBufferFn>(wglGetProcAddress("glMapBuffer"));
    g_glUnmapBuffer = reinterpret_cast<GlUnmapBufferFn>(wglGetProcAddress("glUnmapBuffer"));
    g_glFenceSync = reinterpret_cast<GlFenceSyncFn>(wglGetProcAddress("glFenceSync"));
    g_glClientWaitSync = reinterpret_cast<GlClientWaitSyncFn>(wglGetProcAddress("glClientWaitSync"));
    g_glDeleteSync = reinterpret_cast<GlDeleteSyncFn>(wglGetProcAddress("glDeleteSync"));
    return g_glGenBuffers && g_glDeleteBuffers &&
        g_glBindBuffer && g_glBufferData && g_glMapBuffer && g_glUnmapBuffer && g_glFenceSync &&
        g_glClientWaitSync && g_glDeleteSync;
}

// POD-only SEH boundary: VirtualProtect / IAT walk can AV on exotic modules.
bool installGlCaptureBodySeh(int* patchedOut, DWORD* sehCodeOut) {
    *patchedOut = 0;
    *sehCodeOut = 0;
    __try {
        HMODULE gl = GetModuleHandleW(L"opengl32.dll");
        if (!gl) {
            setGlError(L"opengl32.dll not loaded in target (cannot IAT-hook)");
            return false;
        }

        void* realWgl = reinterpret_cast<void*>(GetProcAddress(gl, "wglSwapBuffers"));
        if (!realWgl) {
            setGlError(L"wglSwapBuffers not found");
            return false;
        }
        if (!resolveGlProcs(gl)) {
            setGlError(L"GL entry points missing");
            return false;
        }

        g_originalWglSwapBuffers = reinterpret_cast<WglSwapBuffersFn>(realWgl);
        const int wglPatched = installIatHooks(
            "opengl32.dll",
            realWgl,
            reinterpret_cast<void*>(&hookedWglSwapBuffers),
            reinterpret_cast<void**>(&g_originalWglSwapBuffers));

        HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
        void* realGdiSwap = gdi
            ? reinterpret_cast<void*>(GetProcAddress(gdi, "SwapBuffers"))
            : nullptr;
        if (realGdiSwap) {
            g_originalGdiSwapBuffers = reinterpret_cast<GdiSwapBuffersFn>(realGdiSwap);
        }
        const int gdiPatched = realGdiSwap
            ? installIatHooks(
                  "gdi32.dll",
                  realGdiSwap,
                  reinterpret_cast<void*>(&hookedGdiSwapBuffers),
                  reinterpret_cast<void**>(&g_originalGdiSwapBuffers))
            : 0;
        const int patched = wglPatched + gdiPatched;
        *patchedOut = patched;
        if (patched <= 0) {
            setGlError(L"IAT hook: no module imported wglSwapBuffers or SwapBuffers");
            g_originalWglSwapBuffers = nullptr;
            g_originalGdiSwapBuffers = nullptr;
            return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *sehCodeOut = GetExceptionCode();
        uninstallIatHooks();
        g_originalWglSwapBuffers = nullptr;
        g_glReadPixels = nullptr;
        g_glGetIntegerv = nullptr;
        g_glPixelStorei = nullptr;
        g_glGetError = nullptr;
        g_wglGetCurrentContext = nullptr;
        return false;
    }
}

} // namespace

namespace backtrack::gamecap {

bool processHasOpenGl() {
    return GetModuleHandleW(L"opengl32.dll") != nullptr;
}

bool glCaptureInstalled() {
    return g_glInstalled.load(std::memory_order_acquire);
}

void resetGlCaptureIpc() {
    g_glActive.store(false, std::memory_order_release);
    g_glTransportResetPending.store(true, std::memory_order_release);
    if (g_glHeader) {
        UnmapViewOfFile(g_glHeader);
        g_glHeader = nullptr;
    }
    if (g_glMapping) {
        CloseHandle(g_glMapping);
        g_glMapping = nullptr;
    }
    if (g_glEvent) {
        CloseHandle(g_glEvent);
        g_glEvent = nullptr;
    }
}

bool installGlCapture() {
    if (g_glInstalled.load(std::memory_order_acquire)) {
        g_glActive.store(true, std::memory_order_release);
        return true;
    }

    int patched = 0;
    DWORD sehCode = 0;
    if (!installGlCaptureBodySeh(&patched, &sehCode)) {
        if (sehCode != 0) {
            auto* h = header();
            if (h) {
                h->lastExceptionCode = sehCode;
                h->flags |= kGameCapFlagPublishException;
            }
            setGlError(L"OpenGL IAT install exception (refusing unsafe hook)");
        }
        return false;
    }

    g_glActive.store(true, std::memory_order_release);
    g_glInstalled.store(true, std::memory_order_release);

    wchar_t note[96] = {};
    swprintf_s(note, L"IAT hooked wglSwapBuffers in %d module(s)", patched);
    setGlInfo(note);
    auto* h = header();
    if (h) {
        h->graphicsApi = static_cast<uint32_t>(GameCaptureGraphicsApi::OpenGL);
        h->flags |= kGameCapFlagPresentHooked;
        h->state = GameCaptureSlotState::Active;
        h->errorCode = 0;
    }
    return true;
}

void uninstallGlCapture() {
    g_glActive.store(false, std::memory_order_release);
    if (!g_glInstalled.load(std::memory_order_acquire)) {
        return;
    }
    uninstallIatHooks();
    releasePbos();
    releaseGlShared();
    if (g_glContext) {
        g_glContext->Release();
        g_glContext = nullptr;
    }
    if (g_glDevice) {
        g_glDevice->Release();
        g_glDevice = nullptr;
    }
    g_glPixels.clear();
    g_originalWglSwapBuffers = nullptr;
    g_originalGdiSwapBuffers = nullptr;
    g_glTransportResetPending.store(false, std::memory_order_release);
    g_glInstalled.store(false, std::memory_order_release);
}

} // namespace backtrack::gamecap
