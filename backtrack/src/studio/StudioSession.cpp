#include "studio/StudioWindowImpl.h"

#include "audio/WasapiCapture.h"
#include "core/AppVersion.h"
#include "core/LogContext.h"
#include "core/Logger.h"
#include "platform/Win32Util.h"
#include "resource.h"
#include "ui_common/WindowTools.h"

#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <cassert>

namespace backtrack {
namespace {
constexpr UINT trayIconId = 1;
constexpr UINT trayOpen = 1;
constexpr UINT trayExit = 2;

bool appliesSettings(ControllerActionKind kind) {
    return kind == ControllerActionKind::Startup || kind == ControllerActionKind::ApplySettings;
}
}

void StudioWindow::Impl::startSession() {
    started = true;
    actionWorker = std::jthread([this](std::stop_token stop) { actionLoop(stop); });
    ControllerAction startup;
    startup.settings = saved;
    startup.hotkeysOk = hotkeys.registerHotkeys(window, registeredHotkeys(saved));
    startup.hotkeyError = hotkeys.lastErrorMessage();
    submit(std::move(startup), L"Starting capture\u2026");
    addTray();
    SetTimer(window, diagnosticsTimer, 1000, nullptr);
    // The probe enumerates audio sessions, so it runs on a slow poll.
    SetTimer(window, autoRecordTimer, 5000, nullptr);
    if (saved.checkForUpdates) {
        const auto targetWindow = window;
        releaseWorker = std::jthread([this, targetWindow] {
            try {
                setThreadDescriptionSafe(L"Backtrack update check");
                LogContext::ScopedTask task(L"update-check");
                const auto version = fetchReleaseVersion(releaseCheckTarget(), checkStopping);
                if (version && !checkStopping.load())
                    PostMessageW(targetWindow, releaseReady, isNewerVersion(*version, kCurrentAppVersion) ? TRUE : FALSE, 0);
            } catch (...) {
                Logger::instance().warning(L"ui", L"Update check failed unexpectedly");
            }
            releaseWorkerDone.store(true);
            PostMessageW(targetWindow, sessionStopped, FALSE, 0);
        });
    }
}

void StudioWindow::Impl::startDeviceDiscovery() {
    if (closing || shuttingDown) return;
    if (deviceWorker.joinable()) {
        // A quick return to Audio can use the scan that is still in flight.
        if (!deviceWorkerDone.load()) return;
        deviceWorker.join();
    }
    for (auto& endpoints : deviceEndpoints) endpoints.clear();
    deviceWorkerDone.store(false);
    try {
        deviceWorker = std::jthread([this, target = window](std::stop_token stop) {
            try {
                setThreadDescriptionSafe(L"Backtrack audio endpoints");
                LogContext::ScopedTask task(L"audio-endpoints");
                if (!stop.stop_requested()) deviceEndpoints[0] = WasapiCapture::enumerateDevices(AudioTrack::System);
                if (!stop.stop_requested()) deviceEndpoints[1] = WasapiCapture::enumerateDevices(AudioTrack::Microphone);
            } catch (...) {
                try { Logger::instance().warning(L"ui", L"Audio endpoint discovery failed"); }
                catch (...) {}
            }
            deviceWorkerDone.store(true);
            PostMessageW(target, devicesReady, 0, 0);
        });
    } catch (...) {
        Logger::instance().warning(L"ui", L"Could not start audio endpoint discovery");
    }
}

void StudioWindow::Impl::requestSessionStop() {
    if (checkStopping.exchange(true)) return;
    actionWorker.request_stop();
    deviceWorker.request_stop();
    requestLibraryStop();
    if (!window) return;
    KillTimer(window, diagnosticsTimer);
    KillTimer(window, autoRecordTimer);
    KillTimer(window, statusTimer);
    hotkeys.unregisterHotkeys(window);
}

void StudioWindow::Impl::finishClose() {
    if (!closing || shuttingDown || (actionWorker.joinable() && !actionWorkerDone.load()) ||
        (releaseWorker.joinable() && !releaseWorkerDone.load()) ||
        (deviceWorker.joinable() && !deviceWorkerDone.load()) || !libraryWorkerStopped()) return;
    DestroyWindow(window);
}

void StudioWindow::Impl::shutdown() {
    if (shuttingDown) return;
    shuttingDown = true;
    saveUiState();
    requestSessionStop();
    if (window) KillTimer(window, shutdownTimer);
    removeTray();
    if (actionWorker.joinable()) actionWorker.join();
    if (releaseWorker.joinable()) releaseWorker.join();
    if (deviceWorker.joinable()) deviceWorker.join();
    // the gallery owns its worker and joins it before the HWND is released
    destroyLibrary();
}

bool StudioWindow::Impl::submit(ControllerAction value, const wchar_t* text, bool pointer) {
    if (closing || shuttingDown) return false;
    if (busy && !actionWorkerDone.load()) {
        setStatus(L"Recorder is busy. Try again when it finishes.");
        playActionIndicator(MB_ICONHAND, saved.notificationSoundVolumePercent);
        return false;
    }
    const auto kind = value.kind;
    bool accepted;
    {
        std::scoped_lock lock(actionMutex);
        accepted = actionAccepting;
        if (accepted) {
            assert(!pendingAction);
            pendingAction.emplace(std::move(value));
            if (kind != ControllerActionKind::SaveReplay) recoveryResult.reset();
            autoProbe.reset();
            autoResult.reset();
            ++actionRevision;
        }
    }
    if (!accepted) {
        setStatus(L"Recorder worker has stopped. Restart Backtrack to capture again.");
        return false;
    }
    busy = true;
    actionFromPointer = pointer;
    applyingSettings = appliesSettings(kind);
    if (kind != ControllerActionKind::SaveReplay) recoveryAvailable = false;
    if (applyingSettings && (!saved.replay.enabled || !saved.gameIntegrations.leagueOfLegendsKillReminder))
        killDeadline = SteadyClock::time_point::min();
    syncRecoveryAction();
    actionCv.notify_one();
    setStatus(text);
    return true;
}

void StudioWindow::Impl::submit(ControllerActionKind kind, const wchar_t* text, bool pointer) {
    ControllerAction action;
    action.kind = kind;
    submit(std::move(action), text, pointer);
}

void StudioWindow::Impl::actionLoop(std::stop_token stop) {
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        setThreadDescriptionSafe(L"Backtrack controller");
        LogContext::ScopedTask task(L"controller-worker");
        for (;;) {
            std::optional<ControllerAction> action;
            std::optional<AudioSessionAppInfo> probe;
            uint64_t revision;
            {
                std::unique_lock lock(actionMutex);
                actionCv.wait(lock, stop, [this] { return pendingAction || autoProbe; });
                if (stop.stop_requested() && !pendingAction) break;
                action = std::exchange(pendingAction, std::nullopt);
                probe = std::exchange(autoProbe, std::nullopt);
                revision = actionRevision;
            }
            if (action) {
                const bool apply = appliesSettings(action->kind);
                if (apply) {
                    voiceEvents.store(false);
                    leagueEvents.store(false);
                }
                ControllerActionResult result;
                bool prepared = action->kind != ControllerActionKind::Startup;
                try {
                    if (!prepared) {
                        if (prepareCapture) prepareCapture();
                        controller.initialize(action->settings);
                        prepared = true;
                    }
                    result = executeRecorderAction(controller, *action);
                } catch (...) {
                    Logger::instance().error(L"ui", L"Recorder action failed unexpectedly");
                    result.kind = action->kind;
                    result.automatic = action->automatic;
                    result.status = L"Action failed unexpectedly. See Diagnostics.";
                }
                if (apply && prepared && !stop.stop_requested()) {
                    try { configureIntegrations(controller.settings()); }
                    catch (...) { Logger::instance().warning(L"ui", L"Could not read settings for integrations"); }
                }
                const bool probeRecovery = prepared && (action->kind != ControllerActionKind::SaveReplay || !result.ok);
                {
                    std::scoped_lock lock(actionMutex);
                    actionResult.emplace(std::move(result));
                }
                if (stop.stop_requested()) continue;
                PostMessageW(window, actionReady, 0, 0);
                if (!probeRecovery) continue;

                // the startup sweep can delay this probe, but not action completion
                bool recovery = false;
                try { recovery = controller.hasRecoverableRecording(); }
                catch (...) { Logger::instance().warning(L"ui", L"Could not check recording recovery availability"); }
                {
                    std::scoped_lock lock(actionMutex);
                    if (stop.stop_requested() || (pendingAction && pendingAction->kind != ControllerActionKind::SaveReplay))
                        continue;
                    recoveryResult = recovery;
                }
            } else if (probe) {
                AudioSessionAppInfo match;
                try {
                    const auto applied = controller.settings();
                    if (applied.autoRecordEnabled && !applied.autoRecordApps.empty()) {
                        std::unordered_set<std::wstring> keys;
                        keys.reserve(applied.autoRecordApps.size());
                        for (const auto& selected : applied.autoRecordApps) keys.insert(normalizedPathKey(selected.executablePath));
                        const auto matches = [&](const auto& app) { return keys.contains(normalizedPathKey(app.executablePath)); };
                        if (probe->processId && isProcessStillRunning(probe->processId) && matches(*probe)) match = *probe;
                        else {
                            const auto apps = WasapiCapture::enumerateOpenApps();
                            if (const auto it = std::ranges::find_if(apps, matches); it != apps.end()) match = *it;
                        }
                    }
                } catch (...) {
                    Logger::instance().warning(L"ui", L"Auto record app scan failed");
                    continue;
                }
                std::scoped_lock lock(actionMutex);
                if (revision != actionRevision) continue;
                autoResult.emplace(std::move(match));
            }
            if (!stop.stop_requested()) PostMessageW(window, actionReady, 0, 0);
        }
    } catch (...) {
        Logger::instance().error(L"ui", L"Recorder worker stopped after an internal error");
    }
    {
        std::scoped_lock lock(actionMutex);
        actionAccepting = false;
    }
    voiceEvents.store(false);
    leagueEvents.store(false);
    try { controller.shutdown(); }
    catch (...) { Logger::instance().error(L"ui", L"Recorder shutdown failed after an internal error"); }
    try { voice.stop(); }
    catch (...) { Logger::instance().error(L"ui", L"Voice command shutdown failed"); }
    try { league.stop(); }
    catch (...) { Logger::instance().error(L"ui", L"League integration shutdown failed"); }
    try { discord.stop(); }
    catch (...) { Logger::instance().error(L"ui", L"Discord Rich Presence shutdown failed"); }
    if (SUCCEEDED(com)) CoUninitialize();
    actionWorkerDone.store(true);
    PostMessageW(window, sessionStopped, TRUE, 0);
}

