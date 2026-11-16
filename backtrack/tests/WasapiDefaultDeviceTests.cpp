#include "audio/WasapiCapture.h"

#include <audioclient.h>
#include <combaseapi.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace backtrack {

struct WasapiCaptureTestAccess {
    static bool start(WasapiCapture& capture, AudioTrack track, const std::wstring& id,
        std::function<HRESULT(IMMDeviceEnumerator**)> factory, AudioPacketCallback callback) {
        WasapiCapture::CaptureOptions options;
        options.track = track;
        options.deviceId = id;
        options.createEnumerator = std::move(factory);
        return capture.start(std::move(options), std::move(callback));
    }
    static bool stopRequested(const WasapiCapture& capture) { return capture.stopRequested_.load(); }
};

std::filesystem::path localAppDataPath() { return {}; }
void setThreadDescriptionSafe(const wchar_t*) {}

} // namespace backtrack

namespace {

using namespace std::chrono_literals;
using Microsoft::WRL::ComPtr;
using backtrack::AudioPacket;
using backtrack::AudioTrack;
using backtrack::WasapiCapture;
using backtrack::WasapiCaptureTestAccess;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Derived, typename Interface>
class FakeCom : public Interface {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(Interface)) return E_NOINTERFACE;
        *object = static_cast<Interface*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = --refs_;
        if (!refs) delete static_cast<Derived*>(this);
        return refs;
    }
private:
    std::atomic<ULONG> refs_{1};
};

enum class SetupFailure { None, Enumerator, Registration, Endpoint, Activation, MixFormat, Initialize, Service, Start };

struct FakeState {
    std::mutex mutex;
    std::condition_variable changed;
    std::wstring defaultId = L"A";
    IMMNotificationClient* listener = nullptr;
    std::vector<std::wstring> starts;
    std::vector<std::wstring> stops;
    std::vector<BYTE> packets;
    EDataFlow expectedFlow = eRender;
    int polls = 0;
    int lookups = 0;
    int registerCalls = 0;
    int unregisterCalls = 0;
    int registerFailures = 0;
    DWORD registrationThread = 0;
    bool wrongRegistrationThread = false;
    bool failUnregister = false;
    bool missingExplicit = false;
    bool switchDuringLookup = false;
    bool switchDuringPacket = false;
    bool blockNextPoll = false;
    bool pollBlocked = false;
    bool releasePoll = false;
    bool wrongLookup = false;
    SetupFailure failure = SetupFailure::None;
    int failuresLeft = 0;
    int failuresObserved = 0;

    bool fail(SetupFailure stage) {
        std::scoped_lock lock(mutex);
        if (failure != stage || failuresLeft == 0) return false;
        --failuresLeft;
        ++failuresObserved;
        changed.notify_all();
        return true;
    }

    template <typename Predicate>
    bool wait(Predicate predicate) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 3s, [&] { return predicate(*this); });
    }

    ComPtr<IMMNotificationClient> retainedListener() {
        std::scoped_lock lock(mutex);
        ComPtr<IMMNotificationClient> retained = listener;
        return retained;
    }

    void notify(EDataFlow flow, ERole role, const wchar_t* id, bool updateDefault) {
        ComPtr<IMMNotificationClient> retained;
        {
            std::scoped_lock lock(mutex);
            if (updateDefault) defaultId = id ? id : L"";
            retained = listener;
        }
        if (retained) retained->OnDefaultDeviceChanged(flow, role, id);
    }
};

class FakeCaptureClient final : public FakeCom<FakeCaptureClient, IAudioCaptureClient> {
public:
    FakeCaptureClient(std::shared_ptr<FakeState> state, const std::wstring& id)
        : state_(std::move(state)), bytes_{static_cast<BYTE>(id[0]), 0, 0, 0} {}

