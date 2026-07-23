#include "audio/WasapiCapture.h"

#include "core/Logger.h"
#include "platform/Win32Util.h"

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <cwchar>
#include <map>
#include <memory>
#include <mutex>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace backtrack {

namespace {

constexpr PROPERTYKEY kPkeyDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    14};
constexpr int64_t kAudioTimestampResyncThreshold100ns = kHundredNanosecondsPerSecond / 20;

WaveFormatBlob copyWaveFormat(WAVEFORMATEX* format) {
    WaveFormatBlob blob;
    if (!format) {
        return blob;
    }
    const auto size = sizeof(WAVEFORMATEX) + format->cbSize;
    const auto* begin = reinterpret_cast<const uint8_t*>(format);
    blob.bytes.assign(begin, begin + size);
    return blob;
}

std::wstring trimText(std::wstring value) {
    while (!value.empty() && iswspace(value.front())) {
        value.erase(value.begin());
    }
    while (!value.empty() && iswspace(value.back())) {
        value.pop_back();
    }
    return value;
}

const wchar_t* trackName(AudioTrack track) {
    switch (track) {
    case AudioTrack::System:
        return L"system audio";
    case AudioTrack::Microphone:
        return L"microphone";
    case AudioTrack::SoundSeparation:
        return L"sound separation";
    }
    return L"audio";
}

int64_t steadyNow100ns() {
    return std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int64_t qpcNow100ns() {
    LARGE_INTEGER frequency{};
    LARGE_INTEGER counter{};
    if (!QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&counter) ||
        frequency.QuadPart <= 0) {
        return 0;
    }
    return static_cast<int64_t>(
        (static_cast<long double>(counter.QuadPart) * kHundredNanosecondsPerSecond) /
        static_cast<long double>(frequency.QuadPart));
}

EDataFlow dataFlowFor(AudioTrack track) {
    return track == AudioTrack::Microphone ? eCapture : eRender;
}

std::wstring propertyString(IPropertyStore* store, const PROPERTYKEY& key) {
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring text;
    if (SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
        text = value.pwszVal;
    }
    PropVariantClear(&value);
    return text;
}

class ScopedComInitialization {
public:
    explicit ScopedComInitialization(DWORD apartmentModel)
        : result_(CoInitializeEx(nullptr, apartmentModel)),
          initialized_(SUCCEEDED(result_)) {
    }

    ~ScopedComInitialization() {
        if (initialized_) {
            CoUninitialize();
        }
    }

    HRESULT result() const {
        return result_;
    }

private:
    HRESULT result_ = E_FAIL;
    bool initialized_ = false;
};

class ScopedMmcssTask {
public:
    ScopedMmcssTask(const wchar_t* taskName, AVRT_PRIORITY priority) {
        DWORD taskIndex = 0;
        handle_ = AvSetMmThreadCharacteristicsW(taskName, &taskIndex);
        if (handle_) {
            AvSetMmThreadPriority(handle_, priority);
        }
    }

    ~ScopedMmcssTask() {
        if (handle_) {
            AvRevertMmThreadCharacteristics(handle_);
        }
    }

private:
    HANDLE handle_ = nullptr;
};

struct WaveFormatDeleter {
    void operator()(WAVEFORMATEX* format) const {
        CoTaskMemFree(format);
    }
};

std::wstring normalizedPathKey(const std::filesystem::path& path) {
    std::wstring value = path.wstring();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return value;
}

std::filesystem::path processImagePath(DWORD processId) {
    if (processId == 0) {
        return {};
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) {
        return {};
    }

    std::wstring buffer(MAX_PATH, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size());
    while (!QueryFullProcessImageNameW(process, 0, buffer.data(), &size)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            CloseHandle(process);
            return {};
        }
        buffer.resize(buffer.size() * 2);
        size = static_cast<DWORD>(buffer.size());
    }
    CloseHandle(process);
    buffer.resize(size);
    return buffer;
}

std::wstring sessionDisplayName(IAudioSessionControl* session) {
    if (!session) {
        return {};
    }
    LPWSTR rawName = nullptr;
    std::wstring name;
    if (SUCCEEDED(session->GetDisplayName(&rawName)) && rawName) {
        name = rawName;
    }
    CoTaskMemFree(rawName);
    return name;
}

