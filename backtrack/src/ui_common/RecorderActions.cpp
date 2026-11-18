#include "ui_common/RecorderActions.h"
#include "ui_common/WindowTools.h"

#include "app/RecorderController.h"
#include "clips/ClipManager.h"
#include "core/AppVersion.h"
#include "core/Logger.h"
#include "integrations/WinHttpJson.h"
#include "platform/Win32Util.h"

namespace backtrack {
namespace {

constexpr wchar_t kGithubHost[] = L"api.github.com";
constexpr wchar_t kLatestReleasePath[] = L"/repos/gyatstian/backtrack/releases/latest";
constexpr auto kUpdateCheckTimeout = std::chrono::seconds(5);

} // namespace

std::optional<std::wstring> fetchReleaseVersion(
    const UpdateCheckTarget& target, const std::atomic<bool>& stopping) {
    if (stopping.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    // Carries the real build so a triage log identifies the version that made
    // the request; a literal here drifts from kCurrentAppVersion on release.
    const std::wstring userAgent = std::wstring(L"Backtrack/") + std::wstring(kCurrentAppVersion);
    auto handles = std::make_shared<AsyncHttpHandles>();
    handles->session.reset(WinHttpOpen(
        userAgent.c_str(), target.accessType, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
    if (!handles->session) {
        return std::nullopt;
    }

    WinHttpSetTimeouts(handles->session.value, 2000, 2000, 2000, 2000);
    handles->connection.reset(WinHttpConnect(handles->session.value, target.host.c_str(), target.port, 0));
    if (!handles->connection) {
        return std::nullopt;
    }

    const std::wstring headers =
        L"User-Agent: " + userAgent + L"\r\n"
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n";
    const CancellableHttpSession session{handles, &stopping, target.timeout, target.secure};
    // Internet host: default proxy and real cert validation (no cert ignore).
    const auto body = httpGetBody(session, handles->connection.value, kLatestReleasePath, headers.c_str(), false, L"ui");
    if (!body) {
        return std::nullopt;
    }
    return jsonStringField(*body, "tag_name");
}

UpdateCheckTarget releaseCheckTarget() {
    return {kGithubHost, INTERNET_DEFAULT_HTTPS_PORT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, true, kUpdateCheckTimeout};
}

ControllerActionResult executeRecorderAction(RecorderController& controller, const ControllerAction& action) {
    ControllerActionResult result;
    result.kind = action.kind;
    result.automatic = action.automatic;

    switch (action.kind) {
    case ControllerActionKind::Startup:
        result.ok = controller.updateSettings(action.settings);
        result.status = result.ok
            ? L"Ready"
            : actionFailureStatus(L"Capture could not start", controller.encoderCapabilities(), controller.stats());
        result.status = withHotkeyWarning(std::move(result.status), action.hotkeysOk, action.hotkeyError);
        break;
    case ControllerActionKind::ApplySettings:
        result.ok = controller.updateSettings(action.settings);
        result.status = result.ok
            ? L"Settings saved"
            : actionFailureStatus(L"Settings saved, but capture could not restart", controller.encoderCapabilities(), controller.stats());
        result.status = withHotkeyWarning(std::move(result.status), action.hotkeysOk, action.hotkeyError);
        break;
    case ControllerActionKind::ToggleRecording:
        playActionIndicator(MB_ICONASTERISK, controller.settings().notificationSoundVolumePercent);
        if (controller.stats().recording) {
            result.clipPath = controller.stopRecording();
            result.stoppedRecording = true;
            result.ok = !result.clipPath.empty();
            result.refreshLibrary = result.ok;
            playActionIndicator(result.ok ? MB_OK : MB_ICONHAND, controller.settings().notificationSoundVolumePercent);
            const std::wstring detail = trimText(controller.lastRecordingError());
            result.status = result.ok
                ? L"Saved " + result.clipPath.filename().wstring()
                : (!detail.empty() ? detail : L"No video frames captured; clip was not created");
        } else {
            result.ok = controller.startRecording();
            result.startedRecording = result.ok;
            playActionIndicator(result.ok ? MB_OK : MB_ICONHAND, controller.settings().notificationSoundVolumePercent);
            const std::wstring detail = trimText(controller.lastRecordingError());
            result.status = result.ok
                ? L"Recording started"
                : (!detail.empty() ? L"Recording could not start: " + detail
                                   : actionFailureStatus(L"Recording could not start", controller.encoderCapabilities(), controller.stats()));
        }
        break;
    case ControllerActionKind::SaveReplay:
        playActionIndicator(MB_ICONASTERISK, controller.settings().notificationSoundVolumePercent);
        result.clipPath = controller.saveReplay();
        result.savedReplay = !result.clipPath.empty();
        if (result.savedReplay && !action.replayTag.empty()) {
            ClipManager manager(controller.settings().clipDirectory);
            if (!manager.addTag(result.clipPath, action.replayTag)) {
                Logger::instance().warning(L"ui", L"Could not tag saved replay: " + result.clipPath.wstring());
            }
        }
        result.ok = result.savedReplay;
        result.refreshLibrary = result.savedReplay;
        playActionIndicator(
            result.savedReplay ? MB_OK : MB_ICONHAND,
            controller.settings().notificationSoundVolumePercent,
            result.savedReplay ? controller.settings().customNotificationSoundPath : std::filesystem::path{});
        if (result.savedReplay) {
            result.status = action.replayTag.empty()
                ? L"Replay saved"
                : action.replayTag + L" saved";
        } else {
            const std::wstring failureSummary = action.replayTag.empty()
                ? L"Replay could not be saved"
                : action.replayTag + L" could not be saved";
            // A specific cause from the save path beats the generic backend
            // status: "disk full", "no keyframe buffered yet", an HRESULT, ...
            const std::wstring detail = trimText(controller.lastRecordingError());
            result.status = !detail.empty()
                ? failureSummary + L": " + detail
                : actionFailureStatus(
                    failureSummary.c_str(),
                    controller.encoderCapabilities(),
                    controller.stats());
        }
        break;
    case ControllerActionKind::RecoverFailedRecording:
        playActionIndicator(MB_ICONASTERISK, controller.settings().notificationSoundVolumePercent);
        result.clipPath = controller.recoverFailedRecording();
        result.recoveredRecording = !result.clipPath.empty();
        result.ok = result.recoveredRecording;
        result.refreshLibrary = result.recoveredRecording;
        playActionIndicator(result.recoveredRecording ? MB_OK : MB_ICONHAND, controller.settings().notificationSoundVolumePercent);
        {
            const std::wstring detail = trimText(controller.lastRecordingError());
            result.status = result.recoveredRecording
                ? L"Recovered " + result.clipPath.filename().wstring()
                : (!detail.empty() ? detail : L"No failed recording recovery data was found");
        }
        break;
    }

    return result;
}

} // namespace backtrack
