#include "voice/VoiceCommandService.h"

#include "core/Logger.h"
#include "ui/UiIds.h"

#include <sapi.h>
#include <sphelper.h>
#include <wrl/client.h>

#include <chrono>

namespace backtrack {

namespace {

constexpr ULONGLONG kCooldownMs = 1500;
constexpr ULONG kCommandRuleId = 1;

using Microsoft::WRL::ComPtr;
using VoiceCommandMode = GameIntegrationSettings::VoiceCommandMode;

const wchar_t* phraseFor(VoiceCommandMode mode) {
    return mode == VoiceCommandMode::RecordVideo
        ? L"Record video"
        : L"Backtrack clip that";
}

std::wstring hrStatus(const wchar_t* operation, HRESULT hr) {
    wchar_t value[16]{};
    swprintf_s(value, L"0x%08X", static_cast<unsigned int>(hr));
    return std::wstring(operation) + L" failed (" + value + L")";
}

} // namespace

VoiceCommandService::~VoiceCommandService() {
    stop();
}

void VoiceCommandService::configure(HWND window, VoiceCommandMode mode, bool replayEnabled) {
    stop();
    {
        std::scoped_lock lock(stateMutex_);
        window_ = window;
    }

    if (mode == VoiceCommandMode::Disabled) {
        setStatus(false, L"disabled");
        return;
    }
    if (!replayEnabled) {
        setStatus(false, L"inactive because replay buffer is disabled");
        return;
    }

    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) {
        ++initializationFailures_;
        setStatus(false, L"could not create stop event");
        return;
    }
    enabled_ = true;
    thread_ = std::thread(&VoiceCommandService::run, this, mode);
}

void VoiceCommandService::stop() {
    enabled_ = false;
    if (stopEvent_) {
        SetEvent(stopEvent_);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
    ready_ = false;
    std::scoped_lock lock(stateMutex_);
    window_ = nullptr;
}

VoiceCommandStats VoiceCommandService::stats() const {
    VoiceCommandStats result;
    result.enabled = enabled_.load();
    result.ready = ready_.load();
    result.accepted = accepted_.load();
    result.rejected = rejected_.load();
    result.initializationFailures = initializationFailures_.load();
    std::scoped_lock lock(stateMutex_);
    result.status = status_;
    return result;
}

void VoiceCommandService::setStatus(bool ready, std::wstring status) {
    ready_ = ready;
    std::scoped_lock lock(stateMutex_);
    status_ = std::move(status);
}

void VoiceCommandService::run(VoiceCommandMode mode) {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comResult)) {
        ++initializationFailures_;
        setStatus(false, hrStatus(L"COM initialization", comResult));
        return;
    }

    ComPtr<ISpRecognizer> recognizer;
    ComPtr<ISpRecoContext> context;
    ComPtr<ISpRecoGrammar> grammar;
    HRESULT hr = CoCreateInstance(CLSID_SpInprocRecognizer, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&recognizer));
    if (SUCCEEDED(hr)) {
        ComPtr<ISpAudio> audio;
        hr = CoCreateInstance(CLSID_SpMMAudioIn, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&audio));
        if (SUCCEEDED(hr)) {
            hr = recognizer->SetInput(audio.Get(), TRUE);
        }
    }
    if (SUCCEEDED(hr)) {
        hr = recognizer->CreateRecoContext(&context);
    }
    if (SUCCEEDED(hr)) {
        hr = context->SetNotifyWin32Event();
    }
    if (SUCCEEDED(hr)) {
        hr = context->SetInterest(SPFEI(SPEI_RECOGNITION), SPFEI(SPEI_RECOGNITION));
    }
    if (SUCCEEDED(hr)) {
        hr = context->CreateGrammar(1, &grammar);
    }

    SPSTATEHANDLE rule = nullptr;
    if (SUCCEEDED(hr)) {
        hr = grammar->GetRule(L"BacktrackCommand", kCommandRuleId, SPRAF_TopLevel | SPRAF_Active, TRUE, &rule);
    }
    if (SUCCEEDED(hr)) {
        hr = grammar->AddWordTransition(rule, nullptr, phraseFor(mode), L" ", SPWT_LEXICAL, 1.0f, nullptr);
    }
    if (SUCCEEDED(hr)) {
        hr = grammar->Commit(0);
    }
    if (SUCCEEDED(hr)) {
        hr = grammar->SetRuleState(L"BacktrackCommand", nullptr, SPRS_ACTIVE);
    }
    if (SUCCEEDED(hr)) {
        hr = recognizer->SetRecoState(SPRST_ACTIVE);
    }

    HANDLE speechEvent = context ? context->GetNotifyEventHandle() : INVALID_HANDLE_VALUE;
    if (FAILED(hr) || speechEvent == INVALID_HANDLE_VALUE) {
        ++initializationFailures_;
        const std::wstring status = FAILED(hr) ? hrStatus(L"Windows speech recognition", hr) : L"Windows speech event unavailable";
        setStatus(false, status);
        Logger::instance().warning(L"voice", status);
        CoUninitialize();
        return;
    }

    setStatus(true, std::wstring(L"listening for ") + phraseFor(mode));
    Logger::instance().info(L"voice", std::wstring(L"Offline voice command ready: ") + phraseFor(mode));
    ULONGLONG lastAcceptedTick = 0;
    HANDLE events[] = {stopEvent_, speechEvent};
    while (true) {
        const DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) {
            break;
        }
        if (wait != WAIT_OBJECT_0 + 1) {
            ++initializationFailures_;
            setStatus(false, L"Windows speech event wait failed");
            break;
        }

        CSpEvent event;
        while (event.GetFrom(context.Get()) == S_OK) {
            if (event.eEventId != SPEI_RECOGNITION) {
                continue;
            }
            ComPtr<ISpRecoResult> result = event.RecoResult();
            SPPHRASE* phrase = nullptr;
            if (!result || FAILED(result->GetPhrase(&phrase)) || !phrase) {
                ++rejected_;
                continue;
            }
            const bool ruleMatches = phrase->Rule.ulId == kCommandRuleId;
            const bool confidenceOk = phrase->Rule.Confidence != SP_LOW_CONFIDENCE;
            CoTaskMemFree(phrase);
            const ULONGLONG now = GetTickCount64();
            if (!ruleMatches || !confidenceOk || (lastAcceptedTick != 0 && now - lastAcceptedTick < kCooldownMs)) {
                ++rejected_;
                continue;
            }

            HWND window = nullptr;
            {
                std::scoped_lock lock(stateMutex_);
                window = window_;
            }
            if (!enabled_.load() || !window || !PostMessageW(window, ui_ids::kVoiceCommandMessage, 0, 0)) {
                ++rejected_;
                continue;
            }
            lastAcceptedTick = now;
            ++accepted_;
        }
    }

    recognizer->SetRecoState(SPRST_INACTIVE);
    grammar->SetRuleState(L"BacktrackCommand", nullptr, SPRS_INACTIVE);
    setStatus(false, enabled_.load() ? L"stopped" : L"disabled");
    CoUninitialize();
}

} // namespace backtrack