class AudioActivationCompletionHandler final : public IActivateAudioInterfaceCompletionHandler {
public:
    explicit AudioActivationCompletionHandler(HANDLE event)
        : event_(event) {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) {
            return E_POINTER;
        }
        *object = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *object = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refCount_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = --refCount_;
        if (count == 0) {
            delete this;
        }
        return count;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        if (operation) {
            Microsoft::WRL::ComPtr<IUnknown> activated;
            HRESULT result = E_FAIL;
            const HRESULT hr = operation->GetActivateResult(&result, &activated);
            {
                std::scoped_lock lock(mutex_);
                activateResult_ = SUCCEEDED(hr) ? result : hr;
                activated_ = activated;
            }
        }
        if (event_) {
            SetEvent(event_);
        }
        return S_OK;
    }

    HRESULT audioClient(IAudioClient** client) {
        if (!client) {
            return E_POINTER;
        }
        *client = nullptr;
        std::scoped_lock lock(mutex_);
        if (FAILED(activateResult_)) {
            return activateResult_;
        }
        if (!activated_) {
            return E_NOINTERFACE;
        }
        Microsoft::WRL::ComPtr<IAudioClient> audioClient;
        const HRESULT hr = activated_.As(&audioClient);
        if (SUCCEEDED(hr)) {
            *client = audioClient.Detach();
        }
        return hr;
    }

private:
    std::atomic<ULONG> refCount_{1};
    HANDLE event_ = nullptr;
    std::mutex mutex_;
    HRESULT activateResult_ = E_FAIL;
    Microsoft::WRL::ComPtr<IUnknown> activated_;
};

PROCESS_LOOPBACK_MODE nativeProcessLoopbackMode(ProcessLoopbackMode mode) {
    return mode == ProcessLoopbackMode::ExcludeTargetProcessTree
        ? PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE
        : PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
}

HRESULT activateProcessLoopbackAudioClient(DWORD processId, ProcessLoopbackMode mode, IAudioClient** audioClient) {
    if (!audioClient) {
        return E_POINTER;
    }
    *audioClient = nullptr;
    if (processId == 0) {
        return E_INVALIDARG;
    }

    HANDLE completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!completed) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    AUDIOCLIENT_ACTIVATION_PARAMS activationParams{};
    activationParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    activationParams.ProcessLoopbackParams.TargetProcessId = processId;
    activationParams.ProcessLoopbackParams.ProcessLoopbackMode = nativeProcessLoopbackMode(mode);

    PROPVARIANT variant;
    PropVariantInit(&variant);
    variant.vt = VT_BLOB;
    variant.blob.cbSize = sizeof(activationParams);
    variant.blob.pBlobData = reinterpret_cast<BYTE*>(&activationParams);

    auto* rawHandler = new AudioActivationCompletionHandler(completed);
    Microsoft::WRL::ComPtr<IActivateAudioInterfaceCompletionHandler> handler;
    handler.Attach(rawHandler);
    Microsoft::WRL::ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    HRESULT hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient),
        &variant,
        handler.Get(),
        &operation);

    if (SUCCEEDED(hr)) {
        const DWORD wait = WaitForSingleObject(completed, 10000);
        if (wait == WAIT_OBJECT_0) {
            hr = rawHandler->audioClient(audioClient);
        } else {
            hr = wait == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(WAIT_TIMEOUT) : HRESULT_FROM_WIN32(GetLastError());
            handler.Detach();
        }
    }

    CloseHandle(completed);
    return hr;
}

} // namespace

WasapiCapture::~WasapiCapture() {
    stop();
}

std::vector<AudioDeviceInfo> WasapiCapture::enumerateDevices(AudioTrack track) {
    std::vector<AudioDeviceInfo> devices;

    ScopedComInitialization com(COINIT_MULTITHREADED);
    HRESULT hr = com.result();

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        return devices;
    }

    Microsoft::WRL::ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(dataFlowFor(track), DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        return devices;
    }

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT index = 0; index < count; ++index) {
        Microsoft::WRL::ComPtr<IMMDevice> endpoint;
        if (FAILED(collection->Item(index, &endpoint))) {
            continue;
        }

        LPWSTR id = nullptr;
        if (FAILED(endpoint->GetId(&id)) || !id) {
            continue;
        }

        Microsoft::WRL::ComPtr<IPropertyStore> properties;
        std::wstring name;
        if (SUCCEEDED(endpoint->OpenPropertyStore(STGM_READ, &properties))) {
            name = propertyString(properties.Get(), kPkeyDeviceFriendlyName);
        }
        if (name.empty()) {
            name = id;
        }

        devices.push_back(AudioDeviceInfo{id, name});
        CoTaskMemFree(id);
    }

    return devices;
}

