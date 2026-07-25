#include "capture/game/GameCaptureSource.h"

#include "capture/D3DDevice.h"
#include "capture/game/AntiCheatGuard.h"
#include "capture/game/GameCaptureProtocol.h"
#include "core/Logger.h"
#include "platform/Win32Util.h"

#include <d3d11_1.h>
#include <Psapi.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <mutex>
#include <string>

namespace backtrack {
namespace {

std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> g_lastTargetExitAt;
std::mutex g_targetExitMutex;

uint32_t pidFromHwnd(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return 0;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return static_cast<uint32_t>(pid);
}

HWND findCoveringWindow(HMONITOR monitor) {
    const HWND foreground = GetForegroundWindow();
    if (!foreground || IsIconic(foreground)) {
        return nullptr;
    }
    if (!monitor) {
        return foreground;
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT wr{};
    if (!GetMonitorInfoW(monitor, &mi) || !GetWindowRect(foreground, &wr)) {
        return nullptr;
    }
    // Only accept true cover — never inject random foreground partial windows.
    if (wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top &&
        wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom) {
        return foreground;
    }
    return nullptr;
}

int64_t qpcTo100ns(int64_t qpc) {
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    if (qpc == 0 || freq.QuadPart == 0) {
        return 0;
    }
    return static_cast<int64_t>((static_cast<long double>(qpc) * kHundredNanosecondsPerSecond) /
                               static_cast<long double>(freq.QuadPart));
}

std::wstring processImageBaseName(uint32_t pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return {};
    }
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        name = std::filesystem::path(path).filename().wstring();
    }
    CloseHandle(process);
    return name;
}

bool isBlacklistedProcessName(const std::wstring& baseName) {
    if (baseName.empty()) {
        return true;
    }
    static const wchar_t* kBlocked[] = {
        L"explorer.exe",
        L"SearchHost.exe",
        L"ShellExperienceHost.exe",
        L"ApplicationFrameHost.exe",
        L"StartMenuExperienceHost.exe",
        L"TextInputHost.exe",
        L"LockApp.exe",
        L"dwm.exe",
        L"sihost.exe",
        L"RuntimeBroker.exe",
        L"SystemSettings.exe",
        L"Backtrack.exe",
    };
    for (const wchar_t* blocked : kBlocked) {
        if (_wcsicmp(baseName.c_str(), blocked) == 0) {
            return true;
        }
    }
    return false;
}

bool processIsWow64(HANDLE process, bool& isWow64) {
    BOOL wow = FALSE;
    if (!IsWow64Process(process, &wow)) {
        return false;
    }
    isWow64 = wow != FALSE;
    return true;
}

bool hostIsWow64() {
    BOOL wow = FALSE;
    IsWow64Process(GetCurrentProcess(), &wow);
    return wow != FALSE;
}

bool processStillAlive(HANDLE process) {
    if (!process) {
        return false;
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(process, &code)) {
        return false;
    }
    return code == STILL_ACTIVE;
}

enum class RemoteGraphicsApi {
    None,
    D3D,
    OpenGL,
    Vulkan,
};

// Best-effort remote module scan for captureable graphics stacks.
RemoteGraphicsApi detectRemoteGraphicsApi(HANDLE process, std::wstring* detailOut) {
    HMODULE modules[512] = {};
    DWORD needed = 0;
    if (!EnumProcessModules(process, modules, sizeof(modules), &needed)) {
        // Protected process: allow inject; filter only blacklist.
        if (detailOut) {
            *detailOut = L"modules-enum-failed (allow inject)";
        }
        return RemoteGraphicsApi::D3D;
    }
    const DWORD count = needed / sizeof(HMODULE);
    bool hasD3d = false;
    bool hasGl = false;
    bool hasVk = false;
    wchar_t name[MAX_PATH] = {};
    for (DWORD i = 0; i < count && i < 512; ++i) {
        if (GetModuleBaseNameW(process, modules[i], name, MAX_PATH) == 0) {
            continue;
        }
        if (_wcsicmp(name, L"d3d11.dll") == 0 || _wcsicmp(name, L"dxgi.dll") == 0 ||
            _wcsicmp(name, L"d3d12.dll") == 0 || _wcsicmp(name, L"d3d9.dll") == 0) {
            hasD3d = true;
        } else if (_wcsicmp(name, L"opengl32.dll") == 0) {
            hasGl = true;
        } else if (_wcsicmp(name, L"vulkan-1.dll") == 0) {
            hasVk = true;
        }
    }
    if (detailOut) {
        std::wstring d;
        if (hasD3d) {
            d += L"D3D ";
        }
        if (hasGl) {
            d += L"OpenGL ";
        }
        if (hasVk) {
            d += L"Vulkan ";
        }
        if (d.empty()) {
            d = L"none";
        }
        *detailOut = d;
    }
    if (hasD3d) {
        return RemoteGraphicsApi::D3D;
    }
    if (hasGl) {
        return RemoteGraphicsApi::OpenGL;
    }
    if (hasVk) {
        return RemoteGraphicsApi::Vulkan;
    }
    return RemoteGraphicsApi::None;
}

HMODULE remoteModuleBase(HANDLE process, const std::wstring& modulePath) {
    HMODULE modules[512] = {};
    DWORD needed = 0;
    if (!EnumProcessModules(process, modules, sizeof(modules), &needed)) {
        return nullptr;
    }

    const std::wstring expectedName = std::filesystem::path(modulePath).filename().wstring();
    wchar_t path[MAX_PATH] = {};
    const DWORD count = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < count && i < 512; ++i) {
        if (GetModuleFileNameExW(process, modules[i], path, MAX_PATH) == 0) {
            continue;
        }
        if (_wcsicmp(std::filesystem::path(path).filename().c_str(), expectedName.c_str()) == 0) {
            return modules[i];
        }
    }
    return nullptr;
}

} // namespace