void StudioWindow::Impl::completeAction() {
    std::optional<ControllerActionResult> completion;
    std::optional<bool> recovery;
    std::optional<AudioSessionAppInfo> match;
    {
        std::scoped_lock lock(actionMutex);
        completion = std::exchange(actionResult, std::nullopt);
        recovery = std::exchange(recoveryResult, std::nullopt);
        match = std::exchange(autoResult, std::nullopt);
    }
    if (recovery) recoveryAvailable = *recovery;
    if (completion) {
        auto& result = *completion;
        const bool pointer = std::exchange(actionFromPointer, false);
        busy = false;
        applyingSettings = false;
        if (appliesSettings(result.kind)) {
            autoRecord = reconcileAutoRecordAfterSettings(autoRecord, controller.isRecording());
        } else if (result.kind == ControllerActionKind::ToggleRecording) {
            autoRecord = reconcileAutoRecordOwnership(autoRecord, result.automatic, result.startedRecording,
                                                       result.stoppedRecording, !matchedApp.executablePath.empty());
        }
        const bool recording = controller.isRecording();
        if (motions[recordGlyph].to != static_cast<float>(recording))
            setMotion(recordGlyph, static_cast<float>(recording), pointer && record);
        if (record) setText(record, recording ? L"Stop recording" : L"Start recording");
        if (!recording) audioWarning.clear();
        // A failure keeps its line until a later action succeeds; a status line
        // that expires after nine seconds would hide it.
        const bool failed = !result.ok || !result.hotkeysOk;
        if (failed) persistentWarning = result.status;
        else persistentWarning.clear();
        setStatus(!failed && result.status != L"Ready" ? std::move(result.status) : std::wstring{},
                  result.savedReplay, pointer);
        if (result.refreshLibrary && gallery) refreshLibrary();
    }
    if (completion || recovery) syncRecoveryAction();
    if (match && !busy) {
        matchedApp = std::move(*match);
        const bool running = !matchedApp.executablePath.empty();
        const bool recording = controller.isRecording();
        if (!running) autoRecord.suppressStart = false;
        if (autoRecord.owns && !recording) autoRecord = {false, running};
        if ((running && !recording && !autoRecord.suppressStart) || (!running && recording && autoRecord.owns)) {
            ControllerAction automatic;
            automatic.kind = ControllerActionKind::ToggleRecording;
            automatic.automatic = true;
            submit(std::move(automatic), running ? L"Auto record: starting recording\u2026" : L"Auto record: stopping recording\u2026");
        }
    }
}