std::vector<AudioSessionAppInfo> WasapiCapture::enumerateAudioSessionApps() {
    std::map<std::wstring, AudioSessionAppInfo> appsByPath;

    ScopedComInitialization com(COINIT_MULTITHREADED);
    HRESULT hr = com.result();

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        return {};
    }

    Microsoft::WRL::ComPtr<IMMDeviceCollection> endpoints;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &endpoints);
    if (FAILED(hr)) {
        return {};
    }

    const DWORD currentProcessId = GetCurrentProcessId();
    UINT endpointCount = 0;
    endpoints->GetCount(&endpointCount);
    for (UINT endpointIndex = 0; endpointIndex < endpointCount; ++endpointIndex) {
        Microsoft::WRL::ComPtr<IMMDevice> endpoint;
        if (FAILED(endpoints->Item(endpointIndex, &endpoint)) || !endpoint) {
            continue;
        }

        Microsoft::WRL::ComPtr<IAudioSessionManager2> sessionManager;
        hr = endpoint->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &sessionManager);
        if (FAILED(hr) || !sessionManager) {
            continue;
        }

        Microsoft::WRL::ComPtr<IAudioSessionEnumerator> sessions;
        hr = sessionManager->GetSessionEnumerator(&sessions);
        if (FAILED(hr) || !sessions) {
            continue;
        }

        int sessionCount = 0;
        sessions->GetCount(&sessionCount);
        for (int sessionIndex = 0; sessionIndex < sessionCount; ++sessionIndex) {
            Microsoft::WRL::ComPtr<IAudioSessionControl> session;
            if (FAILED(sessions->GetSession(sessionIndex, &session)) || !session) {
                continue;
            }

            AudioSessionState state = AudioSessionStateInactive;
            if (FAILED(session->GetState(&state)) || state != AudioSessionStateActive) {
                continue;
            }

            Microsoft::WRL::ComPtr<IAudioSessionControl2> session2;
            if (FAILED(session.As(&session2)) || !session2) {
                continue;
            }

            if (session2->IsSystemSoundsSession() == S_OK) {
                continue;
            }

            DWORD processId = 0;
            if (FAILED(session2->GetProcessId(&processId)) ||
                processId == 0 ||
                processId == currentProcessId) {
                continue;
            }

            const std::filesystem::path imagePath = processImagePath(processId);
            if (imagePath.empty()) {
                continue;
            }

            const std::wstring key = normalizedPathKey(imagePath);
            if (key.empty() || appsByPath.contains(key)) {
                continue;
            }

            std::wstring name = trimText(sessionDisplayName(session.Get()));
            if (name.empty()) {
                name = imagePath.stem().wstring();
            }

            appsByPath.emplace(key, AudioSessionAppInfo{name, imagePath, processId});
        }
    }

    std::vector<AudioSessionAppInfo> apps;
    apps.reserve(appsByPath.size());
    for (auto& [_, app] : appsByPath) {
        apps.push_back(std::move(app));
    }
    std::sort(apps.begin(), apps.end(), [](const AudioSessionAppInfo& left, const AudioSessionAppInfo& right) {
        return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
    });
    return apps;
}

namespace {

struct SessionMuteEntry {
    DWORD processId = 0;
    std::wstring pathKey;
    bool previousMute = false;
    Microsoft::WRL::ComPtr<ISimpleAudioVolume> volume;
};

std::mutex g_sessionMuteMutex;
std::vector<SessionMuteEntry> g_sessionMutes;

void restoreSessionMutesLocked() {
    for (auto& entry : g_sessionMutes) {
        if (entry.volume) {
            entry.volume->SetMute(entry.previousMute ? TRUE : FALSE, nullptr);
        }
    }
    g_sessionMutes.clear();
}

} // namespace