GameCaptureSource::GameCaptureSource() = default;

GameCaptureSource::~GameCaptureSource() {
    shutdown();
}

void GameCaptureSource::setError(const wchar_t* text) {
    Logger::instance().warning(L"gamecap", text ? text : L"Game capture error");
    deviceLost_ = true;
}

bool GameCaptureSource::openSharedMapping(uint32_t hostPid) {
    mapping_ = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(sizeof(gamecap::GameCaptureSharedHeader)),
        gamecap::kGameCaptureShmName);
    if (!mapping_) {
        setError(L"CreateFileMapping for game capture failed");
        return false;
    }
    view_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(gamecap::GameCaptureSharedHeader));
    if (!view_) {
        setError(L"MapViewOfFile for game capture failed");
        return false;
    }
    auto* header = static_cast<gamecap::GameCaptureSharedHeader*>(view_);
    std::memset(header, 0, sizeof(*header));
    header->magic = gamecap::kGameCaptureMagic;
    header->version = gamecap::kGameCaptureProtocolVersion;
    header->headerBytes = static_cast<uint32_t>(sizeof(gamecap::GameCaptureSharedHeader));
    header->state = gamecap::GameCaptureSlotState::Idle;
    header->hostPid = hostPid;

    frameEvent_ = CreateEventW(nullptr, FALSE, FALSE, gamecap::kGameCaptureEventName);
    if (!frameEvent_) {
        setError(L"CreateEvent for game capture failed");
        return false;
    }
    return true;
}

