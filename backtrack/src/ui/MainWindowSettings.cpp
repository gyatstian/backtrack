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
bool MainWindow::readVisibleSettingsInto(AppSettings& target, bool allowFilesystemEffects) {
    if (bitrateEdit_) {
        target.video.bitrateKbps = readUIntControl(bitrateEdit_, target.video.bitrateKbps);
    }
    if (fpsEdit_) {
        target.video.fps = std::max<uint32_t>(1, readUIntControl(fpsEdit_, target.video.fps));
    }
    bool monitorEntrySelected = false;
    if (resolutionModeCombo_) {
        const auto selected = SendMessageW(resolutionModeCombo_, CB_GETCURSEL, 0, 0);
        if (selected >= 0 && selected <= static_cast<LRESULT>(ResolutionMode::Custom)) {
            target.video.resolutionMode = static_cast<ResolutionMode>(selected);
        } else if (selected >= static_cast<LRESULT>(kResolutionPresetCount)) {
            // Monitor entry: resolution shortcut. Persist as Custom with that
            // monitor's native size. Capture target is unaffected.
            const size_t monitorIdx = static_cast<size_t>(selected) - kResolutionPresetCount;
            if (monitorIdx < resolutionMonitorEntries_.size()) {
                const MonitorEnumEntry& monitor = resolutionMonitorEntries_[monitorIdx];
                target.video.resolutionMode = ResolutionMode::Custom;
                target.video.width = monitor.width;
                target.video.height = monitor.height;
                monitorEntrySelected = true;
            }
        }
    }
    // When a monitor entry is selected the size comes from the monitor, not the
    // width/height edits (which still hold the previous mode's values until the
    // category body is rebuilt).
    if (widthEdit_ && !monitorEntrySelected) {
        target.video.width = std::max<uint32_t>(16, readUIntControl(widthEdit_, target.video.width));
    }
    if (heightEdit_ && !monitorEntrySelected) {
        target.video.height = std::max<uint32_t>(16, readUIntControl(heightEdit_, target.video.height));
    }
    if (multiMonitorSupportCheck_) {
        target.multiMonitorSupport = SendMessageW(multiMonitorSupportCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (followFocusedMonitorCheck_) {
        target.followFocusedMonitor = SendMessageW(followFocusedMonitorCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (followMouseMonitorCheck_) {
        target.followMouseMonitor = target.followFocusedMonitor &&
            SendMessageW(followMouseMonitorCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (captureCursorCheck_) {
        target.captureCursor = SendMessageW(captureCursorCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (codecCombo_) {
        const auto selected = SendMessageW(codecCombo_, CB_GETCURSEL, 0, 0);
        target.video.codec = selected == 1 ? VideoCodec::Hevc : VideoCodec::H264;
    }
    if (systemAudioCheck_) {
        target.captureSystemAudio = SendMessageW(systemAudioCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (microphoneCheck_) {
        target.captureMicrophone = SendMessageW(microphoneCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (outputDeviceCombo_) {
        target.audioOutputDeviceId = selectedDeviceId(outputDeviceCombo_, outputDevices_);
    }
    if (inputDeviceCombo_) {
        target.audioInputDeviceId = selectedDeviceId(inputDeviceCombo_, inputDevices_);
    }
    if (outputVolumeEdit_) {
        target.audioOutputVolumePercent = readUIntControl(outputVolumeEdit_, target.audioOutputVolumePercent);
    }
    if (inputVolumeEdit_) {
        target.audioInputVolumePercent = readUIntControl(inputVolumeEdit_, target.audioInputVolumePercent);
    }
    if (startWithWindowsCheck_) {
        target.startWithWindowsMinimized = SendMessageW(startWithWindowsCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (pruneStaleMicrophoneConsentEntriesCheck_) {
        target.pruneStaleMicrophoneConsentEntries =
            SendMessageW(pruneStaleMicrophoneConsentEntriesCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (exitToTrayCheck_) {
        target.exitToTray = SendMessageW(exitToTrayCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (notificationSoundVolumeEdit_) {
        target.notificationSoundVolumePercent =
            readUIntControl(notificationSoundVolumeEdit_, target.notificationSoundVolumePercent);
    }
    if (customNotificationSoundEdit_) {
        target.customNotificationSoundPath = trimText(readText(customNotificationSoundEdit_));
    }
    if (encoderPresetCombo_) {
        const auto selected = SendMessageW(encoderPresetCombo_, CB_GETCURSEL, 0, 0);
        if (selected >= 0 && selected <= 6) {
            target.video.encoderPreset = static_cast<EncoderPreset>(selected);
        }
    }
    if (encoderModeCombo_) {
        const auto selected = SendMessageW(encoderModeCombo_, CB_GETCURSEL, 0, 0);
        if (selected >= 0 && selected <= 4) {
            target.video.encoderMode = static_cast<EncoderMode>(selected);
        }
    }
    if (encoderProfileCombo_) {
        const auto selected = SendMessageW(encoderProfileCombo_, CB_GETCURSEL, 0, 0);
        if (selected >= 0 && selected <= 2) {
            target.video.encoderProfile = static_cast<EncoderProfile>(selected);
        }
    }
    if (encoderLookaheadCheck_) {
        target.video.encoderLookahead = SendMessageW(encoderLookaheadCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (encoderLookaheadDepthEdit_) {
        target.video.encoderLookaheadDepth = std::min<uint32_t>(31, readUIntControl(encoderLookaheadDepthEdit_, target.video.encoderLookaheadDepth));
    }
    if (encoderSpatialAQCheck_) {
        target.video.encoderSpatialAQ = SendMessageW(encoderSpatialAQCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (encoderAQStrengthEdit_) {
        target.video.encoderAQStrength = std::clamp<uint32_t>(readUIntControl(encoderAQStrengthEdit_, target.video.encoderAQStrength), 1, 15);
    }
    if (encoderTemporalAQCheck_) {
        target.video.encoderTemporalAQ = SendMessageW(encoderTemporalAQCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (encoderMultipassCombo_) {
        const auto selected = SendMessageW(encoderMultipassCombo_, CB_GETCURSEL, 0, 0);
        if (selected >= 0 && selected <= 2) {
            target.video.encoderMultipass = static_cast<EncoderMultipass>(selected);
        }
    }
    if (encoderBFramesCheck_) {
        target.video.encoderBFrames = SendMessageW(encoderBFramesCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (encoderAdaptiveBFramesCheck_) {
        target.video.encoderAdaptiveBFrames = SendMessageW(encoderAdaptiveBFramesCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (encoderAdaptiveIFramesCheck_) {
        target.video.encoderAdaptiveIFrames = SendMessageW(encoderAdaptiveIFramesCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (encoderZeroReorderDelayCheck_) {
        target.video.encoderZeroReorderDelay = SendMessageW(encoderZeroReorderDelayCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (encoderGopSecondsEdit_) {
        target.video.gopSeconds = readUIntControl(encoderGopSecondsEdit_, target.video.gopSeconds);
    }
    if (encoderReferenceFramesEdit_) {
        target.video.encoderReferenceFrames = readUIntControl(encoderReferenceFramesEdit_, target.video.encoderReferenceFrames);
    }
    if (gpuAdaptiveCombo_) {
        const auto selected = SendMessageW(gpuAdaptiveCombo_, CB_GETCURSEL, 0, 0);
        if (selected >= 0 && selected <= 2) {
            target.gpu.adaptiveMode = static_cast<GpuAdaptiveMode>(selected);
        }
    }
    if (gpuFrameQueueLimitEdit_) {
        target.gpu.frameQueueLimit = readUIntControl(gpuFrameQueueLimitEdit_, target.gpu.frameQueueLimit);
    }
    if (idleFrameCoalescingCheck_) {
        target.gpu.allowIdleFrameSkipping =
            SendMessageW(idleFrameCoalescingCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (captureMethodCombo_) {
        const auto selected = SendMessageW(captureMethodCombo_, CB_GETCURSEL, 0, 0);
        if (selected == 0) {
            target.preferredCaptureBackend = CaptureBackend::WindowsGraphicsCapture;
        } else if (selected == 1) {
            target.preferredCaptureBackend = CaptureBackend::DesktopDuplication;
        }
    }
    if (gameCaptureModeCombo_) {
        const auto selected = SendMessageW(gameCaptureModeCombo_, CB_GETCURSEL, 0, 0);
        if (selected == 0) {
            target.gameCaptureMode = GameCaptureMode::Off;
        } else if (selected == 2) {
            target.gameCaptureMode = GameCaptureMode::On;
        } else {
            target.gameCaptureMode = GameCaptureMode::Auto;
        }
    }
    if (allowAntiCheatGameCaptureCheck_) {
        target.allowAntiCheatGameCapture =
            SendMessageW(allowAntiCheatGameCaptureCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (stableMultimonitorFramesCheck_) {
        target.gpu.stableMultimonitorFrames =
            SendMessageW(stableMultimonitorFramesCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (replayEnabledCheck_) {
        target.replay.enabled = SendMessageW(replayEnabledCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (replaySecondsEdit_) {
        target.replay.seconds = readUIntControl(replaySecondsEdit_, target.replay.seconds);
    }
    if (leagueKillReminderCheck_) {
        target.gameIntegrations.leagueOfLegendsKillReminder =
            SendMessageW(leagueKillReminderCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (voiceCommandCombo_) {
        const LRESULT selected = SendMessageW(voiceCommandCombo_, CB_GETCURSEL, 0, 0);
        if (selected >= 0 && selected <= 2) {
            target.gameIntegrations.voiceCommand =
                static_cast<GameIntegrationSettings::VoiceCommandMode>(selected);
        }
    }
    if (discordRichPresenceCombo_) {
        const LRESULT selected = SendMessageW(discordRichPresenceCombo_, CB_GETCURSEL, 0, 0);
        if (selected >= 0 && selected <= 2) {
            target.gameIntegrations.discordRichPresence =
                static_cast<GameIntegrationSettings::DiscordRichPresenceMode>(selected);
        }
    }
    if (soundSeparationEnabledCheck_) {
        target.soundSeparationEnabled = SendMessageW(soundSeparationEnabledCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (recordHotkey_) {
        target.hotkeys.startStopModifiers = recordHotkeyModifiers_;
        target.hotkeys.startStopVirtualKey = recordHotkeyVirtualKey_;
    }
    if (replayHotkey_) {
        target.hotkeys.saveReplayModifiers = replayHotkeyModifiers_;
        target.hotkeys.saveReplayVirtualKey = replayHotkeyVirtualKey_;
    }
    if (clipFolderEdit_) {
        const auto folder = trimText(readText(clipFolderEdit_));
        if (!folder.empty()) {
            std::error_code folderError;
            const std::filesystem::path folderPath(folder);
            if (std::filesystem::exists(folderPath, folderError) && !std::filesystem::is_directory(folderPath, folderError)) {
                setStatus(L"Clip folder must be a folder, not a file");
                return false;
            }
            if (folderError) {
                if (allowFilesystemEffects) {
                    setStatus(L"Clip folder could not be checked");
                    return false;
                }
                // During dirty-detection we only compare values; a transient
                // stat failure should not block the diff. Accept the text.
                target.clipDirectory = folder;
            } else {
                if (allowFilesystemEffects) {
                    std::filesystem::create_directories(folderPath, folderError);
                    if (folderError) {
                        setStatus(L"Clip folder could not be created");
                        return false;
                    }
                }
                target.clipDirectory = folder;
            }
        }
    }
    target.libraryGalleryView = libraryViewMode_ == LibraryViewMode::Gallery;
    return true;
}

void MainWindow::stashVisibleSettings() {
    if (!readVisibleSettingsInto(settings_)) {
        return;
    }
    settings_ = sanitizeSettings(settings_);
}

void MainWindow::applyVisibleSettings() {
    if (!readVisibleSettingsInto(settings_)) {
        return;
    }
    settings_ = sanitizeSettings(settings_);
    if (controllerActionPending_.load()) {
        setStatus(L"Recorder is busy; try saving settings again after the current action finishes");
        playActionIndicator(MB_ICONHAND, settings_.notificationSoundVolumePercent);
        return;
    }
    updateStartupRegistration();
    settingsStore_.save(settings_);
    // Settings are now persisted; this becomes the pristine baseline the Save
    // button diffs against, so further edits reverted to these values hide it.
    savedSettings_ = settings_;
    clipManager_.setDirectory(settings_.clipDirectory);
    const bool hotkeysOk = hotkeys_.registerHotkeys(window_, settings_.hotkeys);
    const std::wstring hotkeyError = hotkeys_.lastErrorMessage();
    ControllerAction action;
    action.kind = ControllerActionKind::ApplySettings;
    action.settings = settings_;
    action.hotkeysOk = hotkeysOk;
    action.hotkeyError = hotkeyError;
    if (!queueControllerAction(std::move(action), L"Recorder is busy; capture settings were not applied")) {
        return;
    }
    setStatus(L"Settings saved; applying capture changes...");
    clearSettingsDirty();
}

void MainWindow::addDirtySaveButton() {
    saveSettingsButton_ = addControl(L"BUTTON", L"Save Settings", BS_PUSHBUTTON | WS_TABSTOP, 44, 654, 150, 32, kSaveSettingsButtonId);
    if (!layoutItems_.empty()) {
        layoutItems_.back().kind = LayoutItem::Kind::Footer;
    }
    updateSaveSettingsButton();
}

void MainWindow::updateSaveSettingsButton() {
    if (!saveSettingsButton_) {
        return;
    }
    bool changed = false;
    if (settingsDirty_) {
        if (!IsWindowEnabled(saveSettingsButton_)) {
            EnableWindow(saveSettingsButton_, TRUE);
            changed = true;
        }
        changed = showWindowIfHidden(saveSettingsButton_) || changed;
    } else {
        if (IsWindowEnabled(saveSettingsButton_)) {
            EnableWindow(saveSettingsButton_, FALSE);
            changed = true;
        }
        if (IsWindowVisible(saveSettingsButton_)) {
            ShowWindow(saveSettingsButton_, SW_HIDE);
            changed = true;
        }
    }
    if (changed && pageHost_) {
        layoutCurrentPage();
    }
}

bool MainWindow::computeSettingsDirty() {
    // Overlay the currently visible controls onto a copy of the live settings
    // (which already holds non-visible fields and the sound-separation list),
    // then compare against the pristine saved baseline. Skip filesystem side
    // effects: this runs on every keystroke and must not create folders.
    AppSettings candidate = settings_;
    if (!readVisibleSettingsInto(candidate, /*allowFilesystemEffects=*/false)) {
        // Could not read a coherent snapshot (e.g. clip folder is a file);
        // treat that as dirty so the user can see/fix it via Save.
        return true;
    }
    candidate = sanitizeSettings(candidate);
    return !(candidate == sanitizeSettings(savedSettings_));
}

void MainWindow::markSettingsDirty() {
    if (buildingPage_) {
        return;
    }
    const bool dirty = computeSettingsDirty();
    if (dirty == settingsDirty_) {
        return;
    }
    settingsDirty_ = dirty;
    updateSaveSettingsButton();
    updateTabChrome();
}

void MainWindow::clearSettingsDirty() {
    if (!settingsDirty_ && saveSettingsButton_ && !IsWindowVisible(saveSettingsButton_)) {
        return;
    }
    settingsDirty_ = false;
    updateSaveSettingsButton();
    updateTabChrome();
}

bool MainWindow::promptSaveSettingsIfDirty(bool allowCancel) {
    if (!settingsDirty_) {
        return true;
    }
    stashVisibleSettings();
    const UINT flags = MB_ICONWARNING | (allowCancel ? MB_YESNOCANCEL : MB_YESNO);
    const int result = MessageBoxW(
        window_,
        L"You have unsaved settings. Save them before leaving?",
        L"Unsaved settings",
        flags);
    if (result == IDYES) {
        applyVisibleSettings();
        return !settingsDirty_;
    }
    if (result == IDNO) {
        settings_ = controller_.settings();
        savedSettings_ = settings_;
        recordHotkeyModifiers_ = settings_.hotkeys.startStopModifiers;
        recordHotkeyVirtualKey_ = settings_.hotkeys.startStopVirtualKey;
        replayHotkeyModifiers_ = settings_.hotkeys.saveReplayModifiers;
        replayHotkeyVirtualKey_ = settings_.hotkeys.saveReplayVirtualKey;
        clearSettingsDirty();
        // Cached Capture/Settings HWNDs no longer match discarded values.
        invalidatePageCache(Page::Capture);
        invalidatePageCache(Page::Settings);
        return true;
    }
    return false;
}

void MainWindow::addSettingHelp(HWND control, int x, int y, const std::wstring& text) {
    (void)x;
    (void)y;
    addStatusHelp(control, text);
}

void MainWindow::addStatusHelp(HWND control, const std::wstring& text) {
    if (control && !text.empty()) {
        statusHelpTexts_[control] = text;
        SetWindowSubclass(control, statusHelpSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
    }
}

void MainWindow::updateStatusHelp(HWND hoveredControl) {
    HWND control = hoveredControl;
    while (control && control != window_ && control != pageHost_) {
        const auto help = statusHelpTexts_.find(control);
        if (help != statusHelpTexts_.end()) {
            if (hoveredHelpControl_ != control) {
                hoveredHelpControl_ = control;
                applyStatusText(currentStatusDisplayText());
            }
            return;
        }
        control = GetParent(control);
    }

    if (hoveredHelpControl_) {
        hoveredHelpControl_ = nullptr;
        applyStatusText(currentStatusDisplayText());
    }
}

void MainWindow::applyStatusText(const std::wstring& status) {
    if (!status_) {
        return;
    }
    if (displayedStatusText_ == status) {
        return;
    }

    RECT client{};
    GetClientRect(window_, &client);
    const int contentWidth = std::max(1, static_cast<int>(client.right - client.left) - kOuterMargin * 2);
    const int previousHeight = statusHeightForText(contentWidth, displayedStatusText_);
    const int nextHeight = statusHeightForText(contentWidth, status);

    SetWindowTextW(status_, status.c_str());
    displayedStatusText_ = status;
    if (previousHeight != nextHeight) {
        layoutWindow();
    } else {
        InvalidateRect(status_, nullptr, FALSE);
    }
}

int MainWindow::statusHeightForWidth(int width) const {
    return statusHeightForText(width, currentStatusDisplayText());
}

std::wstring MainWindow::currentStatusDisplayText() const {
    if (hoveredHelpControl_) {
        const auto help = statusHelpTexts_.find(hoveredHelpControl_);
        if (help != statusHelpTexts_.end()) {
            if (statusText_.empty() || statusText_ == L"Ready") {
                return help->second;
            }
            if (statusText_ == help->second) {
                return statusText_;
            }
            return statusText_ + L"  ·  " + help->second;
        }
    }
    return statusText_;
}

int MainWindow::statusHeightForText(int width, const std::wstring& text) const {
    if (width <= 0) {
        return kStatusMinHeight;
    }

    HDC dc = GetDC(window_);
    if (!dc) {
        return kStatusMinHeight;
    }
    HGDIOBJ oldFont = nullptr;
    if (font_) {
        oldFont = SelectObject(dc, font_);
    }

    RECT rect{0, 0, std::max(24, width - kStatusPadding * 2), 0};
    DrawTextW(dc, text.c_str(), -1, &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);

    if (oldFont) {
        SelectObject(dc, oldFont);
    }
    ReleaseDC(window_, dc);
    return std::max(kStatusMinHeight, static_cast<int>(rect.bottom - rect.top) + 8);
}

void MainWindow::updateResolutionControls() {
    if (!resolutionModeCombo_) {
        return;
    }

    const auto selected = SendMessageW(resolutionModeCombo_, CB_GETCURSEL, 0, 0);
    // Monitor entries (selected >= preset count) are resolution shortcuts that
    // persist as Custom, so treat anything outside the preset range as Custom.
    const ResolutionMode mode =
        selected >= 0 && selected <= static_cast<LRESULT>(ResolutionMode::Custom)
            ? static_cast<ResolutionMode>(selected)
            : ResolutionMode::Custom;

    uint32_t presetWidth = 0;
    uint32_t presetHeight = 0;
    if (resolutionPresetSize(mode, presetWidth, presetHeight)) {
        setText(widthEdit_, std::to_wstring(presetWidth));
        setText(heightEdit_, std::to_wstring(presetHeight));
    }

    const bool followFocusedMonitor =
        followFocusedMonitorCheck_ && SendMessageW(followFocusedMonitorCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (followMouseMonitorCheck_) {
        EnableWindow(followMouseMonitorCheck_, followFocusedMonitor ? TRUE : FALSE);
        if (!followFocusedMonitor) {
            SendMessageW(followMouseMonitorCheck_, BM_SETCHECK, BST_UNCHECKED, 0);
        }
    }
    const BOOL editableSize = (mode == ResolutionMode::Custom) ? TRUE : FALSE;
    EnableWindow(widthEdit_, editableSize);
    EnableWindow(heightEdit_, editableSize);
}

void MainWindow::loadAudioDeviceCombos() {
    outputDevices_ = WasapiCapture::enumerateDevices(AudioTrack::System);
    inputDevices_ = WasapiCapture::enumerateDevices(AudioTrack::Microphone);

    if (outputDeviceCombo_) {
        ScopedRedrawLock redraw(outputDeviceCombo_);
        SendMessageW(outputDeviceCombo_, CB_RESETCONTENT, 0, 0);
        SendMessageW(outputDeviceCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Default output device"));
        for (const auto& device : outputDevices_) {
            SendMessageW(outputDeviceCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(device.name.c_str()));
        }
        SendMessageW(outputDeviceCombo_, CB_SETCURSEL, comboIndexForDevice(outputDevices_, settings_.audioOutputDeviceId), 0);
    }

    if (inputDeviceCombo_) {
        ScopedRedrawLock redraw(inputDeviceCombo_);
        SendMessageW(inputDeviceCombo_, CB_RESETCONTENT, 0, 0);
        SendMessageW(inputDeviceCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Default input device"));
        for (const auto& device : inputDevices_) {
            SendMessageW(inputDeviceCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(device.name.c_str()));
        }
        SendMessageW(inputDeviceCombo_, CB_SETCURSEL, comboIndexForDevice(inputDevices_, settings_.audioInputDeviceId), 0);
    }
}

void MainWindow::refreshSoundSeparationApps() {
    std::map<std::wstring, AudioSessionAppInfo> appsByPath;
    auto addApps = [&](std::vector<AudioSessionAppInfo> apps) {
        for (auto& app : apps) {
            const std::wstring key = executableKey(app.executablePath);
            if (key.empty() || appsByPath.contains(key)) {
                continue;
            }
            appsByPath.emplace(key, std::move(app));
        }
    };
    addApps(WasapiCapture::enumerateAudioSessionApps());
    addApps(WasapiCapture::enumerateOpenApps());

    soundSeparationAvailableApps_.clear();
    soundSeparationAvailableApps_.reserve(appsByPath.size());
    for (auto& [_, app] : appsByPath) {
        soundSeparationAvailableApps_.push_back(std::move(app));
    }
    std::sort(soundSeparationAvailableApps_.begin(), soundSeparationAvailableApps_.end(), [](const AudioSessionAppInfo& left, const AudioSessionAppInfo& right) {
        return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
    });

    if (soundSeparationAppCombo_) {
        ScopedRedrawLock redraw(soundSeparationAppCombo_);
        SendMessageW(soundSeparationAppCombo_, CB_RESETCONTENT, 0, 0);
        SendMessageW(soundSeparationAppCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Choose active app"));
        for (const auto& app : soundSeparationAvailableApps_) {
            std::wstring label = app.name.empty() ? appNameFromPath(app.executablePath) : app.name;
            const std::wstring fileName = app.executablePath.filename().wstring();
            if (!fileName.empty() && label.find(fileName) == std::wstring::npos) {
                label += L" (" + fileName + L")";
            }
            SendMessageW(soundSeparationAppCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendMessageW(soundSeparationAppCombo_, CB_SETCURSEL, 0, 0);
    }

    if (!buildingPage_) {
        setStatus(L"Found " + std::to_wstring(soundSeparationAvailableApps_.size()) +
                  (soundSeparationAvailableApps_.size() == 1 ? L" selectable app" : L" selectable apps"));
    }
}

void MainWindow::addSelectedSoundSeparationApp() {
    if (!soundSeparationAppCombo_) {
        return;
    }
    const auto selected = SendMessageW(soundSeparationAppCombo_, CB_GETCURSEL, 0, 0);
    if (selected <= 0) {
        return;
    }
    const size_t index = static_cast<size_t>(selected - 1);
    if (index >= soundSeparationAvailableApps_.size()) {
        return;
    }
    const auto& app = soundSeparationAvailableApps_[index];
    addSoundSeparationApp(app.name, app.executablePath);
    SendMessageW(soundSeparationAppCombo_, CB_SETCURSEL, 0, 0);
}

void MainWindow::addManualSoundSeparationApp() {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) {
        setStatus(L"Manual app picker could not open");
        return;
    }

    COMDLG_FILTERSPEC filters[] = {
        {L"Applications", L"*.exe"},
        {L"All files", L"*.*"},
    };
    dialog->SetTitle(L"Choose app executable");
    dialog->SetFileTypes(static_cast<UINT>(_countof(filters)), filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"exe");

    hr = dialog->Show(window_);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return;
    }
    if (FAILED(hr)) {
        setStatus(L"Manual app picker failed");
        return;
    }

    Microsoft::WRL::ComPtr<IShellItem> item;
    hr = dialog->GetResult(&item);
    if (FAILED(hr) || !item) {
        setStatus(L"No executable selected");
        return;
    }

    PWSTR rawPath = nullptr;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath);
    if (FAILED(hr) || !rawPath) {
        CoTaskMemFree(rawPath);
        setStatus(L"Selected app path could not be read");
        return;
    }

    std::filesystem::path executablePath(rawPath);
    CoTaskMemFree(rawPath);
    addSoundSeparationApp(appNameFromPath(executablePath), executablePath);
}

void MainWindow::addSoundSeparationApp(std::wstring name, std::filesystem::path executablePath) {
    if (executablePath.empty()) {
        setStatus(L"Selected app has no executable path");
        return;
    }

    const std::wstring key = executableKey(executablePath);
    for (auto& app : settings_.soundSeparationApps) {
        if (executableKey(app.executablePath) == key) {
            app.muted = true;
            if (app.name.empty()) {
                app.name = name.empty() ? appNameFromPath(executablePath) : std::move(name);
            }
            rebuildSoundSeparationRows();
            markSettingsDirty();
            setStatus(L"App already listed; muted");
            return;
        }
    }

    AppSettings::SoundSeparationApp app;
    app.name = name.empty() ? appNameFromPath(executablePath) : std::move(name);
    app.executablePath = std::move(executablePath);
    app.muted = true;
    settings_.soundSeparationApps.push_back(std::move(app));
    rebuildSoundSeparationRows();
    markSettingsDirty();
    setStatus(L"App added to sound separation");
}

void MainWindow::rebuildSoundSeparationRows() {
    destroySoundSeparationRows();
    if (!pageHost_ || soundSeparationRowsY_ <= 0) {
        return;
    }

    constexpr int kX = 224;
    constexpr int kMicSize = 30;
    constexpr int kIconSize = 24;
    constexpr int kNameWidth = 300;
    constexpr int kMutedWidth = 70;
    constexpr int kRemoveWidth = 88;
    constexpr int kRowHeight = 42;
    int y = soundSeparationRowsY_;

    if (settings_.soundSeparationApps.empty()) {
        SoundSeparationRowControls row;
        row.nameLabel = addControl(L"STATIC", L"No apps selected", SS_LEFT, kX, y + 4, 360, 22, -1);
        soundSeparationRows_.push_back(row);
        layoutCurrentPage();
        redrawWindowAndChildren(pageHost_);
        return;
    }

    for (size_t index = 0; index < settings_.soundSeparationApps.size(); ++index) {
        const auto& app = settings_.soundSeparationApps[index];
        SoundSeparationRowControls row;
        row.micButton = addControl(
            L"BUTTON",
            L"",
            BS_PUSHBUTTON | WS_TABSTOP,
            kX,
            y - 2,
            kMicSize,
            kMicSize,
            kSoundSeparationMuteButtonBaseId + static_cast<int>(index));
        addSettingHelp(row.micButton, 0, 0, app.muted ? L"Unmute this app in future clips." : L"Mute this app in future clips.");

        row.iconControl = addControl(
            L"STATIC",
            L"",
            SS_ICON,
            kX + 44,
            y,
            kIconSize,
            kIconSize,
            -1);
        row.icon = loadExecutableIcon(app.executablePath);
        if (row.icon) {
            SendMessageW(row.iconControl, STM_SETIMAGE, IMAGE_ICON, reinterpret_cast<LPARAM>(row.icon));
        }

        const std::wstring name = app.name.empty() ? appNameFromPath(app.executablePath) : app.name;
        row.nameLabel = addControl(
            L"STATIC",
            name.c_str(),
            SS_LEFT | SS_NOPREFIX,
            kX + 80,
            y + 4,
            kNameWidth,
            22,
            -1);
        addSettingHelp(row.nameLabel, 0, 0, app.executablePath.wstring());

        row.mutedLabel = addControl(
            L"STATIC",
            app.muted ? L"MUTED" : L"",
            SS_LEFT,
            kX + 396,
            y + 4,
            kMutedWidth,
            22,
            -1);

        row.removeButton = addControl(
            L"BUTTON",
            L"Remove",
            BS_PUSHBUTTON | WS_TABSTOP,
            kX + 484,
            y - 2,
            kRemoveWidth,
            30,
            kSoundSeparationRemoveButtonBaseId + static_cast<int>(index));
        addSettingHelp(row.removeButton, 0, 0, L"Remove this app from sound separation.");

        soundSeparationRows_.push_back(row);
        y += kRowHeight;
    }

    layoutCurrentPage();
    redrawWindowAndChildren(pageHost_);
}

void MainWindow::destroySoundSeparationRows() {
    auto removeControl = [&](HWND control) {
        if (!control) {
            return;
        }
        statusHelpTexts_.erase(control);
        pageControls_.erase(std::remove(pageControls_.begin(), pageControls_.end(), control), pageControls_.end());
        layoutItems_.erase(
            std::remove_if(
                layoutItems_.begin(),
                layoutItems_.end(),
                [control](const LayoutItem& item) {
                    return item.control == control;
                }),
            layoutItems_.end());
        DestroyWindow(control);
    };

    for (auto& row : soundSeparationRows_) {
        removeControl(row.micButton);
        removeControl(row.iconControl);
        removeControl(row.nameLabel);
        removeControl(row.mutedLabel);
        removeControl(row.removeButton);
        if (row.icon) {
            DestroyIcon(row.icon);
            row.icon = nullptr;
        }
    }
    soundSeparationRows_.clear();
}

void MainWindow::releaseSoundSeparationRowIcons() {
    for (auto& row : soundSeparationRows_) {
        if (row.icon) {
            DestroyIcon(row.icon);
            row.icon = nullptr;
        }
    }
    soundSeparationRows_.clear();
}

void MainWindow::toggleSoundSeparationApp(size_t index) {
    if (index >= settings_.soundSeparationApps.size()) {
        return;
    }
    settings_.soundSeparationApps[index].muted = !settings_.soundSeparationApps[index].muted;
    rebuildSoundSeparationRows();
    markSettingsDirty();
    setStatus(settings_.soundSeparationApps[index].muted ? L"App muted" : L"App unmuted");
}

void MainWindow::removeSoundSeparationApp(size_t index) {
    if (index >= settings_.soundSeparationApps.size()) {
        return;
    }
    settings_.soundSeparationApps.erase(settings_.soundSeparationApps.begin() + static_cast<std::ptrdiff_t>(index));
    rebuildSoundSeparationRows();
    markSettingsDirty();
    setStatus(L"App removed from sound separation");
}

bool MainWindow::isSoundSeparationMutedLabel(HWND control) const {
    if (!control) {
        return false;
    }
    wchar_t text[16]{};
    if (GetWindowTextW(control, text, static_cast<int>(_countof(text))) <= 0) {
        return false;
    }
    return lstrcmpW(text, L"MUTED") == 0;
}

bool MainWindow::isGameCaptureWarningLabel(HWND control) const {
    if (!control) {
        return false;
    }
    wchar_t text[32]{};
    if (GetWindowTextW(control, text, static_cast<int>(_countof(text))) <= 0) {
        return false;
    }
    return lstrcmpW(text, L"Game capture !!!") == 0;
}

void MainWindow::updateStartupRegistration() {
    updateWindowsStartupRegistration(settings_.startWithWindowsMinimized);
}

void MainWindow::browseClipFolder() {
    wchar_t displayName[MAX_PATH]{};
    BROWSEINFOW browse{};
    browse.hwndOwner = window_;
    browse.pszDisplayName = displayName;
    browse.lpszTitle = L"Choose where Backtrack saves clips";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
    if (!item) {
        return;
    }

    wchar_t path[MAX_PATH]{};
    if (SHGetPathFromIDListW(item, path)) {
        setText(clipFolderEdit_, path);
        markSettingsDirty();
    }
    CoTaskMemFree(item);
}

void MainWindow::browseCustomNotificationSound() {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) {
        setStatus(L"Sound picker could not open");
        return;
    }

    COMDLG_FILTERSPEC filters[] = {
        {L"WAV files", L"*.wav"},
        {L"All files", L"*.*"},
    };
    dialog->SetTitle(L"Choose custom notification sound");
    dialog->SetFileTypes(static_cast<UINT>(_countof(filters)), filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"wav");

    hr = dialog->Show(window_);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return;
    }
    if (FAILED(hr)) {
        setStatus(L"Sound picker failed");
        return;
    }

    Microsoft::WRL::ComPtr<IShellItem> item;
    hr = dialog->GetResult(&item);
    if (FAILED(hr) || !item) {
        setStatus(L"No sound file selected");
        return;
    }

    PWSTR rawPath = nullptr;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath);
    if (FAILED(hr) || !rawPath) {
        CoTaskMemFree(rawPath);
        setStatus(L"Selected sound path could not be read");
        return;
    }

    const std::wstring path(rawPath);
    CoTaskMemFree(rawPath);
    if (customNotificationSoundEdit_) {
        setText(customNotificationSoundEdit_, path);
        markSettingsDirty();
    }
}

} // namespace backtrack
