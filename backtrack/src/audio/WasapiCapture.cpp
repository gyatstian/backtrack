#include "audio/WasapiCapture.h"
#include "audio/SessionMuteStore.h"

#include "core/Clock.h"
#include "core/LogContext.h"
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
#include <filesystem>
#include <map>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <new>
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

// Terse negotiated-format description for the started/reconnected log lines.
std::wstring waveFormatText(const WAVEFORMATEX& wf) {
    std::wstring encoding;
    if (wf.wFormatTag == WAVE_FORMAT_PCM) {
        encoding = L"pcm";
    } else if (wf.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        encoding = L"float";
    } else if (wf.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        encoding = L"ext";
    } else {
        encoding = L"tag-" + std::to_wstring(wf.wFormatTag);
    }
    return std::to_wstring(wf.nSamplesPerSec) + L"Hz/" + std::to_wstring(wf.nChannels) + L"ch/" +
           std::to_wstring(wf.wBitsPerSample) + L"bit/" + encoding;
}

EDataFlow dataFlowFor(AudioTrack track) {
    return track == AudioTrack::Microphone ? eCapture : eRender;
}

class DefaultEndpointNotification final : public IMMNotificationClient {
public:
    explicit DefaultEndpointNotification(EDataFlow flow) : flow_(flow) {}

    HRESULT initialize(HANDLE wakeEvent) {
        if (!DuplicateHandle(GetCurrentProcess(), wakeEvent, GetCurrentProcess(), &wakeEvent_, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *object = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = --refs_;
        if (!refs) delete this;
        return refs;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        // An in-flight callback can outlive unregistration.
        AddRef();
        if (active_.load(std::memory_order_acquire) && flow == flow_ && role == eConsole) {
            generation_.fetch_add(1, std::memory_order_release);
            SetEvent(wakeEvent_);
        }
        Release();
        return S_OK;
    }

    uint64_t generation() const { return generation_.load(std::memory_order_acquire); }
    void deactivate() { active_.store(false, std::memory_order_release); }

private:
    ~DefaultEndpointNotification() {
        if (wakeEvent_) CloseHandle(wakeEvent_);
    }

    std::atomic<ULONG> refs_{1};
    std::atomic<uint64_t> generation_{0};
    std::atomic<bool> active_{true};
    const EDataFlow flow_;
    HANDLE wakeEvent_ = nullptr;
};

class ScopedDefaultEndpointNotification {
public:
    HRESULT start(IMMDeviceEnumerator* enumerator, EDataFlow flow, HANDLE wakeEvent) {
        notification_.Attach(new (std::nothrow) DefaultEndpointNotification(flow));
        if (!notification_) return E_OUTOFMEMORY;
        HRESULT hr = notification_->initialize(wakeEvent);
        if (FAILED(hr)) return hr;
        hr = enumerator->RegisterEndpointNotificationCallback(notification_.Get());
        if (SUCCEEDED(hr)) enumerator_ = enumerator;
        return hr;
    }

    ~ScopedDefaultEndpointNotification() {
        if (!enumerator_) return;
        notification_->deactivate();
        const HRESULT hr = enumerator_->UnregisterEndpointNotificationCallback(notification_.Get());
        if (FAILED(hr)) {
            // A failed unregister may leave the system holding this raw pointer.
            notification_.Detach();
            try {
                Logger::instance().warning(L"audio", L"Could not unregister default endpoint notification: " + hresultToString(hr));
            } catch (...) {
            }
        }
    }

    uint64_t generation() const { return notification_ ? notification_->generation() : 0; }
    bool changedSince(uint64_t generation) const { return this->generation() != generation; }

private:
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
    Microsoft::WRL::ComPtr<DefaultEndpointNotification> notification_;
};

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

// Takes ownership of a CoTaskMem-allocated string the way IMMDevice::GetId and
// IAudioSessionControl2::GetSessionInstanceIdentifier hand it out.
std::wstring takeCoTaskMemString(LPWSTR value) {
    if (!value) {
        return {};
    }
    std::wstring text = value;
    CoTaskMemFree(value);
    return text;
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

SessionMuteStore& sessionMuteStore() {
    // Process-global like the mutes it mirrors: applied mutes are global
    // system state, not per-stream state.
    static SessionMuteStore store(localAppDataPath() / L"audio-session-mutes.txt");
    return store;
}

// Collects every live foreign render session. False when the endpoint
// enumeration itself failed, in which case the caller leaves everything
// untouched. Only the sweep logs that failure; the mute path stays silent.
// COM is initialized by the caller and stays initialized for the whole store
// call: the candidates' callbacks hold ISimpleAudioVolume, so they must be
// invoked and released before that apartment goes away.
bool enumerateForeignRenderSessions(
    DWORD currentProcessId, std::vector<SessionMuteCandidate>& out, bool logFailures) {
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr) || !enumerator) {
        if (logFailures) {
            Logger::instance().warning(L"audio",
                L"Could not sweep orphaned session mutes: MMDeviceEnumerator unavailable");
        }
        return false;
    }

    Microsoft::WRL::ComPtr<IMMDeviceCollection> endpoints;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &endpoints);
    if (FAILED(hr) || !endpoints) {
        if (logFailures) {
            Logger::instance().warning(L"audio",
                L"Could not sweep orphaned session mutes: endpoint enumeration failed (" + hresultToString(hr) + L")");
        }
        return false;
    }

