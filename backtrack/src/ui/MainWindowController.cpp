#include "ui/MainWindow.h"
#include "ui/UiHelpers.h"
#include "ui/UiIds.h"

#include "audio/WasapiCapture.h"
#include "core/Logger.h"
#include "platform/Win32Util.h"
#include "resource.h"
#include "settings/SettingsStore.h"

#include <CommCtrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace backtrack {
void MainWindow::handleHotkey(int id) {
    if (id == HotkeyService::kStartStopId) {
        ControllerAction action;
        action.kind = ControllerActionKind::ToggleRecording;
        if (queueControllerAction(std::move(action), L"Recorder is busy; hotkey ignored")) {
            setStatus(controller_.isRecording() ? L"Stopping recording..." : L"Starting recording...");
        }
    } else if (id == HotkeyService::kSaveReplayId) {
        requestSaveReplay(L"hotkey");
    }
}

void MainWindow::requestSaveReplay(const wchar_t* source) {
    ControllerAction action;
    action.kind = ControllerActionKind::SaveReplay;
    const auto now = SteadyClock::now();
    if (killClipTagDeadline_ != SteadyClock::time_point::min()) {
        if (now <= killClipTagDeadline_) {
            action.replayTag = L"Kill clip";
        } else {
            killClipTagDeadline_ = SteadyClock::time_point::min();
        }
    }
    const bool taggingKillClip = !action.replayTag.empty();
    const std::wstring busyStatus = std::wstring(L"Recorder is busy; replay ") + source + L" ignored";
    if (queueControllerAction(std::move(action), busyStatus)) {
        if (taggingKillClip) {
            killClipTagDeadline_ = SteadyClock::time_point::min();
        }
        Logger::instance().info(L"ui", std::wstring(L"Replay requested by ") + source);
        setStatus(taggingKillClip ? L"Saving kill clip..." : L"Saving replay...");
    }
}

void MainWindow::updateGameIntegrations() {
    // Snapshot only: assigning settings_/savedSettings_ here would clobber
    // unsaved edits stashed in settings_ and reset the dirty-detection
    // baseline whenever an action completes while the user is mid-edit.
    // applyVisibleSettings already keeps both in sync on Save.
    const AppSettings current = controller_.settings();
    const bool leagueEnabled =
        current.replay.enabled &&
        current.gameIntegrations.leagueOfLegendsKillReminder;
    if (leagueEnabled) {
        leagueIntegration_.start();
    } else {
        leagueIntegration_.stop();
    }
    if (current.gameIntegrations.discordRichPresence !=
        GameIntegrationSettings::DiscordRichPresenceMode::Off) {
        discordRichPresence_.start(
            current.gameIntegrations.discordRichPresence,
            current.gameIntegrations.discordRichPresenceResetTimerOnSwitch);
    } else {
        discordRichPresence_.stop();
    }
    voiceCommandService_.configure(
        window_,
        current.gameIntegrations.voiceCommand,
        current.replay.enabled);
}

void MainWindow::stopGameIntegrations() {
    voiceCommandService_.stop();
    leagueIntegration_.stop();
    discordRichPresence_.stop();
}

void MainWindow::handleLeagueKillDetected() {
    // Snapshot for the gate/volume readout; never overwrite the user's
    // in-memory settings from the controller mid-edit (see updateGameIntegrations).
    const AppSettings current = controller_.settings();
    if (!current.replay.enabled || !current.gameIntegrations.leagueOfLegendsKillReminder) {
        return;
    }

    killClipTagDeadline_ = SteadyClock::now() + std::chrono::seconds(6);
    Logger::instance().info(L"ui", L"League of Legends kill reminder triggered; replay saves are taggable for 6 seconds");
    playActionIndicator(MB_ICONASTERISK, current.notificationSoundVolumePercent);
    setStatus(L"Kill detected; press replay hotkey within 6 seconds to tag it");
}

std::optional<std::wstring> MainWindow::runningAutoRecordAppName() const {
    if (settings_.autoRecordApps.empty()) {
        return std::nullopt;
    }
    std::unordered_set<std::wstring> keys;
    keys.reserve(settings_.autoRecordApps.size());
    for (const auto& app : settings_.autoRecordApps) {
        keys.insert(normalizedPathKey(app.executablePath));
    }
    for (const auto& open : WasapiCapture::enumerateOpenApps()) {
        if (keys.contains(normalizedPathKey(open.executablePath))) {
            return open.name.empty() ? std::optional<std::wstring>(appNameFromPath(open.executablePath))
                                     : std::optional<std::wstring>(open.name);
        }
    }
    return std::nullopt;
}

void MainWindow::startRecordingForAutoRecord(const std::wstring& appName) {
    ControllerAction action;
    action.kind = ControllerActionKind::ToggleRecording;
    if (queueControllerAction(std::move(action), L"Recorder is busy; auto record start ignored")) {
        autoRecordOwnsRecording_ = true;
        Logger::instance().info(L"ui", L"Auto record: starting recording for " + appName);
        setStatus(L"Auto record: starting recording (" + appName + L")");
    }
}