bool GameCaptureSource::injectIntoProcess(uint32_t pid) {
    const std::wstring dllPath =
        (std::filesystem::path(moduleDirectory()) / gamecap::kGameCaptureDllFileName).wstring();
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        setError(L"BacktrackGameCapture.dll missing next to Backtrack.exe");
        return false;
    }

    const std::wstring imageName = processImageBaseName(pid);
    if (isBlacklistedProcessName(imageName)) {
        setError((L"Game capture blocked for process: " +
                  (imageName.empty() ? L"(unknown)" : imageName))
                     .c_str());
        return false;
    }

    // Anti-cheat guard (fail-closed). Never inject into a kernel-anti-cheat
    // title unless the user explicitly enabled the advanced override: injection
    // is detected as tampering and can ban the player. Callers fall back to
    // Windows Graphics Capture, which does not touch the game process.
    const AntiCheatKind antiCheat = detectAntiCheat(pid);
    if (antiCheat != AntiCheatKind::None && !allowAntiCheatInject_) {
        antiCheatBlocked_ = true;
        antiCheatKind_ = antiCheat;
        setError((std::wstring(L"Game capture blocked: ") +
                  antiCheatDisplayName(antiCheat) +
                  L" anti-cheat detected")
                     .c_str());
        return false;
    }

    process_ = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_DUP_HANDLE,
        FALSE,
        pid);
    if (!process_) {
        setError(L"OpenProcess failed (run as admin or game blocked injection)");
        return false;
    }

    bool targetWow64 = false;
    if (processIsWow64(process_, targetWow64)) {
        const bool hostWow = hostIsWow64();
#if defined(_WIN64)
        const bool host64 = !hostWow;
#else
        const bool host64 = false;
#endif
        const bool target64 = !targetWow64;
        if (host64 != target64) {
            setError(L"Game capture bitness mismatch (host/target 32/64)");
            return false;
        }
    }

    std::wstring apiDetail;
    const RemoteGraphicsApi remoteApi = detectRemoteGraphicsApi(process_, &apiDetail);
    // Vulkan-only: hard refuse. None: still inject (delay-loaded GL/D3D common).
    if (remoteApi == RemoteGraphicsApi::Vulkan) {
        setError((L"Vulkan game capture not supported yet: " + imageName).c_str());
        return false;
    }
    if (remoteApi == RemoteGraphicsApi::None) {
        apiDetail = L"unknown (delay-load ok)";
    }

    Logger::instance().info(
        L"gamecap",
        L"Inject target " + imageName + L" pid=" + std::to_wstring(pid) + L" api=" + apiDetail);

    const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process_, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        setError(L"VirtualAllocEx failed for game capture inject");
        return false;
    }
    if (!WriteProcessMemory(process_, remote, dllPath.c_str(), bytes, nullptr)) {
        VirtualFreeEx(process_, remote, 0, MEM_RELEASE);
        setError(L"WriteProcessMemory failed for game capture inject");
        return false;
    }

    const HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    const auto loadLibraryW = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(k32, "LoadLibraryW"));
    if (!loadLibraryW) {
        VirtualFreeEx(process_, remote, 0, MEM_RELEASE);
        setError(L"GetProcAddress(LoadLibraryW) failed");
        return false;
    }

    const HMODULE localDll = LoadLibraryW(dllPath.c_str());
    const FARPROC localStart =
        localDll ? GetProcAddress(localDll, "BacktrackGameCapture_Start") : nullptr;
    const ptrdiff_t startOffset =
        (localDll && localStart)
            ? (reinterpret_cast<const uint8_t*>(localStart) -
               reinterpret_cast<const uint8_t*>(localDll))
            : 0;

    if (remoteThread_) {
        CloseHandle(remoteThread_);
        remoteThread_ = nullptr;
    }
    remoteThread_ = CreateRemoteThread(process_, nullptr, 0, loadLibraryW, remote, 0, nullptr);
    if (!remoteThread_) {
        VirtualFreeEx(process_, remote, 0, MEM_RELEASE);
        if (localDll) {
            FreeLibrary(localDll);
        }
        setError(L"CreateRemoteThread(LoadLibraryW) failed");
        return false;
    }

    const DWORD wait = WaitForSingleObject(remoteThread_, 8000);
    VirtualFreeEx(process_, remote, 0, MEM_RELEASE);
    if (wait != WAIT_OBJECT_0) {
        if (localDll) {
            FreeLibrary(localDll);
        }
        if (!processStillAlive(process_)) {
            targetExited_ = true;
            setError(L"Target process exited during inject (likely crash/anti-cheat)");
        } else {
            setError(L"Inject thread timed out");
        }
        return false;
    }
    CloseHandle(remoteThread_);
    remoteThread_ = nullptr;
    const HMODULE remoteDll = remoteModuleBase(process_, dllPath);
    if (!remoteDll) {
        if (localDll) {
            FreeLibrary(localDll);
        }
        if (!processStillAlive(process_)) {
            targetExited_ = true;
            setError(L"Target process exited during LoadLibrary (likely crash/anti-cheat)");
        } else {
            setError(L"Loaded GameCapture DLL was not found in target module list");
        }
        return false;
    }

    if (startOffset > 0) {
        const auto remoteStart = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            reinterpret_cast<uint8_t*>(remoteDll) + startOffset);
        HANDLE startThread =
            CreateRemoteThread(process_, nullptr, 0, remoteStart, nullptr, 0, nullptr);
        if (startThread) {
            const DWORD startWait = WaitForSingleObject(startThread, 5000);
            DWORD startExitCode = 0;
            if (startWait == WAIT_OBJECT_0) {
                GetExitCodeThread(startThread, &startExitCode);
            }
            CloseHandle(startThread);
            if (!processStillAlive(process_)) {
                if (localDll) {
                    FreeLibrary(localDll);
                }
                targetExited_ = true;
                setError(L"Target process exited during hook Start (likely crash)");
                return false;
            }
            if (startWait != WAIT_OBJECT_0) {
                Logger::instance().warning(L"gamecap", L"Start thread wait timed out; continuing");
            } else if (startExitCode == 0) {
                if (localDll) {
                    FreeLibrary(localDll);
                }
                const auto* header = static_cast<const gamecap::GameCaptureSharedHeader*>(view_);
                setError(header && header->errorText[0]
                    ? header->errorText
                    : L"Game capture hook startup failed");
                return false;
            }
            Logger::instance().info(L"gamecap", L"Called BacktrackGameCapture_Start in target");
        } else {
            Logger::instance().warning(
                L"gamecap",
                L"CreateRemoteThread(Start) failed: " +
                    hresultToString(HRESULT_FROM_WIN32(GetLastError())));
        }
    }
    if (localDll) {
        FreeLibrary(localDll);
    }

    if (!processStillAlive(process_)) {
        targetExited_ = true;
        setError(L"Target process exited after inject (likely crash/anti-cheat)");
        return false;
    }

    injected_ = true;
    targetImageName_ = imageName;
    Logger::instance().info(
        L"gamecap",
        L"Injected BacktrackGameCapture.dll into " + imageName + L" pid=" + std::to_wstring(pid));
    return true;
}