    UINT endpointCount = 0;
    endpoints->GetCount(&endpointCount);
    // Skips across the endpoint/session loops: a session we cannot read is
    // not muted, so excluded audio can leak. One rate-limited line per call.
    uint32_t skipped = 0;
    for (UINT endpointIndex = 0; endpointIndex < endpointCount; ++endpointIndex) {
        Microsoft::WRL::ComPtr<IMMDevice> endpoint;
        if (FAILED(endpoints->Item(endpointIndex, &endpoint)) || !endpoint) {
            ++skipped;
            continue;
        }

        LPWSTR endpointIdRaw = nullptr;
        const std::wstring endpointId = SUCCEEDED(endpoint->GetId(&endpointIdRaw))
            ? takeCoTaskMemString(endpointIdRaw)
            : std::wstring();

        Microsoft::WRL::ComPtr<IAudioSessionManager2> sessionManager;
        if (FAILED(endpoint->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &sessionManager)) ||
            !sessionManager) {
            ++skipped;
            continue;
        }

        Microsoft::WRL::ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(sessionManager->GetSessionEnumerator(&sessions)) || !sessions) {
            ++skipped;
            continue;
        }

        int sessionCount = 0;
        sessions->GetCount(&sessionCount);
        for (int sessionIndex = 0; sessionIndex < sessionCount; ++sessionIndex) {
            Microsoft::WRL::ComPtr<IAudioSessionControl> session;
            if (FAILED(sessions->GetSession(sessionIndex, &session)) || !session) {
                ++skipped;
                continue;
            }

            Microsoft::WRL::ComPtr<IAudioSessionControl2> session2;
            if (FAILED(session.As(&session2)) || !session2) {
                ++skipped;
                continue;
            }
            if (session2->IsSystemSoundsSession() == S_OK) {
                ++skipped;
                continue;
            }

            DWORD processId = 0;
            if (FAILED(session2->GetProcessId(&processId)) ||
                processId == 0 ||
                processId == currentProcessId) {
                ++skipped;
                continue;
            }

            const std::wstring key = normalizedPathKey(processImagePath(processId));
            if (key.empty()) {
                ++skipped;
                continue;
            }

            Microsoft::WRL::ComPtr<ISimpleAudioVolume> volume;
            if (FAILED(session.As(&volume)) || !volume) {
                ++skipped;
                continue;
            }

            LPWSTR instanceIdRaw = nullptr;
            const std::wstring instanceId = SUCCEEDED(session2->GetSessionInstanceIdentifier(&instanceIdRaw))
                ? takeCoTaskMemString(instanceIdRaw)
                : std::wstring();

            BOOL muted = FALSE;
            // A failed read leaves the previous state unknown; the store refuses
            // to mute then instead of guessing what to restore.
            const bool muteStateKnown = SUCCEEDED(volume->GetMute(&muted));
            // The lambda holds the volume alive, so the session cannot die
            // between enumeration and the store's SetMute. It is never stored
            // beyond the store call.
            out.push_back(SessionMuteCandidate{
                static_cast<uint32_t>(processId),
                key,
                endpointId,
                instanceId,
                muted == TRUE,
                muteStateKnown,
                [volume](bool mute) {
                    return SUCCEEDED(volume->SetMute(mute ? TRUE : FALSE, nullptr));
                },
            });
        }
    }
    if (skipped > 0 && logFailures) {
        Logger::instance().warning(L"audio",
            L"Session-mute enumeration skipped " + std::to_wstring(skipped) +
            L" endpoint/session(s); excluded audio may not be fully muted");
    }
    return true;
}

