#include "voice/VoiceCommandService.h"

#include "core/LogContext.h"
#include "core/Logger.h"
#include "platform/Win32Util.h"
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
    // Rebuilding the recognizer tears down the SAPI engine (including the slow
    // SetInput mic open) and joins the worker; every controller action
    // completion calls configure, so skip the rebuild when nothing changed.
    if (window == configuredWindow_ && mode == configuredMode_ && replayEnabled == configuredReplayEnabled_) {
        return;
    }
    configuredWindow_ = window;
    configuredMode_ = mode;
    configuredReplayEnabled_ = replayEnabled;

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
    setThreadDescriptionSafe(L"Backtrack voice recognition");
    LogContext::ScopedTask task(L"voice");
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comResult)) {
        ++initializationFailures_;
        setStatus(false, hrStatus(L"COM initialization", comResult));
        return;
    }

    ComPtr<ISpRecognizer> recognizer;
    ComPtr<ISpRecoContext> context;
    ComPtr<ISpRecoGrammar> grammar;
    // Name the failing step so the init error pinpoints which SAPI call broke.
    const wchar_t* initStep = L"recognizer creation";
    HRESULT hr = CoCreateInstance(CLSID_SpInprocRecognizer, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&recognizer));
    if (SUCCEEDED(hr)) {
        ComPtr<ISpAudio> audio;
        initStep = L"audio input creation";
        hr = CoCreateInstance(CLSID_SpMMAudioIn, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&audio));
        if (SUCCEEDED(hr)) {
            initStep = L"SetInput";
            hr = recognizer->SetInput(audio.Get(), TRUE);
        }
    }
    // stop() clears enabled_ before it signals stopEvent_, so this catches a stop
    // that landed while SAPI was opening the microphone.
    if (!enabled_.load()) {
        CoUninitialize();
        return;
    }
    if (SUCCEEDED(hr)) {
        initStep = L"CreateRecoContext";
        hr = recognizer->CreateRecoContext(&context);
    }
    if (SUCCEEDED(hr)) {
        initStep = L"SetNotifyWin32Event";
        hr = context->SetNotifyWin32Event();
    }
    if (SUCCEEDED(hr)) {
        initStep = L"SetInterest";
        hr = context->SetInterest(SPFEI(SPEI_RECOGNITION), SPFEI(SPEI_RECOGNITION));
    }
    if (SUCCEEDED(hr)) {
        initStep = L"CreateGrammar";
        hr = context->CreateGrammar(1, &grammar);
    }

    SPSTATEHANDLE rule = nullptr;
    if (SUCCEEDED(hr)) {
        initStep = L"GetRule";
        hr = grammar->GetRule(L"BacktrackCommand", kCommandRuleId, SPRAF_TopLevel | SPRAF_Active, TRUE, &rule);
    }
    if (SUCCEEDED(hr)) {
        initStep = L"AddWordTransition";
        hr = grammar->AddWordTransition(rule, nullptr, phraseFor(mode), L" ", SPWT_LEXICAL, 1.0f, nullptr);
    }
    if (SUCCEEDED(hr)) {
        initStep = L"Commit";
        hr = grammar->Commit(0);
    }
    if (SUCCEEDED(hr)) {
        initStep = L"SetRuleState";
        hr = grammar->SetRuleState(L"BacktrackCommand", nullptr, SPRS_ACTIVE);
    }
    if (SUCCEEDED(hr)) {
        initStep = L"SetRecoState";
        hr = recognizer->SetRecoState(SPRST_ACTIVE);
    }

    HANDLE speechEvent = context ? context->GetNotifyEventHandle() : INVALID_HANDLE_VALUE;
    if (FAILED(hr) || speechEvent == INVALID_HANDLE_VALUE) {
        ++initializationFailures_;
        const std::wstring status = FAILED(hr)
            ? hrStatus(L"Windows speech recognition", hr) + L" at " + initStep
            : L"Windows speech event unavailable";
        setStatus(false, status);
        Logger::instance().error(L"voice", status);
        CoUninitialize();
        return;
    }

    setStatus(true, std::wstring(L"listening for ") + phraseFor(mode));
    Logger::instance().info(L"voice", std::wstring(L"Offline voice command ready: ") + phraseFor(mode));
    ULONGLONG lastAcceptedTick = 0;
    // Per-cause rejection accounting: one aggregate info instead of a line per
    // noisy input; reported on a 60s cadence, on the next accepted command, or
    // at stop.
    uint64_t rejectedNoResult = 0;
    uint64_t rejectedRuleMismatch = 0;
    uint64_t rejectedLowConfidence = 0;
    uint64_t rejectedCooldown = 0;
    uint64_t rejectedWindowUnavailable = 0;
    uint64_t rejectedPostFailed = 0;
    auto reportRejections = [&] {
        const uint64_t total = rejectedNoResult + rejectedRuleMismatch + rejectedLowConfidence +
            rejectedCooldown + rejectedWindowUnavailable + rejectedPostFailed;
        if (total == 0) {
            return;
        }
        std::wstring detail;
        auto add = [&](uint64_t count, const wchar_t* cause) {
            if (count == 0) {
                return;
            }
            if (!detail.empty()) {
                detail += L", ";
            }
            detail += std::to_wstring(count) + L" " + cause;
        };
        add(rejectedNoResult, L"no result");
        add(rejectedRuleMismatch, L"wrong rule");
        add(rejectedLowConfidence, L"low confidence");
        add(rejectedCooldown, L"cooldown");
        add(rejectedWindowUnavailable, L"window unavailable");
        add(rejectedPostFailed, L"post failed");
        Logger::instance().info(L"voice",
            L"Voice recognition rejected " + std::to_wstring(total) + L" inputs (" + detail + L")");
        rejectedNoResult = 0;
        rejectedRuleMismatch = 0;
        rejectedLowConfidence = 0;
        rejectedCooldown = 0;
        rejectedWindowUnavailable = 0;
        rejectedPostFailed = 0;
    };
    auto lastRejectionReport = std::chrono::steady_clock::now();
    auto reportIfDue = [&] {
        if (std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - lastRejectionReport).count() >= 60) {
            reportRejections();
            lastRejectionReport = std::chrono::steady_clock::now();
        }
    };
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
                ++rejectedNoResult;
                reportIfDue();
                continue;
            }
            const bool ruleMatches = phrase->Rule.ulId == kCommandRuleId;
            const bool confidenceOk = phrase->Rule.Confidence != SP_LOW_CONFIDENCE;
            CoTaskMemFree(phrase);
            const ULONGLONG now = GetTickCount64();
            if (!ruleMatches) {
                ++rejected_;
                ++rejectedRuleMismatch;
                reportIfDue();
                continue;
            }
            if (!confidenceOk) {
                ++rejected_;
                ++rejectedLowConfidence;
                reportIfDue();
                continue;
            }
            if (lastAcceptedTick != 0 && now - lastAcceptedTick < kCooldownMs) {
                ++rejected_;
                ++rejectedCooldown;
                reportIfDue();
                continue;
            }

            HWND window = nullptr;
            {
                std::scoped_lock lock(stateMutex_);
                window = window_;
            }
            if (!enabled_.load() || !window) {
                ++rejected_;
                ++rejectedWindowUnavailable;
                reportIfDue();
                continue;
            }
            if (!PostMessageW(window, ui_ids::kVoiceCommandMessage, 0, 0)) {
                ++rejected_;
                ++rejectedPostFailed;
                LogContext::breadcrumb(L"voice command post to window failed");
                Logger::instance().warning(L"voice", L"Could not post voice command to the window; command lost");
                reportIfDue();
                continue;
            }
            lastAcceptedTick = now;
            ++accepted_;
            // A real command is a natural report point for what got rejected.
            reportRejections();
            lastRejectionReport = std::chrono::steady_clock::now();
        }
    }

    reportRejections();
    Logger::instance().info(L"voice", L"Voice recognition stopped");
    recognizer->SetRecoState(SPRST_INACTIVE);
    grammar->SetRuleState(L"BacktrackCommand", nullptr, SPRS_INACTIVE);
    setStatus(false, enabled_.load() ? L"stopped" : L"disabled");
    CoUninitialize();
}

} // namespace backtrack