void MainWindow::stopRecordingForAutoRecord() {
    const bool ownsRecording = autoRecordOwnsRecording_;
    // Clear ownership up front so the completion poll cannot mistake this stop
    // for a user-initiated one and suppress the next launch.
    autoRecordOwnsRecording_ = false;
    ControllerAction action;
    action.kind = ControllerActionKind::ToggleRecording;
    if (!queueControllerAction(std::move(action), L"Recorder is busy; auto record stop ignored")) {
        // Keep ownership so the next tick retries the stop.
        autoRecordOwnsRecording_ = ownsRecording;
        return;
    }
    Logger::instance().info(L"ui", L"Auto record: app closed; stopping recording");
    setStatus(L"Auto record: stopping recording");
}

void MainWindow::pollAutoRecord() {
    if (controllerActionPending_.load(std::memory_order_acquire)) {
        // Judge state once the in-flight action finishes; the next tick handles it.
        return;
    }

    const bool featureOn =
        settings_.autoRecordEnabled && !settings_.autoRecordApps.empty();
    const auto runningName = runningAutoRecordAppName();
    const bool running = featureOn && runningName.has_value();
    const bool previousRunning = std::exchange(autoRecordAppRunning_, running);

    if (previousRunning && !running) {
        // Last selected app exited (or feature turned off): re-arm so the next
        // launch triggers again even after a user override.
        autoRecordSuppressStart_ = false;
    }

    // Recording ended without auto record stopping it (hotkey or failure):
    // drop ownership and hold off restarting while the app stays open. Suppress
    // only applies while a selected app is actually running; if the app also
    // exited this tick, the re-arm above must survive (the next launch should
    // trigger even though the user stopped the recording).
    if (autoRecordOwnsRecording_ && !controller_.isRecording()) {
        autoRecordOwnsRecording_ = false;
        autoRecordSuppressStart_ = running;
    }

    if (running && !controller_.isRecording() && !autoRecordSuppressStart_) {
        startRecordingForAutoRecord(runningName.value_or(L"selected app"));
        return;
    }
    if (!running && controller_.isRecording() && autoRecordOwnsRecording_) {
        stopRecordingForAutoRecord();
    }
}

bool MainWindow::handleShortcut(MSG& message) {
    if (message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN) {
        return false;
    }

    HWND focused = GetFocus();
    if (focused == recordHotkey_ || focused == replayHotkey_) {
        if (isModifierKey(message.wParam)) {
            return true;
        }

        uint32_t modifiers = 0;
        uint32_t virtualKey = 0;
        if (message.wParam != VK_BACK && message.wParam != VK_DELETE) {
            modifiers = currentModifierState();
            virtualKey = static_cast<uint32_t>(message.wParam);
            if (!isSafeGlobalHotkey(modifiers, virtualKey)) {
                setStatus(L"Hotkeys for normal keys need Ctrl or Alt");
                return true;
            }
        }

        if (focused == recordHotkey_) {
            recordHotkeyModifiers_ = modifiers;
            recordHotkeyVirtualKey_ = virtualKey;
        } else {
            replayHotkeyModifiers_ = modifiers;
            replayHotkeyVirtualKey_ = virtualKey;
        }
        setText(focused, hotkeyDisplay(modifiers, virtualKey));
        markSettingsDirty();
        return true;
    }

    if (page_ != Page::Library) {
        return false;
    }

    if (message.wParam == VK_F2 && renameEdit_) {
        const auto path = selectedClipPath();
        if (!path.empty()) {
            setText(renameEdit_, path.stem().wstring());
        }
        SetFocus(renameEdit_);
        SendMessageW(renameEdit_, EM_SETSEL, 0, -1);
        return true;
    }

    if (message.wParam == VK_RETURN && renameEdit_ && GetFocus() == renameEdit_) {
        renameSelectedClip();
        return true;
    }

    if (GetFocus() != renameEdit_) {
        if (message.wParam == VK_DELETE) {
            deleteSelectedClip();
            return true;
        }
        if (message.wParam == VK_F5 || (isControlDown() && message.wParam == L'R')) {
            refreshClips();
            return true;
        }
        if (isControlDown() && message.wParam == L'O') {
            openSelectedClip();
            return true;
        }
    }

    return false;
}

bool MainWindow::addTrayIcon() {
    if (trayVisible_) {
        return true;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = reinterpret_cast<HICON>(GetClassLongPtrW(window_, GCLP_HICON));
    if (!data.hIcon) {
        data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    lstrcpynW(data.szTip, L"Backtrack", static_cast<int>(_countof(data.szTip)));
    trayVisible_ = Shell_NotifyIconW(NIM_ADD, &data) == TRUE;
    return trayVisible_;
}

void MainWindow::removeTrayIcon() {
    if (!trayVisible_) {
        return;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    trayVisible_ = false;
}

void MainWindow::restoreFromTray() {
    ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOWNORMAL);
    BringWindowToTop(window_);
    SetForegroundWindow(window_);
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_FRAME);
}

void MainWindow::showTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    AppendMenuW(menu, MF_STRING, kTrayOpenId, L"Open Backtrack");
    AppendMenuW(menu, MF_STRING, kTrayExitId, L"Exit");

    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(window_);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr);
    DestroyMenu(menu);

    if (command == kTrayOpenId) {
        restoreFromTray();
    } else if (command == kTrayExitId) {
        exitFromTray_ = true;
        removeTrayIcon();
        DestroyWindow(window_);
    }
}

} // namespace backtrack
