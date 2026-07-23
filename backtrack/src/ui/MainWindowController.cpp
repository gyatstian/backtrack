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
            setStatus(controller_.stats().recording ? L"Stopping recording..." : L"Starting recording...");
        }
    } else if (id == HotkeyService::kSaveReplayId) {
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
        if (queueControllerAction(std::move(action), L"Recorder is busy; replay hotkey ignored")) {
            if (taggingKillClip) {
                killClipTagDeadline_ = SteadyClock::time_point::min();
            }
            setStatus(taggingKillClip ? L"Saving kill clip..." : L"Saving replay...");
        }
    }
}

void MainWindow::updateGameIntegrations() {
    settings_ = controller_.settings();
    const bool leagueEnabled =
        settings_.replay.enabled &&
        settings_.gameIntegrations.leagueOfLegendsKillReminder;
    if (leagueEnabled) {
        leagueIntegration_.start();
    } else {
        leagueIntegration_.stop();
    }
}

void MainWindow::stopGameIntegrations() {
    leagueIntegration_.stop();
}

void MainWindow::handleLeagueKillDetected() {
    settings_ = controller_.settings();
    if (!settings_.replay.enabled || !settings_.gameIntegrations.leagueOfLegendsKillReminder) {
        return;
    }

    killClipTagDeadline_ = SteadyClock::now() + std::chrono::seconds(6);
    Logger::instance().info(L"League of Legends kill reminder triggered; replay saves are taggable for 6 seconds");
    playActionIndicator(MB_ICONASTERISK, settings_.notificationSoundVolumePercent);
    setStatus(L"Kill detected; press replay hotkey within 6 seconds to tag it");
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

void MainWindow::addTrayIcon() {
    if (trayVisible_) {
        return;
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