uint32_t WasapiCapture::applySessionMutesForExecutables(const std::vector<std::wstring>& mutedExecutableKeys) {
    std::scoped_lock lock(g_sessionMuteMutex);
    if (mutedExecutableKeys.empty()) {
        restoreSessionMutesLocked();
        return 0;
    }

    std::unordered_set<std::wstring> mutedKeys(
        mutedExecutableKeys.begin(), mutedExecutableKeys.end());

    ScopedComInitialization com(COINIT_MULTITHREADED);
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr) || !enumerator) {
        return 0;
    }

    Microsoft::WRL::ComPtr<IMMDeviceCollection> endpoints;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &endpoints);
    if (FAILED(hr) || !endpoints) {
        return 0;
    }

    // Drop stale mute entries for processes that exited or are no longer requested.
    for (auto it = g_sessionMutes.begin(); it != g_sessionMutes.end();) {
        const bool stillWanted = mutedKeys.contains(it->pathKey);
        HANDLE process = stillWanted ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, it->processId) : nullptr;
        const bool alive = process != nullptr;
        if (process) {
            CloseHandle(process);
        }
        if (!stillWanted || !alive || !it->volume) {
            if (it->volume) {
                it->volume->SetMute(it->previousMute ? TRUE : FALSE, nullptr);
            }
            it = g_sessionMutes.erase(it);
        } else {
            ++it;
        }
    }

    std::unordered_set<DWORD> alreadyMuted;
    for (const auto& entry : g_sessionMutes) {
        alreadyMuted.insert(entry.processId);
    }

    const DWORD currentProcessId = GetCurrentProcessId();
    UINT endpointCount = 0;
    endpoints->GetCount(&endpointCount);
    for (UINT endpointIndex = 0; endpointIndex < endpointCount; ++endpointIndex) {
        Microsoft::WRL::ComPtr<IMMDevice> endpoint;
        if (FAILED(endpoints->Item(endpointIndex, &endpoint)) || !endpoint) {
            continue;
        }

        Microsoft::WRL::ComPtr<IAudioSessionManager2> sessionManager;
        hr = endpoint->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &sessionManager);
        if (FAILED(hr) || !sessionManager) {
            continue;
        }

        Microsoft::WRL::ComPtr<IAudioSessionEnumerator> sessions;
        hr = sessionManager->GetSessionEnumerator(&sessions);
        if (FAILED(hr) || !sessions) {
            continue;
        }

        int sessionCount = 0;
        sessions->GetCount(&sessionCount);
        for (int sessionIndex = 0; sessionIndex < sessionCount; ++sessionIndex) {
            Microsoft::WRL::ComPtr<IAudioSessionControl> session;
            if (FAILED(sessions->GetSession(sessionIndex, &session)) || !session) {
                continue;
            }

            Microsoft::WRL::ComPtr<IAudioSessionControl2> session2;
            if (FAILED(session.As(&session2)) || !session2) {
                continue;
            }
            if (session2->IsSystemSoundsSession() == S_OK) {
                continue;
            }

            DWORD processId = 0;
            if (FAILED(session2->GetProcessId(&processId)) ||
                processId == 0 ||
                processId == currentProcessId ||
                alreadyMuted.contains(processId)) {
                continue;
            }

            const std::filesystem::path imagePath = processImagePath(processId);
            const std::wstring key = normalizedPathKey(imagePath);
            if (key.empty() || !mutedKeys.contains(key)) {
                continue;
            }

            Microsoft::WRL::ComPtr<ISimpleAudioVolume> volume;
            if (FAILED(session.As(&volume)) || !volume) {
                continue;
            }

            BOOL previousMute = FALSE;
            volume->GetMute(&previousMute);
            if (FAILED(volume->SetMute(TRUE, nullptr))) {
                continue;
            }

            SessionMuteEntry entry;
            entry.processId = processId;
            entry.pathKey = key;
            entry.previousMute = previousMute == TRUE;
            entry.volume = std::move(volume);
            g_sessionMutes.push_back(std::move(entry));
            alreadyMuted.insert(processId);
        }
    }

    return static_cast<uint32_t>(g_sessionMutes.size());
}

void WasapiCapture::clearSessionMutes() {
    std::scoped_lock lock(g_sessionMuteMutex);
    restoreSessionMutesLocked();
}