void StudioWindow::Impl::pollAutoRecord() {
    if (closing || shuttingDown || busy || (!saved.autoRecordEnabled && !autoRecord.owns && !autoRecord.suppressStart)) return;
    {
        std::scoped_lock lock(actionMutex);
        if (!actionAccepting) return;
        autoProbe.emplace(autoRecord.owns ? matchedApp : AudioSessionAppInfo{});
    }
    actionCv.notify_one();
}

void StudioWindow::Impl::requestReplay(bool pointer) {
    ControllerAction action;
    action.kind = ControllerActionKind::SaveReplay;
    const bool tag = SteadyClock::now() <= killDeadline;
    if (tag) action.replayTag = L"Kill clip";
    if (submit(std::move(action), tag ? L"Saving kill clip\u2026" : L"Saving replay\u2026", pointer) && tag)
        killDeadline = SteadyClock::time_point::min();
}

void StudioWindow::Impl::configureIntegrations(const AppSettings& applied) {
    const auto& integration = applied.gameIntegrations;
    try {
        voice.configure(window, integration.voiceCommand, applied.replay.enabled);
        voiceEvents.store(voice.stats().enabled);
    } catch (...) { Logger::instance().warning(L"ui", L"Could not configure voice commands"); }
    try {
        if (applied.replay.enabled && integration.leagueOfLegendsKillReminder) {
            league.start();
            leagueEvents.store(true);
        } else league.stop();
    } catch (...) { Logger::instance().warning(L"ui", L"Could not configure League integration"); }
    try { discord.start(integration.discordRichPresence, integration.discordRichPresenceResetTimerOnSwitch); }
    catch (...) { Logger::instance().warning(L"ui", L"Could not configure Discord Rich Presence"); }
}