bool GameCaptureSource::ensurePool(uint32_t width, uint32_t height, DXGI_FORMAT format) {
    if (!pool_.empty()) {
        D3D11_TEXTURE2D_DESC desc{};
        pool_.front()->texture->GetDesc(&desc);
        if (desc.Width == width && desc.Height == height && desc.Format == format) {
            return true;
        }
    }
    pool_.clear();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    for (uint32_t i = 0; i < poolSize_; ++i) {
        auto slot = std::make_shared<TextureSlot>();
        HRESULT hr = device_->device()->CreateTexture2D(&desc, nullptr, &slot->texture);
        if (FAILED(hr)) {
            pool_.clear();
            setError(L"Game capture pool CreateTexture2D failed");
            return false;
        }
        pool_.push_back(std::move(slot));
    }
    return true;
}

bool GameCaptureSource::openSharedTexture(
    uint32_t sharedSlot,
    HANDLE remoteHandle,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    bool ntHandle) {
    if (!device_ || !process_ || !remoteHandle || sharedSlot >= gamecap::kGameCaptureMaxSharedTextures) {
        return false;
    }
    if (openSharedHandleValue_[sharedSlot] == reinterpret_cast<uint64_t>(remoteHandle) && sharedTexture_[sharedSlot]) {
        return true;
    }

    HANDLE local = nullptr;
    ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = E_FAIL;

    if (ntHandle) {
        // NT handle: DuplicateHandle then OpenSharedResource1.
        if (!DuplicateHandle(
                process_,
                remoteHandle,
                GetCurrentProcess(),
                &local,
                0,
                FALSE,
                DUPLICATE_SAME_ACCESS)) {
            Logger::instance().warning(
                L"gamecap",
                L"DuplicateHandle NT shared texture failed: " +
                    hresultToString(HRESULT_FROM_WIN32(GetLastError())));
            return false;
        }
        ComPtr<ID3D11Device1> device1;
        hr = device_->device()->QueryInterface(IID_PPV_ARGS(&device1));
        if (SUCCEEDED(hr) && device1) {
            hr = device1->OpenSharedResource1(local, IID_PPV_ARGS(&tex));
        }
        if (FAILED(hr) || !tex) {
            CloseHandle(local);
            Logger::instance().warning(L"gamecap", L"OpenSharedResource1 failed: " + hresultToString(hr));
            return false;
        }
    } else {
        if (!DuplicateHandle(
                process_,
                remoteHandle,
                GetCurrentProcess(),
                &local,
                0,
                FALSE,
                DUPLICATE_SAME_ACCESS)) {
            Logger::instance().warning(
                L"gamecap",
                L"DuplicateHandle shared texture failed: " +
                    hresultToString(HRESULT_FROM_WIN32(GetLastError())));
            return false;
        }
        hr = device_->device()->OpenSharedResource(local, IID_PPV_ARGS(&tex));
        if (FAILED(hr)) {
            CloseHandle(local);
            Logger::instance().warning(L"gamecap", L"OpenSharedResource failed: " + hresultToString(hr));
            return false;
        }
    }

    if (localSharedHandle_[sharedSlot]) {
        CloseHandle(localSharedHandle_[sharedSlot]);
    }
    ComPtr<IDXGIKeyedMutex> mutex;
    hr = tex.As(&mutex);
    if (FAILED(hr) || !mutex) {
        CloseHandle(local);
        Logger::instance().warning(L"gamecap", L"Shared texture has no keyed mutex: " + hresultToString(hr));
        return false;
    }
    localSharedHandle_[sharedSlot] = local;
    sharedTexture_[sharedSlot] = tex;
    sharedMutex_[sharedSlot] = mutex;
    openSharedHandleValue_[sharedSlot] = reinterpret_cast<uint64_t>(remoteHandle);
    width_ = width;
    height_ = height;
    format_ = format;
    return ensurePool(width, height, format);
}