std::vector<AudioSessionAppInfo> WasapiCapture::enumerateOpenApps() {
    std::map<std::wstring, AudioSessionAppInfo> appsByPath;
    const DWORD currentProcessId = GetCurrentProcessId();

    struct EnumState {
        std::map<std::wstring, AudioSessionAppInfo>* apps = nullptr;
        DWORD currentProcessId = 0;
    } state{&appsByPath, currentProcessId};

    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL {
            auto* state = reinterpret_cast<EnumState*>(param);
            if (!state || !state->apps || !IsWindowVisible(window)) {
                return TRUE;
            }
            if (GetAncestor(window, GA_ROOT) != window) {
                return TRUE;
            }
            const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
            if ((exStyle & WS_EX_TOOLWINDOW) != 0) {
                return TRUE;
            }
            const int titleLength = GetWindowTextLengthW(window);
            if (titleLength <= 0) {
                return TRUE;
            }

            DWORD processId = 0;
            GetWindowThreadProcessId(window, &processId);
            if (processId == 0 || processId == state->currentProcessId) {
                return TRUE;
            }

            const std::filesystem::path imagePath = processImagePath(processId);
            if (imagePath.empty()) {
                return TRUE;
            }

            const std::wstring key = normalizedPathKey(imagePath);
            if (key.empty() || state->apps->contains(key)) {
                return TRUE;
            }

            std::wstring title(static_cast<size_t>(titleLength + 1), L'\0');
            GetWindowTextW(window, title.data(), titleLength + 1);
            title.resize(static_cast<size_t>(titleLength));
            title = trimText(std::move(title));
            std::wstring name = imagePath.stem().wstring();
            if (name.empty()) {
                name = title.empty() ? imagePath.filename().wstring() : title;
            }

            state->apps->emplace(key, AudioSessionAppInfo{name, imagePath, processId});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&state));

    std::vector<AudioSessionAppInfo> apps;
    apps.reserve(appsByPath.size());
    for (auto& [_, app] : appsByPath) {
        apps.push_back(std::move(app));
    }
    std::sort(apps.begin(), apps.end(), [](const AudioSessionAppInfo& left, const AudioSessionAppInfo& right) {
        return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
    });
    return apps;
}

bool WasapiCapture::start(AudioTrack track, const std::wstring& deviceId, AudioPacketCallback callback) {
    CaptureOptions options;
    options.track = track;
    options.deviceId = deviceId;
    return start(std::move(options), std::move(callback));
}

bool WasapiCapture::startProcessLoopback(uint32_t processId, AudioPacketCallback callback) {
    return startProcessLoopback(
        AudioTrack::SoundSeparation,
        processId,
        ProcessLoopbackMode::IncludeTargetProcessTree,
        std::move(callback));
}

bool WasapiCapture::startProcessLoopback(
    AudioTrack track,
    uint32_t processId,
    ProcessLoopbackMode mode,
    AudioPacketCallback callback) {
    if (processId == 0) {
        return false;
    }
    CaptureOptions options;
    options.track = track;
    options.processId = processId;
    options.processLoopback = true;
    options.processLoopbackMode = mode;
    return start(std::move(options), std::move(callback));
}

bool WasapiCapture::start(CaptureOptions options, AudioPacketCallback callback) {
    if (running_) {
        return true;
    }
    if (thread_.joinable()) {
        stopRequested_ = true;
        if (wakeEvent_) {
            SetEvent(wakeEvent_);
        }
        thread_.join();
    }
    if (wakeEvent_) {
        CloseHandle(wakeEvent_);
        wakeEvent_ = nullptr;
    }

    wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wakeEvent_) {
        Logger::instance().error(L"CreateEvent for WASAPI capture failed");
        return false;
    }

    stopRequested_ = false;
    running_ = true;
    std::promise<bool> startedPromise;
    std::future<bool> startedFuture = startedPromise.get_future();
    thread_ = std::thread(&WasapiCapture::captureThread, this, std::move(options), std::move(callback), std::move(startedPromise));

    // Block until the capture thread reports whether audio-client activation
    // actually succeeded. Without this the caller sees an optimistic "true"
    // while activation fails asynchronously, causing callers that poll
    // running() to retry (and re-log) every cycle forever.
    const bool started = startedFuture.get();
    if (!started && thread_.joinable()) {
        thread_.join();
    }
    return started;
}