bool StudioWindow::Impl::addTray() {
    if (tray) return true;
    // The class icon is shared with every window of the class and must stay
    // undestroyed, so the tray owns a small copy of its own.
    if (!trayIcon) trayIcon = loadOwnedSmallAppIcon(reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE)), IDI_APP_ICON);
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = window;
    data.uID = trayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = trayEvent;
    data.hIcon = trayIcon ? trayIcon : LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(data.szTip, L"Backtrack");
    tray = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    return tray;
}

void StudioWindow::Impl::removeTray() {
    if (!std::exchange(tray, false)) return;
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = window;
    data.uID = trayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    if (trayIcon) DestroyIcon(std::exchange(trayIcon, nullptr));
}

void StudioWindow::Impl::restore() {
    ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(window);
}

void StudioWindow::Impl::trayMenu() {
    const auto menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, trayOpen, L"Open Backtrack");
    AppendMenuW(menu, MF_STRING, trayExit, L"Exit");
    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(window);
    const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
    DestroyMenu(menu);
    PostMessageW(window, WM_NULL, 0, 0);
    if (command == trayOpen) restore();
    else if (command == trayExit) close(true);
}

void StudioWindow::Impl::close(bool forceExit) {
    if (closing || shuttingDown || !IsWindowEnabled(window)) return;
    // Hiding to the tray keeps the window (and with it an unsaved draft) alive,
    // so there is nothing to save and no prompt to show.
    if (saved.exitToTray && !forceExit) {
        if (addTray()) ShowWindow(window, SW_HIDE);
        else {
            if (!IsWindowVisible(window)) ShowWindow(window, SW_SHOWNA);
            setStatus(L"Tray unavailable. Backtrack will stay open.");
        }
        return;
    }
    // Exit from the tray runs while the window is hidden; show it first so the
    // unsaved-changes prompt is visible and focused.
    if (forceExit) restore();
    if (!leaveSettings(!forceExit)) return;
    // Arm completion polling before stopping workers so timer failure cannot force a UI-thread join.
    if (!SetTimer(window, shutdownTimer, 50, nullptr)) {
        setStatus(L"Could not start shutdown. Try closing again.");
        return;
    }
    closing = true;
    EnableWindow(window, FALSE);
    hovered = focused = nullptr;
    setStatus(L"Finishing capture and closing\u2026");
    requestSessionStop();
    finishClose();
}

} // namespace backtrack