bool GameCaptureSource::ensureDataMapping(const gamecap::GameCaptureSharedHeader* header) {
    // Producer publishes layout fields first, then bumps dataMappingGeneration
    // last. Read the generation up front; if it matches our open view, reuse it.
    const long generation = InterlockedCompareExchange(
        const_cast<volatile long*>(&header->dataMappingGeneration), 0, 0);
    if (generation == 0) {
        return false; // producer has not created a data mapping yet
    }
    if (dataMapping_ && dataView_ && generation == dataMappingGeneration_) {
        return true;
    }

    // Layout snapshot. Producer wrote these before bumping the generation.
    const uint32_t bufferCount = header->dataBufferCount;
    const uint32_t stride = header->dataStride;
    const uint32_t bufferBytes = header->dataBufferBytes;
    const uint32_t mappingBytes = header->dataMappingBytes;
    const uint32_t width = header->dataWidth;
    const uint32_t height = header->dataHeight;
    const DXGI_FORMAT format = static_cast<DXGI_FORMAT>(
        header->dataFormat ? header->dataFormat : DXGI_FORMAT_B8G8R8A8_UNORM);
    if (bufferCount == 0 || bufferCount > gamecap::kGameCaptureMaxDataBuffers ||
        stride == 0 || bufferBytes == 0 || mappingBytes == 0 ||
        width == 0 || height == 0 || bufferBytes > mappingBytes ||
        static_cast<uint64_t>(bufferCount) * bufferBytes > mappingBytes) {
        return false; // producer layout not fully populated / inconsistent
    }

    // Release any stale view before opening the fresh generation.
    if (dataView_) {
        UnmapViewOfFile(dataView_);
        dataView_ = nullptr;
    }
    if (dataMapping_) {
        CloseHandle(dataMapping_);
        dataMapping_ = nullptr;
    }

    wchar_t name[64] = {};
    gamecap::gameCaptureDataMappingName(name, static_cast<uint32_t>(generation));
    // Producer owns creation; host only opens. OpenFileMapping avoids racing a
    // create against a producer that may still be sizing the section.
    dataMapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!dataMapping_) {
        // Not yet visible; try again on a later frame without erroring out.
        return false;
    }
    dataView_ = MapViewOfFile(dataMapping_, FILE_MAP_READ, 0, 0, mappingBytes);
    if (!dataView_) {
        CloseHandle(dataMapping_);
        dataMapping_ = nullptr;
        return false;
    }

    if (!ensurePool(width, height, format)) {
        return false;
    }
    width_ = width;
    height_ = height;
    format_ = format;
    dataBufferCount_ = bufferCount;
    dataStride_ = stride;
    dataBufferBytes_ = bufferBytes;
    dataMappingBytes_ = mappingBytes;
    dataMappingGeneration_ = generation;
    Logger::instance().info(
        L"gamecap",
        L"Game capture shmem mapping gen=" + std::to_wstring(generation) + L" " +
            std::to_wstring(width) + L"x" + std::to_wstring(height) +
            L" stride=" + std::to_wstring(stride) +
            L" buffers=" + std::to_wstring(bufferCount));
    return true;
}

bool GameCaptureSource::acquireNextFrameShmem(GpuFrame& frame, uint32_t timeoutMs) {
    auto* header = static_cast<gamecap::GameCaptureSharedHeader*>(view_);

    if (!pendingInitialFrame_ && header->sequence == lastSequence_) {
        if (timeoutMs > 0 && frameEvent_) {
            WaitForSingleObject(frameEvent_, timeoutMs);
        }
        if (header->sequence == lastSequence_) {
            return false;
        }
    }

    if (!ensureDataMapping(header)) {
        return false;
    }

    // Seqlock read: sample epoch (even), read buffer index + descriptor, then
    // re-sample. Retry a bounded number of times if the producer was mid-write.
    uint32_t bufferIndex = 0;
    gamecap::GameCaptureFrameDescriptor descriptor{};
    bool captured = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const long epochBefore = InterlockedCompareExchange(&header->publishEpoch, 0, 0);
        if (epochBefore & 1) continue;
        const long idx = InterlockedCompareExchange(&header->latestDataBuffer, 0, 0);
        const uint32_t slot = header->latestReadySlot;
        if (slot < gamecap::kGameCaptureMaxSharedTextures) {
            descriptor = header->frames[slot];
        }
        const long epochAfter = InterlockedCompareExchange(&header->publishEpoch, 0, 0);
        if (epochBefore == epochAfter && !(epochAfter & 1) &&
            idx >= 0 && static_cast<uint32_t>(idx) < dataBufferCount_) {
            bufferIndex = static_cast<uint32_t>(idx);
            captured = true;
            break;
        }
    }
    if (!captured) {
        return false;
    }

    auto slot = acquireSlot();
    if (!slot) {
        return false;
    }

    const auto* base = static_cast<const uint8_t*>(dataView_) +
                       static_cast<size_t>(bufferIndex) * dataBufferBytes_;
    D3D11_BOX box{};
    box.left = 0;
    box.top = 0;
    box.front = 0;
    box.right = width_;
    box.bottom = height_;
    box.back = 1;
    LARGE_INTEGER copyStart{};
    QueryPerformanceCounter(&copyStart);
    {
        std::scoped_lock lock(device_->immediateContextMutex());
        device_->context()->UpdateSubresource(
            slot->texture.Get(), 0, &box, base, dataStride_, dataBufferBytes_);
    }
    LARGE_INTEGER copyEnd{};
    QueryPerformanceCounter(&copyEnd);
    InterlockedAdd64(&header->hostCopyQpc, copyEnd.QuadPart - copyStart.QuadPart);

    lastSequence_ = header->sequence;
    pendingInitialFrame_ = false;
    frame.texture = slot->texture;
    frame.lease = slot;
    frame.frameIndex = frameIndex_++;
    frame.width = width_;
    frame.height = height_;
    frame.format = format_;
    frame.pts100ns = qpcTo100ns(descriptor.presentQpc);
    if (frame.pts100ns == 0) {
        frame.pts100ns = static_cast<int64_t>(frame.frameIndex * (kHundredNanosecondsPerSecond / 60));
    }
    return true;
}