bool isProcessAlive(DWORD processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process) {
        CloseHandle(process);
    }
    return process != nullptr;
}

} // namespace

namespace {

// Process-lifetime and intentionally never destroyed: AudioPacket payloads can
// outlive static teardown (a replay buffer evicted at exit), and a destroyed
// resource would free into a dead pool.
std::pmr::synchronized_pool_resource& audioPacketPool() {
    static std::pmr::synchronized_pool_resource* pool =
        new std::pmr::synchronized_pool_resource();
    return *pool;
}

} // namespace

void ensureAudioPacketPool() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::pmr::set_default_resource(&audioPacketPool());
        // Touch the block classes that typical shared-mode packet sizes land in
        // (10 ms stereo float32 is 3840 bytes) so the first callbacks do not
        // pay for chunk creation on the real-time thread.
        for (const size_t packetBytes : {960u, 1920u, 3840u, 7680u}) {
            std::pmr::vector<uint8_t> warmUp(packetBytes);
        }
    });
}

void WasapiCapture::emergencyRestoreSessionMutes() noexcept {
    try {
        // COM must be initialized on this thread for the whole store call, and
        // the call releases every interface it created before it returns. The
        // crash path may run on a thread that never touched COM (SEH filter,
        // terminate handler, console control handler).
        ScopedComInitialization com(COINIT_MULTITHREADED);
        const DWORD currentProcessId = GetCurrentProcessId();
        sessionMuteStore().emergencyRestore(
            [&](std::vector<SessionMuteCandidate>& sessions) {
                return enumerateForeignRenderSessions(currentProcessId, sessions, false);
            });
    } catch (...) {
        // the singleton may not have been constructed before the fatal exit.
    }
}

void WasapiCapture::restoreOrphanedSessionMutes() {
    // Startup sweep for mutes applied by a previous crashed session. Runs
    // under the single-instance mutex, so no concurrent session can apply
    // fresh mutes while sweeping. COM is initialized here and balanced again
    // before Application's OleInitialize runs.
    ScopedComInitialization com(COINIT_MULTITHREADED);
    const DWORD currentProcessId = GetCurrentProcessId();
    const auto result = sessionMuteStore().sweepOrphans(
        [&](std::vector<SessionMuteCandidate>& sessions) {
            return enumerateForeignRenderSessions(currentProcessId, sessions, true);
        },
        isProcessAlive);
    if (!result) {
        return;
    }

    Logger::instance().info(L"audio",
        L"Orphaned session-mute sweep restored " + std::to_wstring(result->restored) +
        L" audio session(s); " + std::to_wstring(result->unmatched) +
        L" journaled entr(y/ies) no longer matched");
    if (result->journalFailed) {
        Logger::instance().warning(L"audio",
            L"Orphaned session-mute sweep could not update the mute journal; it stays for the next launch");
    }
}


SessionMuteApplyResult WasapiCapture::applySessionMutesForExecutables(
    const std::vector<std::wstring>& mutedExecutableKeys) {
    // One apartment for enumeration, every callback invocation, and the release
    // of the temporary ISimpleAudioVolume interfaces. The store retains none of
    // them, so nothing COM-owned escapes this scope.
    ScopedComInitialization com(COINIT_MULTITHREADED);
    const DWORD currentProcessId = GetCurrentProcessId();
    return sessionMuteStore().apply(
        mutedExecutableKeys,
        [&](std::vector<SessionMuteCandidate>& sessions) {
            return enumerateForeignRenderSessions(currentProcessId, sessions, false);
        },
        isProcessAlive);
}