    HRESULT STDMETHODCALLTYPE GetBuffer(BYTE** data, UINT32* frames, DWORD* flags, UINT64* position, UINT64* qpc) override {
        *data = bytes_;
        *frames = 1;
        *flags = 0;
        if (position) *position = 0;
        if (qpc) *qpc = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32) override { pending_ = false; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetNextPacketSize(UINT32* frames) override {
        {
            std::unique_lock lock(state_->mutex);
            if (state_->blockNextPoll) {
                state_->blockNextPoll = false;
                state_->pollBlocked = true;
                state_->changed.notify_all();
                state_->changed.wait(lock, [&] { return state_->releasePoll; });
                pending_ = true;
            }
            ++state_->polls;
            state_->changed.notify_all();
        }
        *frames = pending_ ? 1 : 0;
        return S_OK;
    }
private:
    std::shared_ptr<FakeState> state_;
    BYTE bytes_[4];
    bool pending_ = true;
};

class FakeAudioClient final : public FakeCom<FakeAudioClient, IAudioClient> {
public:
    FakeAudioClient(std::shared_ptr<FakeState> state, std::wstring id)
        : state_(std::move(state)), id_(std::move(id)) {}

    HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE, DWORD, REFERENCE_TIME, REFERENCE_TIME, const WAVEFORMATEX*, LPCGUID) override {
        return state_->fail(SetupFailure::Initialize) ? E_FAIL : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE, const WAVEFORMATEX*, WAVEFORMATEX**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX** format) override {
        if (state_->fail(SetupFailure::MixFormat)) return E_FAIL;
        *format = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
        if (!*format) return E_OUTOFMEMORY;
        **format = {};
        (*format)->wFormatTag = WAVE_FORMAT_PCM;
        (*format)->nChannels = 2;
        (*format)->nSamplesPerSec = 48000;
        (*format)->wBitsPerSample = 16;
        (*format)->nBlockAlign = 4;
        (*format)->nAvgBytesPerSec = 192000;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME*, REFERENCE_TIME*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Start() override {
        if (state_->fail(SetupFailure::Start)) return E_FAIL;
        std::scoped_lock lock(state_->mutex);
        state_->starts.push_back(id_);
        state_->changed.notify_all();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Stop() override {
        std::scoped_lock lock(state_->mutex);
        state_->stops.push_back(id_);
        state_->changed.notify_all();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Reset() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetService(REFIID iid, void** object) override {
        if (state_->fail(SetupFailure::Service)) return E_FAIL;
        if (iid != __uuidof(IAudioCaptureClient)) return E_NOINTERFACE;
        *object = static_cast<IAudioCaptureClient*>(new FakeCaptureClient(state_, id_));
        return S_OK;
    }
private:
    std::shared_ptr<FakeState> state_;
    std::wstring id_;
};

class FakeDevice final : public FakeCom<FakeDevice, IMMDevice> {
public:
    FakeDevice(std::shared_ptr<FakeState> state, std::wstring id)
        : state_(std::move(state)), id_(std::move(id)) {}

    HRESULT STDMETHODCALLTYPE Activate(REFIID iid, DWORD, PROPVARIANT*, void** object) override {
        if (state_->fail(SetupFailure::Activation)) return E_FAIL;
        if (iid != __uuidof(IAudioClient)) return E_NOINTERFACE;
        *object = static_cast<IAudioClient*>(new FakeAudioClient(state_, id_));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD, IPropertyStore**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetId(LPWSTR*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetState(DWORD*) override { return E_NOTIMPL; }
private:
    std::shared_ptr<FakeState> state_;
    std::wstring id_;
};

class FakeEnumerator final : public FakeCom<FakeEnumerator, IMMDeviceEnumerator> {
public:
    explicit FakeEnumerator(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}

    HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow, DWORD, IMMDeviceCollection**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow flow, ERole role, IMMDevice** endpoint) override {
        if (state_->fail(SetupFailure::Endpoint)) return E_FAIL;
        std::wstring id;
        ComPtr<IMMNotificationClient> retained;
        {
            std::scoped_lock lock(state_->mutex);
            ++state_->lookups;
            state_->wrongLookup |= flow != state_->expectedFlow || role != eConsole || !state_->listener;
            id = state_->defaultId;
            if (state_->switchDuringLookup) {
                state_->switchDuringLookup = false;
                state_->defaultId = L"B";
                retained = state_->listener;
            }
            state_->changed.notify_all();
        }
        if (retained) retained->OnDefaultDeviceChanged(flow, role, L"B");
        if (id.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        *endpoint = new FakeDevice(state_, id);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR id, IMMDevice** endpoint) override {
        std::scoped_lock lock(state_->mutex);
        if (state_->missingExplicit) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        *endpoint = new FakeDevice(state_, id);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient* client) override {
        if (state_->fail(SetupFailure::Registration)) return E_FAIL;
        std::scoped_lock lock(state_->mutex);
        ++state_->registerCalls;
        APTTYPE apartment;
        APTTYPEQUALIFIER qualifier;
        const DWORD thread = GetCurrentThreadId();
        state_->wrongRegistrationThread |= FAILED(CoGetApartmentType(&apartment, &qualifier)) || apartment != APTTYPE_MTA ||
            (state_->registrationThread != 0 && state_->registrationThread != thread);
        state_->registrationThread = thread;
        if (state_->registerFailures > 0) {
            --state_->registerFailures;
            return E_FAIL;
        }
        state_->listener = client;
        state_->changed.notify_all();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient* client) override {
        std::scoped_lock lock(state_->mutex);
        ++state_->unregisterCalls;
        state_->wrongRegistrationThread |= state_->registrationThread != GetCurrentThreadId();
        if (state_->failUnregister) return E_FAIL;
        if (state_->listener != client) return E_INVALIDARG;
        state_->listener = nullptr;
        state_->changed.notify_all();
        return S_OK;
    }
private:
    std::shared_ptr<FakeState> state_;
};

bool startCapture(WasapiCapture& capture, const std::shared_ptr<FakeState>& state, AudioTrack track, const std::wstring& id) {
    return WasapiCaptureTestAccess::start(capture, track, id,
        [state](IMMDeviceEnumerator** enumerator) -> HRESULT {
            if (state->fail(SetupFailure::Enumerator)) return E_FAIL;
            *enumerator = new FakeEnumerator(state);
            return S_OK;
        },
        [state](AudioPacket&& packet) {
            bool switchDefault = false;
            {
                std::scoped_lock lock(state->mutex);
                if (!packet.bytes.empty()) state->packets.push_back(packet.bytes[0]);
                switchDefault = state->switchDuringPacket;
                state->switchDuringPacket = false;
                state->changed.notify_all();
            }
            if (switchDefault) state->notify(state->expectedFlow, eConsole, L"B", true);
        });
}

bool hasPacket(const FakeState& state, BYTE marker) {
    return std::find(state.packets.begin(), state.packets.end(), marker) != state.packets.end();
}

void testDefaultSwitch(AudioTrack track, EDataFlow flow) {
    auto state = std::make_shared<FakeState>();
    state->expectedFlow = flow;
    WasapiCapture capture;
    expect(startCapture(capture, state, track, L""), "default A did not start");
    expect(state->wait([](const auto& s) { return hasPacket(s, 'A'); }), "default A did not deliver a packet");

    int polls;
    {
        std::scoped_lock lock(state->mutex);
        polls = state->polls;
    }
    state->notify(flow == eRender ? eCapture : eRender, eConsole, L"B", false);
    state->notify(flow, eCommunications, L"B", false);
    state->notify(flow, eMultimedia, L"B", false);
    expect(state->wait([&](const auto& s) { return s.polls >= polls + 3; }), "worker did not continue polling A");
    {
        std::scoped_lock lock(state->mutex);
        expect(state->starts == std::vector<std::wstring>{L"A"}, "unrelated flow or role caused a switch");
    }

    state->notify(flow, eConsole, L"B", true);
    expect(state->wait([](const auto& s) { return hasPacket(s, 'B'); }), "healthy A did not switch to default B");
    capture.stop();
    std::scoped_lock lock(state->mutex);
    expect(state->starts == std::vector<std::wstring>({L"A", L"B"}), "unexpected clients during default switch");
    expect(state->stops == std::vector<std::wstring>({L"A", L"B"}), "default clients were not stopped");
    expect(!state->wrongLookup, "lookup did not subscribe first or use the expected flow/eConsole");
    expect(!state->wrongRegistrationThread && state->registrationThread != GetCurrentThreadId(), "notification registration left the capture COM thread");
    expect(state->registerCalls == state->unregisterCalls, "default notifications were not unregistered");
}

void testExplicitDeviceRemainsPinned() {
    auto state = std::make_shared<FakeState>();
    WasapiCapture capture;
    expect(startCapture(capture, state, AudioTrack::System, L"A"), "explicit A did not start");
    expect(state->wait([](const auto& s) { return hasPacket(s, 'A'); }), "explicit A did not deliver a packet");
    int polls;
    {
        std::scoped_lock lock(state->mutex);
        polls = state->polls;
    }
    state->notify(eRender, eConsole, L"B", true);
    expect(state->wait([&](const auto& s) { return s.polls >= polls + 3; }), "explicit A stopped polling");
    capture.stop();
    std::scoped_lock lock(state->mutex);
    expect(state->starts == std::vector<std::wstring>{L"A"}, "explicit A followed default B");
    expect(state->lookups == 0 && state->registerCalls == 0, "explicit capture watched or resolved the default");
}

void testMissingExplicitDeviceDoesNotFallBack() {
    auto state = std::make_shared<FakeState>();
    state->missingExplicit = true;
    WasapiCapture capture;
    expect(!startCapture(capture, state, AudioTrack::Microphone, L"A"), "missing explicit device unexpectedly started");
    capture.stop();
    std::scoped_lock lock(state->mutex);
    expect(state->starts.empty() && state->lookups == 0 && state->registerCalls == 0, "missing explicit device fell back to default");
}

void testSwitchDuringResolution() {
    auto state = std::make_shared<FakeState>();
    state->switchDuringLookup = true;
    WasapiCapture capture;
    expect(!startCapture(capture, state, AudioTrack::System, L""), "stale first endpoint was reported as started");
    expect(state->wait([](const auto& s) { return hasPacket(s, 'B'); }), "change during endpoint resolution was lost");
    capture.stop();
    std::scoped_lock lock(state->mutex);
    expect(!hasPacket(*state, 'A'), "stale A delivered a packet after setup change");
    expect(state->starts == std::vector<std::wstring>({L"A", L"B"}), "setup race did not rebuild the stale client");
    expect(!state->wrongLookup, "setup lookup ran before registration");
}

void testSwitchDuringPacketCallback() {
    auto state = std::make_shared<FakeState>();
    state->switchDuringPacket = true;
    WasapiCapture capture;
    expect(startCapture(capture, state, AudioTrack::System, L""), "packet-race capture did not start");
    expect(state->wait([](const auto& s) { return hasPacket(s, 'B'); }), "change during packet delivery was lost");
    capture.stop();
    std::scoped_lock lock(state->mutex);
    expect(state->starts == std::vector<std::wstring>({L"A", L"B"}), "packet race did not switch A to B");
}

void testDefaultLossAndRecovery() {
    auto state = std::make_shared<FakeState>();
    state->expectedFlow = eCapture;
    WasapiCapture capture;
    expect(startCapture(capture, state, AudioTrack::Microphone, L""), "microphone A did not start");
    state->notify(eCapture, eConsole, nullptr, true);
    expect(state->wait([](const auto& s) { return s.lookups >= 2 && s.defaultId.empty(); }), "missing default was not resolved");
    state->notify(eCapture, eConsole, L"B", true);
    expect(state->wait([](const auto& s) { return hasPacket(s, 'B'); }), "microphone did not recover when a default returned");
    capture.stop();
}

void testRegistrationFailureRetries() {
    auto state = std::make_shared<FakeState>();
    state->registerFailures = 1;
    WasapiCapture capture;
    expect(!startCapture(capture, state, AudioTrack::System, L""), "failed registration must fail the first attempt");
    expect(state->wait([](const auto& s) { return hasPacket(s, 'A'); }), "registration failure did not retry");
    capture.stop();
    std::scoped_lock lock(state->mutex);
    expect(state->registerCalls >= 2 && state->unregisterCalls == 1, "registration retry leaked or skipped cleanup");
}

void testSetupFailuresRetryUntilRecovered() {
    for (const auto stage : {SetupFailure::Enumerator, SetupFailure::Registration, SetupFailure::Endpoint,
             SetupFailure::Activation, SetupFailure::MixFormat, SetupFailure::Initialize,
             SetupFailure::Service, SetupFailure::Start}) {
        auto state = std::make_shared<FakeState>();
        state->failure = stage;
        state->failuresLeft = 2;
        WasapiCapture capture;
        expect(!startCapture(capture, state, AudioTrack::System, L""), "setup failure must report a failed first attempt");
        expect(state->wait([](const auto& s) { return hasPacket(s, 'A'); }), "repeated setup failure did not recover");
        capture.stop();
        std::scoped_lock lock(state->mutex);
        expect(state->failuresObserved == 2 && state->failuresLeft == 0, "setup failures were not both retried");
        expect(state->starts == std::vector<std::wstring>{L"A"} && state->stops == std::vector<std::wstring>{L"A"},
            "setup retries must start and stop only the successful client");
        expect(state->listener == nullptr, "setup retries left a notification registered");
    }
}

void testStopDuringSetupBackoff() {
    auto state = std::make_shared<FakeState>();
    state->failure = SetupFailure::Enumerator;
    state->failuresLeft = 100;
    WasapiCapture capture;
    expect(!startCapture(capture, state, AudioTrack::System, L""), "failed enumerator must fail the first attempt");
    capture.stop();
    std::scoped_lock lock(state->mutex);
    expect(!capture.running() && state->starts.empty() && state->listener == nullptr,
        "stop during setup backoff must not start a client or retain a notification");
}

void testStopDrainsPendingPacket() {
    auto state = std::make_shared<FakeState>();
    WasapiCapture capture;
    expect(startCapture(capture, state, AudioTrack::System, L"A"), "stop-drain capture did not start");
    expect(state->wait([](const auto& s) { return hasPacket(s, 'A'); }), "stop-drain capture delivered no initial packet");
    {
        std::scoped_lock lock(state->mutex);
        state->blockNextPoll = true;
    }
    const bool blocked = state->wait([](const auto& s) { return s.pollBlocked; });
    if (!blocked) {
        {
            std::scoped_lock lock(state->mutex);
            state->blockNextPoll = false;
            state->releasePoll = true;
            state->changed.notify_all();
        }
        capture.stop();
        expect(false, "capture did not enter the controlled poll");
    }

    std::thread stopper([&] { capture.stop(); });
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!WasapiCaptureTestAccess::stopRequested(capture) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool requested = WasapiCaptureTestAccess::stopRequested(capture);
    {
        std::scoped_lock lock(state->mutex);
        state->releasePoll = true;
        state->changed.notify_all();
    }
    stopper.join();
    expect(requested, "stop was not requested while the poll was blocked");
    std::scoped_lock lock(state->mutex);
    expect(std::count(state->packets.begin(), state->packets.end(), static_cast<BYTE>('A')) == 2,
        "stop discarded the packet already being polled");
}

void testCallbacksAcrossStop() {
    auto state = std::make_shared<FakeState>();
    WasapiCapture capture;
    expect(startCapture(capture, state, AudioTrack::System, L""), "race capture did not start");
    auto retained = state->retainedListener();
    expect(retained != nullptr, "missing registered callback");
    std::atomic<bool> started = false;
    std::atomic<bool> done = false;
    std::thread notifier([&] {
        started = true;
        while (!done.load()) retained->OnDefaultDeviceChanged(eRender, eConsole, L"B");
    });
    while (!started.load()) std::this_thread::yield();
    capture.stop();
    done = true;
    notifier.join();
    retained->OnDefaultDeviceChanged(eRender, eConsole, L"B");
    retained.Reset();
    expect(!capture.running(), "capture remained running after stop");
    std::scoped_lock lock(state->mutex);
    expect(state->listener == nullptr, "callback stayed registered after stop");
}

void testPooledPayloadSurvivesStop() {
    // AudioPacket::bytes binds the process pool as its resource at construction,
    // so a packet the audio writer or the replay buffer still holds must stay
    // allocated across stop().
    std::pmr::memory_resource* const previousDefault = std::pmr::get_default_resource();
    backtrack::ensureAudioPacketPool();
    {
        auto state = std::make_shared<FakeState>();
        WasapiCapture capture;
        expect(startCapture(capture, state, AudioTrack::System, L"A"), "pool capture did not start");
        expect(state->wait([](const auto& s) { return hasPacket(s, 'A'); }), "pool capture delivered no packet");

        // this payload plays the downstream holder; 3840 is the 10 ms stereo
        // float32 class the pool warms up and lands in one chunk with small
        // capture packets.
        AudioPacket held;
        held.bytes.resize(3840, 0x5A);
        uint8_t* const payload = held.bytes.data();

        capture.stop();

        held.bytes[0] = 0x11;
        expect(held.bytes[0] == 0x11 && held.bytes[3839] == 0x5A,
            "packet payload was freed by stop() while still held downstream");
        // a released block is reused by the next allocation of its class
        // (200/200 measured on this toolchain), so getting the held address back
        // means stop() freed live packet memory. a heap that reuses nothing would
        // leave the payload intact and hide the free; so would a run that aborts
        // in the pool while unwinding, which still fails the case.
        std::pmr::vector<uint8_t> reused(3840);
        expect(reused.data() != payload,
            "stop() returned a held packet payload's block to the pool (use after free)");
    }
    std::pmr::set_default_resource(previousDefault);
}

void testUnregisterFailureKeepsCallbackSafe() {
    auto state = std::make_shared<FakeState>();
    state->failUnregister = true;
    WasapiCapture capture;
    expect(startCapture(capture, state, AudioTrack::System, L""), "unregister-failure capture did not start");
    capture.stop();
    auto retained = state->retainedListener();
    expect(retained != nullptr, "failed unregister lost the callback");
    retained->OnDefaultDeviceChanged(eRender, eConsole, L"B");
    {
        std::scoped_lock lock(state->mutex);
        state->listener = nullptr;
    }
    retained->Release(); // release the registration reference retained on failure
}

} // namespace

int main() {
    try {
        testDefaultSwitch(AudioTrack::System, eRender);
        testDefaultSwitch(AudioTrack::Microphone, eCapture);
        testExplicitDeviceRemainsPinned();
        testMissingExplicitDeviceDoesNotFallBack();
        testSwitchDuringResolution();
        testSwitchDuringPacketCallback();
        testDefaultLossAndRecovery();
        testRegistrationFailureRetries();
        testSetupFailuresRetryUntilRecovered();
        testStopDuringSetupBackoff();
        testStopDrainsPendingPacket();
        testCallbacksAcrossStop();
        testUnregisterFailureKeepsCallbackSafe();
        testPooledPayloadSurvivesStop();
        std::cout << "WasapiDefaultDeviceTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "WasapiDefaultDeviceTests failed: " << error.what() << '\n';
        return 1;
    }
}