std::shared_ptr<GameCaptureSource::TextureSlot> GameCaptureSource::acquireSlot() {
    for (auto& slot : pool_) {
        if (slot.use_count() == 1) {
            return slot;
        }
    }
    return {};
}

bool GameCaptureSource::initialize(D3DDevice& device, const AppSettings& settings, const CaptureTarget& target) {
    shutdown();
    device_ = &device;
    deviceLost_ = false;
    targetExited_ = false;
    antiCheatBlocked_ = false;
    antiCheatKind_ = AntiCheatKind::None;
    frameIndex_ = 0;
    lastSequence_ = 0;
    pendingInitialFrame_ = false;
    poolSize_ = std::clamp<uint32_t>(settings.gpu.frameQueueLimit + 4, 5, 16);
    allowAntiCheatInject_ = settings.allowAntiCheatGameCapture;
    hostPid_ = GetCurrentProcessId();

    // Per-process cool-down avoids blocking another game after one target exits.
    constexpr auto kInjectCooldown = std::chrono::seconds(60);
    const auto now = std::chrono::steady_clock::now();

    const HMONITOR monitor = target.monitor ? target.monitor : monitorFromIndex(target.monitorIndex);
    HWND hwnd = target.window;
    if (!hwnd || !IsWindow(hwnd)) {
        hwnd = findCoveringWindow(monitor);
    }
    targetPid_ = pidFromHwnd(hwnd);
    if (targetPid_ == 0 || targetPid_ == hostPid_) {
        setError(L"Game capture: no target process (focus a game window)");
        return false;
    }
    {
        std::scoped_lock lock(g_targetExitMutex);
        const auto it = g_lastTargetExitAt.find(targetPid_);
        if (it != g_lastTargetExitAt.end() && now - it->second < kInjectCooldown) {
            setError(L"Game capture inject cool-down after target exit (wait 60s)");
            return false;
        }
    }

    if (!openSharedMapping(hostPid_)) {
        shutdown();
        return false;
    }

    auto* header = static_cast<gamecap::GameCaptureSharedHeader*>(view_);
    header->targetPid = targetPid_;
    header->targetWindow = reinterpret_cast<uint64_t>(hwnd);
    header->captureFps = std::max<uint32_t>(1, settings.video.fps);
    header->state = gamecap::GameCaptureSlotState::Idle;

    if (!injectIntoProcess(targetPid_)) {
        if (targetExited_ || (process_ && !processStillAlive(process_))) {
            std::scoped_lock lock(g_targetExitMutex);
            g_lastTargetExitAt[targetPid_] = std::chrono::steady_clock::now();
            targetExited_ = true;
        }
        shutdown();
        return false;
    }

    // Recovery runs on capture thread. Bound an unsupported hook probe so a
    // failed DLL handoff returns to the existing source recovery path quickly.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    std::wstring lastHookNote;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!processStillAlive(process_)) {
            targetExited_ = true;
            std::scoped_lock lock(g_targetExitMutex);
            g_lastTargetExitAt[targetPid_] = std::chrono::steady_clock::now();
            setError(L"Target process exited while waiting for Present frames (likely crash/anti-cheat)");
            shutdown();
            return false;
        }
        if (header->state == gamecap::GameCaptureSlotState::Error) {
            std::wstring detail = header->errorText[0] ? header->errorText : L"Game capture hook reported error";
            if (header->lastExceptionCode != 0) {
                detail += L" seh=0x";
                wchar_t hex[16] = {};
                swprintf_s(hex, L"%08X", header->lastExceptionCode);
                detail += hex;
            }
            setError(detail.c_str());
            shutdown();
            return false;
        }
        if (header->errorText[0] && lastHookNote != header->errorText) {
            lastHookNote = header->errorText;
            Logger::instance().info(L"gamecap", L"Hook status: " + lastHookNote);
        }
        if (header->sequence != 0 && header->sharedHandle != 0 &&
            header->width > 0 && header->height > 0) {
            const bool ntHandle = (header->flags & gamecap::kGameCapFlagNtHandle) != 0;
            if (openSharedTexture(
                    header->latestReadySlot,
                    reinterpret_cast<HANDLE>(static_cast<uintptr_t>(header->sharedHandle)),
                    header->width,
                    header->height,
                    static_cast<DXGI_FORMAT>(
                        header->format ? header->format : DXGI_FORMAT_B8G8R8A8_UNORM),
                    ntHandle)) {
                // Do not mark this frame consumed. The producer released its
                // keyed mutex with key 1, which acquireNextFrame must return
                // to key 0 before another frame can be published.
                pendingInitialFrame_ = true;
                Logger::instance().info(
                    L"gamecap",
                    L"Game capture live " + std::to_wstring(width_) + L"x" + std::to_wstring(height_) +
                        L" pid=" + std::to_wstring(targetPid_) + L" " + targetImageName_ +
                        L" frames=" + std::to_wstring(header->framesPublished) +
                        L" installMs=" + std::to_wstring(header->hookInstallMs));
                return true;
            }
        }
        WaitForSingleObject(frameEvent_, 50);
    }

    std::wstring detail = L"Game capture: no Present frames after inject";
    if (header->errorText[0]) {
        detail += L" — ";
        detail += header->errorText;
    } else if (header->graphicsApi == static_cast<uint32_t>(gamecap::GameCaptureGraphicsApi::D3D12)) {
        detail += L" (D3D12 backbuffer; need D3D12 path)";
    } else {
        detail += L" (D3D12/OpenGL/Vulkan need extra hooks; try borderless or restart game with Backtrack running)";
    }
    if (header->flags != 0) {
        detail += L" flags=0x";
        wchar_t hex[16] = {};
        swprintf_s(hex, L"%X", header->flags);
        detail += hex;
    }
    Logger::instance().warning(
        L"gamecap",
        detail + L" pid=" + std::to_wstring(targetPid_) +
            L" published=" + std::to_wstring(header->framesPublished) +
            L" installMs=" + std::to_wstring(header->hookInstallMs));
    setError(detail.c_str());
    shutdown();
    return false;
}