SessionMuteRestoreResult WasapiCapture::clearSessionMutes() {
    ScopedComInitialization com(COINIT_MULTITHREADED);
    const DWORD currentProcessId = GetCurrentProcessId();
    return sessionMuteStore().clear(
        [&](std::vector<SessionMuteCandidate>& sessions) {
            return enumerateForeignRenderSessions(currentProcessId, sessions, false);
        },
        isProcessAlive);
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
        Logger::instance().error(L"audio", L"CreateEvent for WASAPI capture failed");
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
    // Do not join on failure: the capture thread stays alive and keeps retrying
    // to reconnect (a failed first activation can still succeed later when the
    // device comes back). It is reaped by stop() or by a subsequent start().
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
    // Hand back the pool's unused chunks: a replay window's worth of retained
    // payloads would otherwise stay reserved for the process lifetime. Only
    // unused blocks are freed, so packets still held downstream are safe.
    audioPacketPool().release();
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
    const LogContext::ScopedTask taskContext(L"wasapi-" + std::wstring(trackName(track)));

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

    // Endpoint loss is retried; a healthy default endpoint needs an explicit
    // notification because changing the Windows default need not invalidate it.
    constexpr int64_t kMaxReconnectDelayMs = 2000;
    int64_t reconnectDelayMs = 250;
    unsigned reconnectCount = 0;

    // Waits out the backoff (interruptibly, via wakeEvent_, so stop() wakes the
    // thread immediately) and returns true when another connection attempt should
    // run, or false when stop() was requested while waiting.
    auto reconnectBackoff = [&](bool wasCapturing) -> bool {
        running_ = false;
        signalStarted(false);
        const int64_t delayMs = reconnectDelayMs;
        ++reconnectCount;
        if (reconnectCount == 1) {
            Logger::instance().info(L"audio",
                std::wstring(wasCapturing ? L"WASAPI capture session lost for " : L"WASAPI capture could not start for ") +
                trackName(track) + L"; retrying in " + std::to_wstring(delayMs) + L"ms");
        } else {
            Logger::instance().debug(L"audio",
                std::wstring(L"WASAPI capture reconnect attempt ") + std::to_wstring(reconnectCount) +
                L" for " + trackName(track) + L" pending; next retry in " + std::to_wstring(delayMs) + L"ms");
        }
        if (stopRequested_) {
            return false;
        }
        WaitForSingleObject(wakeEvent_, static_cast<DWORD>(delayMs));
        reconnectDelayMs = std::min<int64_t>(reconnectDelayMs * 2, kMaxReconnectDelayMs);
        return !stopRequested_;
    };

    while (!stopRequested_) {
        ScopedDefaultEndpointNotification defaultNotification;
        uint64_t defaultGeneration = 0;
        Microsoft::WRL::ComPtr<IAudioClient> audioClient;
        if (options.processLoopback) {
            hr = activateProcessLoopbackAudioClient(options.processId, options.processLoopbackMode, &audioClient);
            if (FAILED(hr)) {
                if (reconnectCount == 0) {
                    Logger::instance().warning(L"audio", L"Process loopback capture could not start for PID " +
                                               std::to_wstring(options.processId) + L": " + hresultToString(hr));
                }
                if (!reconnectBackoff(false)) {
                    break;
                }
                continue;
            }
        } else {
            Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
            hr = options.createEnumerator
                ? options.createEnumerator(enumerator.GetAddressOf())
                : CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
            if (FAILED(hr) || !enumerator) {
                if (reconnectCount == 0) {
                    Logger::instance().error(L"audio", L"CoCreateInstance(MMDeviceEnumerator) failed: " + hresultToString(hr));
                }
                if (!reconnectBackoff(false)) {
                    break;
                }
                continue;
            }

            Microsoft::WRL::ComPtr<IMMDevice> endpoint;
            if (options.deviceId.empty()) {
                hr = defaultNotification.start(enumerator.Get(), dataFlowFor(track), wakeEvent_);
                if (FAILED(hr)) {
                    if (reconnectCount == 0) {
                        Logger::instance().error(L"audio", std::wstring(L"Default endpoint notification failed for ") + trackName(track) + L": " + hresultToString(hr));
                    }
                    if (!reconnectBackoff(false)) {
                        break;
                    }
                    continue;
                }
                // Subscribe before resolving so changes during setup cannot be lost.
                defaultGeneration = defaultNotification.generation();
                hr = enumerator->GetDefaultAudioEndpoint(dataFlowFor(track), eConsole, &endpoint);
            } else {
                hr = enumerator->GetDevice(options.deviceId.c_str(), &endpoint);
            }
            if (FAILED(hr) || !endpoint) {
                if (reconnectCount == 0) {
                    Logger::instance().error(L"audio", std::wstring(options.deviceId.empty() ? L"GetDefaultAudioEndpoint failed for " : L"Configured audio device was not found for ") + trackName(track) + L": " + hresultToString(hr));
                }
                if (!reconnectBackoff(false)) {
                    break;
                }
                continue;
            }

            hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audioClient);
            if (FAILED(hr)) {
                if (reconnectCount == 0) {
                    Logger::instance().error(L"audio", L"IMMDevice::Activate(IAudioClient) failed: " + hresultToString(hr));
                }
                if (!reconnectBackoff(false)) {
                    break;
                }
                continue;
            }
        }

        WAVEFORMATEX* rawMixFormat = nullptr;
        hr = audioClient->GetMixFormat(&rawMixFormat);
        std::unique_ptr<WAVEFORMATEX, WaveFormatDeleter> mixFormat(rawMixFormat);
        if (FAILED(hr) || !mixFormat) {
            if (reconnectCount == 0) {
                Logger::instance().error(L"audio", L"IAudioClient::GetMixFormat failed: " + hresultToString(hr));
            }
            if (!reconnectBackoff(false)) {
                break;
            }
            continue;
        }

        const auto streamFormat = std::make_shared<const WaveFormatBlob>(copyWaveFormat(mixFormat.get()));
        bool formatChanged = false;
        {
            std::scoped_lock lock(formatMutex_);
            formatChanged = format_.bytes != streamFormat->bytes;
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
            if (reconnectCount == 0) {
                Logger::instance().error(L"audio", std::wstring(L"IAudioClient::Initialize failed for ") + trackName(track) + L": " + hresultToString(hr));
            }
            if (!reconnectBackoff(false)) {
                break;
            }
            continue;
        }

        hr = audioClient->SetEventHandle(wakeEvent_);
        if (FAILED(hr) && reconnectCount == 0) {
            Logger::instance().warning(L"audio", L"IAudioClient::SetEventHandle failed; capture will use timed polling: " + hresultToString(hr));
        }

        Microsoft::WRL::ComPtr<IAudioCaptureClient> captureClient;
        hr = audioClient->GetService(IID_PPV_ARGS(&captureClient));
        if (FAILED(hr)) {
            if (reconnectCount == 0) {
                Logger::instance().error(L"audio", L"IAudioClient::GetService(IAudioCaptureClient) failed: " + hresultToString(hr));
            }
            if (!reconnectBackoff(false)) {
                break;
            }
            continue;
        }

        hr = audioClient->Start();
        if (FAILED(hr)) {
            if (reconnectCount == 0) {
                Logger::instance().error(L"audio", L"IAudioClient::Start failed: " + hresultToString(hr));
            }
            if (!reconnectBackoff(false)) {
                break;
            }
            continue;
        }

        if (stopRequested_ || defaultNotification.changedSince(defaultGeneration)) {
            audioClient->Stop();
            running_ = false;
            signalStarted(false);
            continue;
        }

        const bool reconnected = reconnectCount > 0;
        const unsigned reconnectAttempts = reconnectCount; // for the reconnected log line
        reconnectCount = 0;
        reconnectDelayMs = 250;
        running_ = true;
        signalStarted(true);
        if (reconnected) {
            Logger::instance().info(L"audio",
                std::wstring(L"WASAPI capture reconnected for ") + trackName(track) +
                L" after " + std::to_wstring(reconnectAttempts) + L" attempt(s); format=" +
                waveFormatText(*mixFormat) + (formatChanged ? L" (changed from previous)" : L""));
        } else {
            Logger::instance().info(L"audio",
                std::wstring(L"WASAPI capture started for ") + trackName(track) +
                L"; format=" + waveFormatText(*mixFormat));
        }

        const int64_t qpcCalibration100ns = qpcNow100ns();
        const bool hasQpcCalibration = qpcCalibration100ns > 0;
        const int64_t qpcToSteadyOffset100ns = hasQpcCalibration
            ? steadyNow100ns() - qpcCalibration100ns
            : 0;

        int64_t nextPacketPts100ns = -1;
        bool sessionLost = false;
        // Underrun probe: a packet arrival gap past 50 ms is a device stall or
        // CPU starvation, not normal device cadence.
        auto lastArrival = std::chrono::steady_clock::now();
        while (!stopRequested_ && !sessionLost && !defaultNotification.changedSince(defaultGeneration)) {
            WaitForSingleObject(wakeEvent_, 20);
            if (!stopRequested_ && defaultNotification.changedSince(defaultGeneration)) {
                break;
            }

            UINT32 packetFrames = 0;
            hr = captureClient->GetNextPacketSize(&packetFrames);
            if (FAILED(hr)) {
                Logger::instance().warning(L"audio", L"GetNextPacketSize failed: " + hresultToString(hr));
                sessionLost = true;
                break;
            }

            while (packetFrames > 0 && (stopRequested_ || !defaultNotification.changedSince(defaultGeneration))) {
                BYTE* data = nullptr;
                UINT32 framesAvailable = 0;
                DWORD flags = 0;
                UINT64 devicePosition = 0;
                UINT64 qpcPosition = 0;

                hr = captureClient->GetBuffer(&data, &framesAvailable, &flags, &devicePosition, &qpcPosition);
                if (FAILED(hr)) {
                    Logger::instance().warning(L"audio", L"IAudioCaptureClient::GetBuffer failed: " + hresultToString(hr));
                    sessionLost = true;
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
                if (resyncTimeline && hasTimeline) {
                    // Mid-stream resync only (the first-packet !hasTimeline
                    // case is expected, not a gap). Rate-limited by the logger.
                    Logger::instance().warning(L"audio",
                        L"WASAPI capture timeline resynced for " + std::wstring(trackName(track)) +
                        L" (drift_ms=" +
                        std::to_wstring(drift100ns / (kHundredNanosecondsPerSecond / 1000)) +
                        L" discontinuity=" + (discontinuity ? L"yes" : L"no") +
                        L"); audio gap expected");
                }
                packet.pts100ns = resyncTimeline ? capturedPts100ns : nextPacketPts100ns;
                nextPacketPts100ns = packet.pts100ns + packetDuration100ns;

                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !data) {
                    packet.bytes.assign(byteCount, 0);
                } else {
                    packet.bytes.assign(data, data + byteCount);
                }

                hr = captureClient->ReleaseBuffer(framesAvailable);
                if (FAILED(hr)) {
                    // ReleaseBuffer failing means the client is as broken as a
                    // GetBuffer failure; reconnect now instead of spinning on
                    // a dead client until the next error surfaces.
                    Logger::instance().warning(L"audio", L"IAudioCaptureClient::ReleaseBuffer failed: " + hresultToString(hr));
                    sessionLost = true;
                    packetFrames = 0;
                    callback(std::move(packet));
                    break;
                }

                callback(std::move(packet));

                const auto arrival = std::chrono::steady_clock::now();
                const auto gapMs = std::chrono::duration_cast<std::chrono::milliseconds>(arrival - lastArrival);
                lastArrival = arrival;
                if (gapMs >= std::chrono::milliseconds(50)) {
                    // gap_ms is wall-clock packet-arrival spacing, not a count
                    // of missing buffers: it grows through true silence too
                    // (a silent loopback delivers nothing), so it is a
                    // liveness hint, not proof of lost audio.
                    Logger::instance().warning(L"audio",
                        L"WASAPI capture underrun (gap_ms=" + std::to_wstring(gapMs.count()) +
                        L" track=" + std::wstring(trackName(track)) +
                        L"; packet-arrival gap, includes silence)");
                }

                hr = captureClient->GetNextPacketSize(&packetFrames);
                if (FAILED(hr)) {
                    Logger::instance().warning(L"audio", L"GetNextPacketSize failed after ReleaseBuffer: " + hresultToString(hr));
                    packetFrames = 0;
                }
            }
        }

        audioClient->Stop();
        running_ = false;

        if (stopRequested_) {
            break;
        }
        if (defaultNotification.changedSince(defaultGeneration)) {
            Logger::instance().info(L"audio", std::wstring(L"Default endpoint changed for ") + trackName(track) + L"; reconnecting");
            continue;
        }
        if (sessionLost) {
            if (!reconnectBackoff(true)) {
                break;
            }
            continue;
        }
        break;
    }

    Logger::instance().info(L"audio", std::wstring(L"WASAPI capture stopped for ") + trackName(track));

}

} // namespace backtrack