void WasapiCapture::stop() {
    stopRequested_ = true;
    if (wakeEvent_) {
        SetEvent(wakeEvent_);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (wakeEvent_) {
        CloseHandle(wakeEvent_);
        wakeEvent_ = nullptr;
    }
    running_ = false;
}

WaveFormatBlob WasapiCapture::format() const {
    std::scoped_lock lock(formatMutex_);
    return format_;
}

void WasapiCapture::captureThread(CaptureOptions options, AudioPacketCallback callback, std::promise<bool> startedPromise) {
    const AudioTrack track = options.track;
    setThreadDescriptionSafe(options.processLoopback
        ? L"Backtrack WASAPI app loopback"
        : (track == AudioTrack::System ? L"Backtrack WASAPI loopback" : L"Backtrack WASAPI microphone"));

    // Report activation success/failure to start() exactly once. On any failure
    // path we also clear running_ so pollers see the real state.
    bool startSignaled = false;
    auto signalStarted = [&](bool ok) {
        if (!startSignaled) {
            startSignaled = true;
            if (!ok) {
                running_ = false;
            }
            startedPromise.set_value(ok);
        }
    };
    // Guarantee the promise is always fulfilled, even on an unexpected early
    // return, so start() can never block forever.
    struct PromiseGuard {
        std::function<void(bool)>& signal;
        ~PromiseGuard() { signal(false); }
    };
    std::function<void(bool)> signalFn = signalStarted;
    PromiseGuard guard{signalFn};

    ScopedComInitialization com(COINIT_MULTITHREADED);
    HRESULT hr = com.result();
    ScopedMmcssTask mmcss(L"Pro Audio", AVRT_PRIORITY_HIGH);

    Microsoft::WRL::ComPtr<IAudioClient> audioClient;
    if (options.processLoopback) {
        hr = activateProcessLoopbackAudioClient(options.processId, options.processLoopbackMode, &audioClient);
        if (FAILED(hr)) {
            Logger::instance().warning(L"Process loopback capture could not start for PID " +
                                       std::to_wstring(options.processId) + L": " + hresultToString(hr));
            running_ = false;
            return;
        }
    } else {
        Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) {
            Logger::instance().error(L"CoCreateInstance(MMDeviceEnumerator) failed: " + hresultToString(hr));
            running_ = false;
            return;
        }

        Microsoft::WRL::ComPtr<IMMDevice> endpoint;
        if (!options.deviceId.empty()) {
            hr = enumerator->GetDevice(options.deviceId.c_str(), &endpoint);
            if (FAILED(hr)) {
                Logger::instance().warning(std::wstring(L"Configured audio device was not found for ") + trackName(track) + L"; falling back to default");
            }
        }
        if (!endpoint) {
            hr = enumerator->GetDefaultAudioEndpoint(dataFlowFor(track), eConsole, &endpoint);
        }
        if (FAILED(hr)) {
            Logger::instance().error(std::wstring(L"GetDefaultAudioEndpoint failed for ") + trackName(track) + L": " + hresultToString(hr));
            running_ = false;
            return;
        }

        hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audioClient);
        if (FAILED(hr)) {
            Logger::instance().error(L"IMMDevice::Activate(IAudioClient) failed: " + hresultToString(hr));
            running_ = false;
            return;
        }
    }

    WAVEFORMATEX* rawMixFormat = nullptr;
    hr = audioClient->GetMixFormat(&rawMixFormat);
    std::unique_ptr<WAVEFORMATEX, WaveFormatDeleter> mixFormat(rawMixFormat);
    if (FAILED(hr) || !mixFormat) {
        Logger::instance().error(L"IAudioClient::GetMixFormat failed: " + hresultToString(hr));
        running_ = false;
        return;
    }

    const auto streamFormat = std::make_shared<const WaveFormatBlob>(copyWaveFormat(mixFormat.get()));
    {
        std::scoped_lock lock(formatMutex_);
        format_ = *streamFormat;
    }

    constexpr REFERENCE_TIME bufferDuration = 10'000'000;
    DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (track == AudioTrack::System || options.processLoopback) {
        streamFlags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    }

    hr = audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        streamFlags,
        bufferDuration,
        0,
        mixFormat.get(),
        nullptr);
    if (FAILED(hr)) {
        Logger::instance().error(std::wstring(L"IAudioClient::Initialize failed for ") + trackName(track) + L": " + hresultToString(hr));
        running_ = false;
        return;
    }

    hr = audioClient->SetEventHandle(wakeEvent_);
    if (FAILED(hr)) {
        Logger::instance().warning(L"IAudioClient::SetEventHandle failed; capture will use timed polling: " + hresultToString(hr));
    }

    Microsoft::WRL::ComPtr<IAudioCaptureClient> captureClient;
    hr = audioClient->GetService(IID_PPV_ARGS(&captureClient));
    if (FAILED(hr)) {
        Logger::instance().error(L"IAudioClient::GetService(IAudioCaptureClient) failed: " + hresultToString(hr));
        running_ = false;
        return;
    }

    hr = audioClient->Start();
    if (FAILED(hr)) {
        Logger::instance().error(L"IAudioClient::Start failed: " + hresultToString(hr));
        running_ = false;
        return;
    }

    Logger::instance().info(std::wstring(L"WASAPI capture started for ") + trackName(track));
    running_ = true;
    signalStarted(true);

    const int64_t qpcCalibration100ns = qpcNow100ns();
    const bool hasQpcCalibration = qpcCalibration100ns > 0;
    const int64_t qpcToSteadyOffset100ns = hasQpcCalibration
        ? steadyNow100ns() - qpcCalibration100ns
        : 0;

    int64_t nextPacketPts100ns = -1;
    while (!stopRequested_) {
        WaitForSingleObject(wakeEvent_, 20);

        UINT32 packetFrames = 0;
        hr = captureClient->GetNextPacketSize(&packetFrames);
        if (FAILED(hr)) {
            Logger::instance().warning(L"GetNextPacketSize failed: " + hresultToString(hr));
            break;
        }

        while (packetFrames > 0) {
            BYTE* data = nullptr;
            UINT32 framesAvailable = 0;
            DWORD flags = 0;
            UINT64 devicePosition = 0;
            UINT64 qpcPosition = 0;

            hr = captureClient->GetBuffer(&data, &framesAvailable, &flags, &devicePosition, &qpcPosition);
            if (FAILED(hr)) {
                Logger::instance().warning(L"IAudioCaptureClient::GetBuffer failed: " + hresultToString(hr));
                break;
            }

            const size_t byteCount = static_cast<size_t>(framesAvailable) * mixFormat->nBlockAlign;
            AudioPacket packet;
            packet.track = track;
            packet.processId = options.processId;
            packet.frameCount = framesAvailable;
            const int64_t packetDuration100ns = mixFormat->nSamplesPerSec > 0
                ? static_cast<int64_t>(
                      (static_cast<int64_t>(framesAvailable) * kHundredNanosecondsPerSecond) /
                      mixFormat->nSamplesPerSec)
                : 0;
            const bool timestampError = (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0;
            const int64_t capturedPts100ns = !timestampError && hasQpcCalibration && qpcPosition != 0
                ? std::max<int64_t>(0, static_cast<int64_t>(qpcPosition) + qpcToSteadyOffset100ns)
                : std::max<int64_t>(0, steadyNow100ns() - packetDuration100ns);
            packet.format = streamFormat;

            const bool hasTimeline = nextPacketPts100ns >= 0;
            const bool discontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
            const int64_t drift100ns = hasTimeline ? capturedPts100ns - nextPacketPts100ns : 0;
            const bool resyncTimeline =
                !hasTimeline ||
                discontinuity ||
                drift100ns < -kAudioTimestampResyncThreshold100ns ||
                drift100ns > kAudioTimestampResyncThreshold100ns;
            packet.pts100ns = resyncTimeline ? capturedPts100ns : nextPacketPts100ns;
            nextPacketPts100ns = packet.pts100ns + packetDuration100ns;

            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !data) {
                packet.bytes.assign(byteCount, 0);
            } else {
                packet.bytes.assign(data, data + byteCount);
            }

            hr = captureClient->ReleaseBuffer(framesAvailable);
            if (FAILED(hr)) {
                Logger::instance().warning(L"IAudioCaptureClient::ReleaseBuffer failed: " + hresultToString(hr));
                packetFrames = 0;
                callback(std::move(packet));
                break;
            }

            callback(std::move(packet));

            hr = captureClient->GetNextPacketSize(&packetFrames);
            if (FAILED(hr)) {
                Logger::instance().warning(L"GetNextPacketSize failed after ReleaseBuffer: " + hresultToString(hr));
                packetFrames = 0;
            }
        }
    }

    audioClient->Stop();
    running_ = false;
    Logger::instance().info(std::wstring(L"WASAPI capture stopped for ") + trackName(track));

}

} // namespace backtrack