bool GameCaptureSource::acquireNextFrame(GpuFrame& frame, uint32_t timeoutMs) {
    if (deviceLost_ || !view_) {
        return false;
    }
    if (process_ && !processStillAlive(process_)) {
        targetExited_ = true;
        setError(L"Target process exited during capture");
        return false;
    }
    auto* header = static_cast<gamecap::GameCaptureSharedHeader*>(view_);
    if (header->state == gamecap::GameCaptureSlotState::Error) {
        setError(header->errorText[0] ? header->errorText : L"Game capture hook error");
        return false;
    }
    if (header->state == gamecap::GameCaptureSlotState::Stopping) {
        deviceLost_ = true;
        return false;
    }

    // Producer may switch to CPU-copy transport when it cannot publish a
    // shareable GPU texture. Dispatch on the currently-advertised mode.
    if (header->transportMode == static_cast<uint32_t>(gamecap::GameCaptureTransport::SharedMemory)) {
        return acquireNextFrameShmem(frame, timeoutMs);
    }

    if (!pendingInitialFrame_ && header->sequence == lastSequence_) {
        if (timeoutMs > 0 && frameEvent_) {
            WaitForSingleObject(frameEvent_, timeoutMs);
        }
        if (header->sequence == lastSequence_) {
            return false;
        }
    }

    const long epochBefore = InterlockedCompareExchange(&header->publishEpoch, 0, 0);
    if (epochBefore & 1) return false;
    const uint32_t sharedSlot = header->latestReadySlot;
    if (sharedSlot >= gamecap::kGameCaptureMaxSharedTextures) return false;
    const auto descriptor = header->frames[sharedSlot];
    const long epochAfter = InterlockedCompareExchange(&header->publishEpoch, 0, 0);
    if (epochBefore != epochAfter || (epochAfter & 1)) return false;
    if (descriptor.sharedHandle != openSharedHandleValue_[sharedSlot] ||
        descriptor.width != width_ || descriptor.height != height_) {
        const bool ntHandle = (descriptor.flags & gamecap::kGameCapFlagNtHandle) != 0;
        if (!openSharedTexture(
                sharedSlot,
                reinterpret_cast<HANDLE>(static_cast<uintptr_t>(descriptor.sharedHandle)),
                descriptor.width,
                descriptor.height,
                static_cast<DXGI_FORMAT>(descriptor.format),
                ntHandle)) {
            return false;
        }
    }

    auto slot = acquireSlot();
    if (!slot || !sharedMutex_[sharedSlot]) {
        return false;
    }

    // WAIT_TIMEOUT is not FAILED(hr), but does not grant ownership of key 1.
    if (sharedMutex_[sharedSlot]->AcquireSync(1, 0) != S_OK) {
        InterlockedIncrement(&header->hostKeyedMutexMisses);
        InterlockedIncrement(&header->droppedByContention);
        return false;
    }
    LARGE_INTEGER copyStart{};
    QueryPerformanceCounter(&copyStart);
    {
        std::scoped_lock lock(device_->immediateContextMutex());
        device_->context()->CopyResource(slot->texture.Get(), sharedTexture_[sharedSlot].Get());
    }
    LARGE_INTEGER copyEnd{};
    QueryPerformanceCounter(&copyEnd);
    InterlockedAdd64(&header->hostCopyQpc, copyEnd.QuadPart - copyStart.QuadPart);
    const HRESULT releaseHr = sharedMutex_[sharedSlot]->ReleaseSync(0);
    if (FAILED(releaseHr)) {
        setError(L"Game capture shared texture release failed");
        return false;
    }

    lastSequence_ = header->sequence;
    pendingInitialFrame_ = false;
    frame.texture = slot->texture;
    frame.lease = slot;
    frame.frameIndex = frameIndex_++;
    frame.width = width_;
    frame.height = height_;
    frame.format = format_;
    frame.pts100ns = qpcTo100ns(descriptor.presentQpc);
    if (frame.pts100ns == 0) {
        frame.pts100ns = static_cast<int64_t>(frame.frameIndex * (kHundredNanosecondsPerSecond / 60));
    }
    return true;
}

bool GameCaptureSource::isDeviceLost() const {
    if (deviceLost_ || targetExited_) {
        return true;
    }
    if (process_) {
        DWORD code = 0;
        if (GetExitCodeProcess(process_, &code) && code != STILL_ACTIVE) {
            return true;
        }
    }
    if (view_) {
        const auto* header = static_cast<const gamecap::GameCaptureSharedHeader*>(view_);
        if (header->state == gamecap::GameCaptureSlotState::Error ||
            header->state == gamecap::GameCaptureSlotState::Stopping) {
            return true;
        }
    }
    return false;
}

bool GameCaptureSource::targetProcessExited() const {
    if (targetExited_) {
        return true;
    }
    if (process_) {
        DWORD code = 0;
        if (GetExitCodeProcess(process_, &code) && code != STILL_ACTIVE) {
            return true;
        }
    }
    return false;
}

void GameCaptureSource::shutdown() {
    if (view_) {
        auto* header = static_cast<gamecap::GameCaptureSharedHeader*>(view_);
        header->state = gamecap::GameCaptureSlotState::Stopping;
    }
    // Ask injected code to stop publishing before host releases IPC handles.
    // DLL stays loaded until target exit; unloading under loader lock is unsafe.
    if (process_ && injected_) {
        const std::wstring dllPath =
            (std::filesystem::path(moduleDirectory()) / gamecap::kGameCaptureDllFileName).wstring();
        HMODULE localDll = LoadLibraryW(dllPath.c_str());
        const FARPROC localStop = localDll ? GetProcAddress(localDll, "BacktrackGameCapture_Stop") : nullptr;
        const HMODULE remoteDll = localStop ? remoteModuleBase(process_, dllPath) : nullptr;
        if (remoteDll && localDll && localStop) {
            const auto remoteStop = reinterpret_cast<LPTHREAD_START_ROUTINE>(
                reinterpret_cast<uint8_t*>(remoteDll) +
                (reinterpret_cast<const uint8_t*>(localStop) - reinterpret_cast<const uint8_t*>(localDll)));
            HANDLE stopThread = CreateRemoteThread(process_, nullptr, 0, remoteStop, nullptr, 0, nullptr);
            // Stop only clears the hook's active flag. Do not hold an F11 return
            // to desktop capture for a second while waiting for target scheduling.
            if (stopThread) { WaitForSingleObject(stopThread, 50); CloseHandle(stopThread); }
        }
        if (localDll) FreeLibrary(localDll);
    }
    for (uint32_t i = 0; i < gamecap::kGameCaptureMaxSharedTextures; ++i) {
        sharedTexture_[i].Reset();
        sharedMutex_[i].Reset();
        if (localSharedHandle_[i]) CloseHandle(localSharedHandle_[i]);
        localSharedHandle_[i] = nullptr;
        openSharedHandleValue_[i] = 0;
    }
    pool_.clear();
    if (dataView_) {
        UnmapViewOfFile(dataView_);
        dataView_ = nullptr;
    }
    if (dataMapping_) {
        CloseHandle(dataMapping_);
        dataMapping_ = nullptr;
    }
    dataMappingGeneration_ = 0;
    dataBufferCount_ = 0;
    dataStride_ = 0;
    dataBufferBytes_ = 0;
    dataMappingBytes_ = 0;
    if (frameEvent_) {
        CloseHandle(frameEvent_);
        frameEvent_ = nullptr;
    }
    if (view_) {
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (mapping_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
    if (remoteThread_) {
        CloseHandle(remoteThread_);
        remoteThread_ = nullptr;
    }
    if (process_) {
        CloseHandle(process_);
        process_ = nullptr;
    }
    device_ = nullptr;
    injected_ = false;
    deviceLost_ = true;
    pendingInitialFrame_ = false;
    width_ = 0;
    height_ = 0;
    targetImageName_.clear();
}

} // namespace backtrack
