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
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace backtrack {
void MainWindow::readVisibleSettingsInto(AppSettings& target) {
    if (bitrateEdit_) {
        target.video.bitrateKbps = readUIntControl(bitrateEdit_, target.video.bitrateKbps);
    }
    if (fpsEdit_) {
        target.video.fps = readUIntControl(fpsEdit_, target.video.fps);
    }
    bool monitorEntrySelected = false;
    if (resolutionModeCombo_) {
        const auto selected = SendMessageW(resolutionModeCombo_, CB_GETCURSEL, 0, 0);
        // Preset indices map back through the same table the builder filled;
        // higher indices are monitor shortcuts appended after the presets.
        constexpr size_t kPresetCount = std::size(kResolutionPresetComboEntries);
        if (selected >= 0 && static_cast<size_t>(selected) < kPresetCount) {
            int mode = static_cast<int>(target.video.resolutionMode);
            if (readEnumCombo(resolutionModeCombo_, kResolutionPresetComboEntries, mode)) {
                target.video.resolutionMode = static_cast<ResolutionMode>(mode);
            }
        } else if (selected >= static_cast<LRESULT>(kPresetCount)) {
            // Monitor entry: resolution shortcut. Persist as Custom with that
            // monitor's native size. Capture target is unaffected.
            const size_t monitorIdx = static_cast<size_t>(selected) - kPresetCount;
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
        target.video.width = readUIntControl(widthEdit_, target.video.width);
    }
    if (heightEdit_ && !monitorEntrySelected) {
        target.video.height = readUIntControl(heightEdit_, target.video.height);
    }
    if (multiMonitorSupportCheck_) {
        target.multiMonitorSupport = SendMessageW(multiMonitorSupportCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (followFocusedMonitorCheck_) {
        target.followFocusedMonitor = SendMessageW(followFocusedMonitorCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (followMouseMonitorCheck_) {
        target.followMouseMonitor = target.multiMonitorSupport && target.followFocusedMonitor &&
            SendMessageW(followMouseMonitorCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (captureCursorCheck_) {
        target.captureCursor = SendMessageW(captureCursorCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (codecCombo_) {
        int codec = static_cast<int>(target.video.codec);
        if (readEnumCombo(codecCombo_, kVideoCodecComboEntries, codec)) {
            target.video.codec = static_cast<VideoCodec>(codec);
        }
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
    if (inputAutoGainCheck_) {
        target.audioInputAutoGainEnabled = SendMessageW(inputAutoGainCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
    if (inputNoiseSuppressionCheck_) {
        target.audioInputNoiseSuppressionEnabled = SendMessageW(inputNoiseSuppressionCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
#endif
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
    if (checkForUpdatesCheck_) {
        target.checkForUpdates = SendMessageW(checkForUpdatesCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (notificationSoundVolumeEdit_) {
        target.notificationSoundVolumePercent =
            readUIntControl(notificationSoundVolumeEdit_, target.notificationSoundVolumePercent);
    }
    if (customNotificationSoundEdit_) {
        target.customNotificationSoundPath = trimText(readText(customNotificationSoundEdit_));
    }
    if (encoderPresetCombo_) {
        int preset = static_cast<int>(target.video.encoderPreset);
        if (readEnumCombo(encoderPresetCombo_, kEncoderPresetComboEntries, preset)) {
            target.video.encoderPreset = static_cast<EncoderPreset>(preset);
        }
    }
    if (encoderModeCombo_) {
        int mode = static_cast<int>(target.video.encoderMode);
        if (readEnumCombo(encoderModeCombo_, kEncoderModeComboEntries, mode)) {
            target.video.encoderMode = static_cast<EncoderMode>(mode);
        }
    }
    if (encoderProfileCombo_) {
        int profile = static_cast<int>(target.video.encoderProfile);
        if (readEnumCombo(encoderProfileCombo_, kEncoderProfileComboEntries, profile)) {
            target.video.encoderProfile = static_cast<EncoderProfile>(profile);
        }
    }
    if (encoderLookaheadCheck_) {
        target.video.encoderLookahead = SendMessageW(encoderLookaheadCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (encoderLookaheadDepthEdit_) {
        target.video.encoderLookaheadDepth = readUIntControl(encoderLookaheadDepthEdit_, target.video.encoderLookaheadDepth);
    }
    if (encoderSpatialAQCheck_) {
        target.video.encoderSpatialAQ = SendMessageW(encoderSpatialAQCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (encoderAQStrengthEdit_) {
        target.video.encoderAQStrength = readUIntControl(encoderAQStrengthEdit_, target.video.encoderAQStrength);
    }
    if (encoderTemporalAQCheck_) {
        target.video.encoderTemporalAQ = SendMessageW(encoderTemporalAQCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (encoderMultipassCombo_) {
        int multipass = static_cast<int>(target.video.encoderMultipass);
        if (readEnumCombo(encoderMultipassCombo_, kEncoderMultipassComboEntries, multipass)) {
            target.video.encoderMultipass = static_cast<EncoderMultipass>(multipass);
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
        int adaptive = static_cast<int>(target.gpu.adaptiveMode);
        if (readEnumCombo(gpuAdaptiveCombo_, kGpuAdaptiveComboEntries, adaptive)) {
            target.gpu.adaptiveMode = static_cast<GpuAdaptiveMode>(adaptive);
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
        int backend = static_cast<int>(target.preferredCaptureBackend);
        if (readEnumCombo(captureMethodCombo_, kCaptureBackendComboEntries, backend)) {
            target.preferredCaptureBackend = static_cast<CaptureBackend>(backend);
        }
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
    if (replayDiskBackedCheck_) {
        target.replay.diskBackedVideo =
            SendMessageW(replayDiskBackedCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (replayDiskBackedAudioCheck_) {
        target.replay.diskBackedAudio =
            SendMessageW(replayDiskBackedAudioCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (replayRingDirEdit_) {
        // Stored raw; ensureFoldersExist validates and creates on the save path.
        target.replayRingDirectory = trimText(readText(replayRingDirEdit_));
    }
    if (leagueKillReminderCheck_) {
        target.gameIntegrations.leagueOfLegendsKillReminder =
            SendMessageW(leagueKillReminderCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (autoRecordEnabledCheck_) {
        target.autoRecordEnabled =
            SendMessageW(autoRecordEnabledCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (voiceCommandCombo_) {
        int voiceCommand = static_cast<int>(target.gameIntegrations.voiceCommand);
        if (readEnumCombo(voiceCommandCombo_, kVoiceCommandComboEntries, voiceCommand)) {
            target.gameIntegrations.voiceCommand =
                static_cast<GameIntegrationSettings::VoiceCommandMode>(voiceCommand);
        }
    }
    if (discordRichPresenceCombo_) {
        int presence = static_cast<int>(target.gameIntegrations.discordRichPresence);
        if (readEnumCombo(discordRichPresenceCombo_, kDiscordPresenceComboEntries, presence)) {
            target.gameIntegrations.discordRichPresence =
                static_cast<GameIntegrationSettings::DiscordRichPresenceMode>(presence);
        }
    }
    if (discordPresenceTimerCombo_) {
        const LRESULT selected = SendMessageW(discordPresenceTimerCombo_, CB_GETCURSEL, 0, 0);
        if (selected == 0 || selected == 1) {
            target.gameIntegrations.discordRichPresenceResetTimerOnSwitch = (selected == 0);
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
        // Stored even when empty, like the ring folder, so clearing the box is
        // a visible edit; sanitizeSettings turns the empty value back into
        // defaultClipDirectory() on save. Validated by ensureFoldersExist.
        target.clipDirectory = trimText(readText(clipFolderEdit_));
    }
    if (organizeClipsByGameCheck_) {
        target.organizeClipsByGame =
            SendMessageW(organizeClipsByGameCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    target.libraryGalleryView = libraryViewMode_ == LibraryViewMode::Gallery;
}

bool MainWindow::ensureFoldersExist() {
    // Save-path-only folder validation plus creation for the paths
    // readVisibleSettingsInto just stored. An unusable ring folder otherwise
    // reaches ReplayBuffer::openRingLocked, which degrades to RAM buffering
    // and leaves the checkbox looking healthy, so create it now rather than
    // letting the first encode push discover an unwritable location.
    auto ensureFolder = [&](const std::filesystem::path& folder,
                            const wchar_t* notFolderStatus,
                            const wchar_t* notCheckableStatus,
                            const wchar_t* notCreatableStatus) {
        std::error_code folderError;
        if (std::filesystem::exists(folder, folderError) && !std::filesystem::is_directory(folder, folderError)) {
            setStatus(notFolderStatus);
            return false;
        }
        if (folderError) {
            setStatus(notCheckableStatus);
            return false;
        }
        std::filesystem::create_directories(folder, folderError);
        if (folderError) {
            setStatus(notCreatableStatus);
            return false;
        }
        return true;
    };
    // Only checked while the feature is on - unchecking it tears this row
    // down, and failing a save over a folder that is about to be unused would
    // be wrong. The path is always stored, so re-checking restores it.
    if (!settings_.replayRingDirectory.empty() &&
        (settings_.replay.diskBackedVideo || settings_.replay.diskBackedAudio)) {
        if (!ensureFolder(
                settings_.replayRingDirectory,
                L"Replay buffer folder must be a folder, not a file",
                L"Replay buffer folder could not be checked",
                L"Replay buffer folder could not be created")) {
            return false;
        }
    }
    if (!settings_.clipDirectory.empty()) {
        if (!ensureFolder(
                settings_.clipDirectory,
                L"Clip folder must be a folder, not a file",
                L"Clip folder could not be checked",
                L"Clip folder could not be created")) {
            return false;
        }
    }
    return true;
}

void MainWindow::commitSettings() {
    settings_ = sanitizeSettings(settings_);
    updateStartupRegistration();
    settingsStore_.save(settings_);
    // Persisted: the pristine baseline the Save button diffs against, so
    // further edits reverted to these values hide it.
    savedSettings_ = settings_;
}

void MainWindow::stashVisibleSettings() {
    readVisibleSettingsInto(settings_);
    settings_ = sanitizeSettings(settings_);
}

void MainWindow::applyVisibleSettings() {
    // Busy check first: everything below persists (file, startup key, hotkeys),
    // and a save that loses the race would leave disk saved but the recorder
    // never receiving ApplySettings.
    if (controllerActionPending_.load()) {
        setStatus(L"Recorder is busy; try saving settings again after the current action finishes");
        playActionIndicator(MB_ICONHAND, settings_.notificationSoundVolumePercent);
        return;
    }
    readVisibleSettingsInto(settings_);
    if (!ensureFoldersExist()) {
        return;
    }
    commitSettings();
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
    // then compare against the pristine saved baseline. The read is pure with
    // no filesystem effects: this runs on every keystroke and must not create
    // folders.
    AppSettings candidate = settings_;
    readVisibleSettingsInto(candidate);
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
    int nextHeight = statusHeightForText(contentWidth, status);

    // Hover-driven help texts wrap to different heights; resizing (and thus a
    // full relayout) on every hover change stutters while the mouse sweeps a
    // column of controls. Keep the tallest height seen during this hover
    // session instead: growing still happens immediately (so nothing clips),
    // shrinking is deferred until the pointer leaves the help control.
    if (hoveredHelpControl_) {
        if (hoverStickyStatusHeight_ <= 0) {
            hoverStickyStatusHeight_ = previousHeight;
        }
        nextHeight = std::max(nextHeight, hoverStickyStatusHeight_);
        hoverStickyStatusHeight_ = nextHeight;
    } else {
        hoverStickyStatusHeight_ = 0;
    }

    SetWindowTextW(status_, status.c_str());
    displayedStatusText_ = status;
    if (previousHeight != nextHeight) {
        layoutWindow();
    } else {
        InvalidateRect(status_, nullptr, FALSE);
    }
}

int MainWindow::statusHeightForWidth(int width) const {
    int height = statusHeightForText(width, currentStatusDisplayText());
    // Keep other layout passes consistent with the sticky hover height so a
    // resize mid-hover does not shrink the bar under the wrapped help text.
    if (hoveredHelpControl_ && hoverStickyStatusHeight_ > 0) {
        height = std::max(height, hoverStickyStatusHeight_);
    }
    return height;
}

std::wstring MainWindow::currentStatusDisplayText() const {
    // Live audio-health warnings ride along with whatever status text is
    // showing instead of replacing it, so action feedback is never lost.
    const std::wstring base = audioLivenessWarning_.empty()
        ? statusText_
        : (statusText_.empty() || statusText_ == L"Ready"
            ? audioLivenessWarning_
            : statusText_ + L"  ·  " + audioLivenessWarning_);
    if (hoveredHelpControl_) {
        const auto help = statusHelpTexts_.find(hoveredHelpControl_);
        if (help != statusHelpTexts_.end()) {
            if (base.empty() || base == L"Ready") {
                return help->second;
            }
            if (base == help->second) {
                return base;
            }
            return base + L"  ·  " + help->second;
        }
    }
    return base;
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

void MainWindow::refreshAvailableApps(HWND combo, std::vector<AudioSessionAppInfo>& available, bool hasMuteToggle) {
    // Sound separation watches audio sessions as well as visible windows;
    // auto record only matches visible windows by executable path.
    std::map<std::wstring, AudioSessionAppInfo> appsByPath;
    auto addApps = [&](std::vector<AudioSessionAppInfo> apps) {
        for (auto& app : apps) {
            const std::wstring key = normalizedPathKey(app.executablePath);
            if (key.empty() || appsByPath.contains(key)) {
                continue;
            }
            appsByPath.emplace(key, std::move(app));
        }
    };
    if (hasMuteToggle) {
        addApps(WasapiCapture::enumerateAudioSessionApps());
    }
    addApps(WasapiCapture::enumerateOpenApps());

    available.clear();
    available.reserve(appsByPath.size());
    for (auto& [_, app] : appsByPath) {
        available.push_back(std::move(app));
    }
    std::sort(available.begin(), available.end(), [](const AudioSessionAppInfo& left, const AudioSessionAppInfo& right) {
        return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
    });

    if (combo) {
        ScopedRedrawLock redraw(combo);
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Choose active app"));
        for (const auto& app : available) {
            std::wstring label = app.name.empty() ? appNameFromPath(app.executablePath) : app.name;
            const std::wstring fileName = app.executablePath.filename().wstring();
            if (!fileName.empty() && label.find(fileName) == std::wstring::npos) {
                label += L" (" + fileName + L")";
            }
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
    }

    if (!buildingPage_) {
        setStatus(L"Found " + std::to_wstring(available.size()) +
                  (available.size() == 1 ? L" selectable app" : L" selectable apps"));
    }
}

const AudioSessionAppInfo* MainWindow::selectedAvailableApp(
    HWND combo, const std::vector<AudioSessionAppInfo>& available) {
    if (!combo) {
        return nullptr;
    }
    const auto selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selected <= 0) {
        return nullptr;
    }
    const size_t index = static_cast<size_t>(selected - 1);
    return index < available.size() ? &available[index] : nullptr;
}

bool MainWindow::pickAppExecutable(std::filesystem::path& out) {
    COMDLG_FILTERSPEC filters[] = {
        {L"Applications", L"*.exe"},
        {L"All files", L"*.*"},
    };
    return pickFileForOpen(window_, L"Choose app executable", filters, static_cast<UINT>(_countof(filters)), L"exe", out);
}

bool MainWindow::validateAppExecutablePath(const std::filesystem::path& executablePath) {
    if (!executablePath.empty()) {
        return true;
    }
    setStatus(L"Selected app has no executable path");
    return false;
}

void MainWindow::commitAppListEdit(void (MainWindow::*rebuild)(), const wchar_t* status) {
    (this->*rebuild)();
    markSettingsDirty();
    setStatus(status);
}

void MainWindow::buildAppRowCommon(
    int x,
    int y,
    const std::wstring& displayName,
    const std::filesystem::path& executablePath,
    int removeButtonId,
    const wchar_t* removeHelp,
    bool hasMuteToggle,
    HWND& iconControl,
    HICON& icon,
    HWND& nameLabel,
    HWND& removeButton) {
    constexpr int kIconSize = 24;
    constexpr int kNameWidth = 300;
    constexpr int kRemoveWidth = 88;
    // Rows with a mute toggle reserve the leading column for it, so their
    // icon and name shift right; plain rows start at the icon.
    const int iconX = x + (hasMuteToggle ? 44 : 0);
    const int nameX = x + (hasMuteToggle ? 80 : 36);
    iconControl = addControl(L"STATIC", L"", SS_ICON, iconX, y, kIconSize, kIconSize, -1);
    icon = loadExecutableIcon(executablePath);
    if (icon) {
        SendMessageW(iconControl, STM_SETIMAGE, IMAGE_ICON, reinterpret_cast<LPARAM>(icon));
    }
    nameLabel = addControl(L"STATIC", displayName.c_str(), SS_LEFT | SS_NOPREFIX, nameX, y + 4, kNameWidth, 22, -1);
    addStatusHelp(nameLabel, executablePath.wstring());
    removeButton = addControl(
        L"BUTTON", L"Remove", BS_PUSHBUTTON | WS_TABSTOP, x + 484, y - 2, kRemoveWidth, 30, removeButtonId);
    addStatusHelp(removeButton, removeHelp);
}

void MainWindow::destroyAppRowCommon(HWND iconControl, HWND nameLabel, HWND removeButton, HICON& icon) {
    // detachPageControl also unregisters mutedLabels_, so both lists keep the
    // muted-theme membership correct through the same path.
    detachPageControl(iconControl);
    detachPageControl(nameLabel);
    detachPageControl(removeButton);
    if (icon) {
        DestroyIcon(icon);
        icon = nullptr;
    }
}

void MainWindow::resetAppRowHover() {
    // Reset hover while the old controls are still alive so they repaint
    // un-hovered; otherwise hoveredHelpControl_/hoveredButton_ dangle and the
    // status bar can stick tall until the next mousemove.
    clearButtonHover();
    hoveredHelpControl_ = nullptr;
    hoverStickyStatusHeight_ = 0;
}

template <typename Row>
void MainWindow::buildEmptyAppListRow(std::vector<Row>& rows, int y, HWND combo) {
    constexpr int kX = 224;
    Row row;
    row.nameLabel = addControl(L"STATIC", L"No apps selected", SS_LEFT, kX, y + 4, 360, 22, -1);
    rows.push_back(row);
    parkFocusOnCombo(combo);
    layoutCurrentPage();
    redrawWindowAndChildren(pageHost_);
}

void MainWindow::refreshSoundSeparationApps() {
    refreshAvailableApps(soundSeparationAppCombo_, soundSeparationAvailableApps_, /*hasMuteToggle=*/true);
}

void MainWindow::addSelectedSoundSeparationApp() {
    const auto* app = selectedAvailableApp(soundSeparationAppCombo_, soundSeparationAvailableApps_);
    if (!app) {
        return;
    }
    addSoundSeparationApp(app->name, app->executablePath);
    SendMessageW(soundSeparationAppCombo_, CB_SETCURSEL, 0, 0);
}

void MainWindow::addManualSoundSeparationApp() {
    std::filesystem::path executablePath;
    if (!pickAppExecutable(executablePath)) {
        return;
    }
    addSoundSeparationApp(appNameFromPath(executablePath), executablePath);
}

void MainWindow::addSoundSeparationApp(std::wstring name, std::filesystem::path executablePath) {
    if (!validateAppExecutablePath(executablePath)) {
        return;
    }

    const std::wstring key = normalizedPathKey(executablePath);
    for (auto& app : settings_.soundSeparationApps) {
        if (normalizedPathKey(app.executablePath) == key) {
            app.muted = true;
            if (app.name.empty()) {
                app.name = name.empty() ? appNameFromPath(executablePath) : std::move(name);
            }
            commitAppListEdit(&MainWindow::rebuildSoundSeparationRows, L"App already listed; muted");
            return;
        }
    }

    AppSettings::SoundSeparationApp app;
    app.name = name.empty() ? appNameFromPath(executablePath) : std::move(name);
    app.executablePath = std::move(executablePath);
    app.muted = true;
    settings_.soundSeparationApps.push_back(std::move(app));
    commitAppListEdit(&MainWindow::rebuildSoundSeparationRows, L"App added to sound separation");
}

void MainWindow::rebuildSoundSeparationRows() {
    destroySoundSeparationRows();
    if (!pageHost_ || soundSeparationRowsY_ <= 0) {
        return;
    }

    constexpr int kX = 224;
    constexpr int kMicSize = 30;
    constexpr int kMutedWidth = 70;
    constexpr int kRowHeight = 42;
    int y = soundSeparationRowsY_;

    if (settings_.soundSeparationApps.empty()) {
        buildEmptyAppListRow(soundSeparationRows_, y, soundSeparationAppCombo_);
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
        addStatusHelp(row.micButton, app.muted ? L"Unmute this app in future clips." : L"Mute this app in future clips.");

        const std::wstring name = app.name.empty() ? appNameFromPath(app.executablePath) : app.name;
        buildAppRowCommon(
            kX,
            y,
            name,
            app.executablePath,
            kSoundSeparationRemoveButtonBaseId + static_cast<int>(index),
            L"Remove this app from sound separation.",
            /*hasMuteToggle=*/true,
            row.iconControl,
            row.icon,
            row.nameLabel,
            row.removeButton);

        row.mutedLabel = addControl(
            L"STATIC",
            app.muted ? L"MUTED" : L"",
            SS_LEFT,
            kX + 396,
            y + 4,
            kMutedWidth,
            22,
            -1);
        if (app.muted && row.mutedLabel) {
            mutedLabels_.insert(row.mutedLabel);
        }

        soundSeparationRows_.push_back(row);
        y += kRowHeight;
    }

    parkFocusOnCombo(soundSeparationAppCombo_);
    layoutCurrentPage();
    redrawWindowAndChildren(pageHost_);
}

void MainWindow::parkFocusOnCombo(HWND combo) {
    if (!pageHost_ || !combo) {
        return;
    }
    HWND focused = GetFocus();
    if (!focused || !IsChild(pageHost_, focused)) {
        SetFocus(combo);
    }
}

void MainWindow::destroySoundSeparationRows() {
    resetAppRowHover();
    for (auto& row : soundSeparationRows_) {
        detachPageControl(row.micButton);
        destroyAppRowCommon(row.iconControl, row.nameLabel, row.removeButton, row.icon);
        detachPageControl(row.mutedLabel);
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
    commitAppListEdit(
        &MainWindow::rebuildSoundSeparationRows,
        settings_.soundSeparationApps[index].muted ? L"App muted" : L"App unmuted");
}

void MainWindow::removeSoundSeparationApp(size_t index) {
    if (index >= settings_.soundSeparationApps.size()) {
        return;
    }
    settings_.soundSeparationApps.erase(settings_.soundSeparationApps.begin() + static_cast<std::ptrdiff_t>(index));
    commitAppListEdit(&MainWindow::rebuildSoundSeparationRows, L"App removed from sound separation");
}

void MainWindow::rebuildAutoRecordRows() {
    destroyAutoRecordRows();
    if (!pageHost_ || autoRecordRowsY_ <= 0) {
        return;
    }

    constexpr int kX = 224;
    constexpr int kRowHeight = 42;
    int y = autoRecordRowsY_;

    if (settings_.autoRecordApps.empty()) {
        buildEmptyAppListRow(autoRecordRows_, y, autoRecordAppCombo_);
        return;
    }

    for (size_t index = 0; index < settings_.autoRecordApps.size(); ++index) {
        const auto& app = settings_.autoRecordApps[index];
        AppListRowControls row;
        const std::wstring name = app.name.empty() ? appNameFromPath(app.executablePath) : app.name;
        buildAppRowCommon(
            kX,
            y,
            name,
            app.executablePath,
            kAutoRecordRemoveButtonBaseId + static_cast<int>(index),
            L"Remove this app from auto record.",
            /*hasMuteToggle=*/false,
            row.iconControl,
            row.icon,
            row.nameLabel,
            row.removeButton);

        autoRecordRows_.push_back(row);
        y += kRowHeight;
    }

    parkFocusOnCombo(autoRecordAppCombo_);
    layoutCurrentPage();
    redrawWindowAndChildren(pageHost_);
}

void MainWindow::destroyAutoRecordRows() {
    // Same hover reset as destroySoundSeparationRows: the clicked Remove
    // button dies here, so clear hover state before it dangles.
    resetAppRowHover();
    for (auto& row : autoRecordRows_) {
        destroyAppRowCommon(row.iconControl, row.nameLabel, row.removeButton, row.icon);
    }
    autoRecordRows_.clear();
}

void MainWindow::releaseAutoRecordRowIcons() {
    for (auto& row : autoRecordRows_) {
        if (row.icon) {
            DestroyIcon(row.icon);
            row.icon = nullptr;
        }
    }
    autoRecordRows_.clear();
}

void MainWindow::removeAutoRecordApp(size_t index) {
    if (index >= settings_.autoRecordApps.size()) {
        return;
    }
    settings_.autoRecordApps.erase(settings_.autoRecordApps.begin() + static_cast<std::ptrdiff_t>(index));
    commitAppListEdit(&MainWindow::rebuildAutoRecordRows, L"App removed from auto record");
}

void MainWindow::refreshAutoRecordApps() {
    refreshAvailableApps(autoRecordAppCombo_, autoRecordAvailableApps_, /*hasMuteToggle=*/false);
}

void MainWindow::addSelectedAutoRecordApp() {
    const auto* app = selectedAvailableApp(autoRecordAppCombo_, autoRecordAvailableApps_);
    if (!app) {
        return;
    }
    addAutoRecordApp(app->name, app->executablePath);
    SendMessageW(autoRecordAppCombo_, CB_SETCURSEL, 0, 0);
}

void MainWindow::addManualAutoRecordApp() {
    std::filesystem::path executablePath;
    if (!pickAppExecutable(executablePath)) {
        return;
    }
    addAutoRecordApp(appNameFromPath(executablePath), executablePath);
}

void MainWindow::addAutoRecordApp(std::wstring name, std::filesystem::path executablePath) {
    if (!validateAppExecutablePath(executablePath)) {
        return;
    }

    const std::wstring key = normalizedPathKey(executablePath);
    for (const auto& existing : settings_.autoRecordApps) {
        if (normalizedPathKey(existing.executablePath) == key) {
            commitAppListEdit(&MainWindow::rebuildAutoRecordRows, L"App already listed");
            return;
        }
    }

    AppSettings::AutoRecordApp app;
    app.name = name.empty() ? appNameFromPath(executablePath) : std::move(name);
    app.executablePath = std::move(executablePath);
    settings_.autoRecordApps.push_back(std::move(app));
    commitAppListEdit(&MainWindow::rebuildAutoRecordRows, L"App added to auto record");
}

bool MainWindow::isSoundSeparationMutedLabel(HWND control) const {
    return control != nullptr && mutedLabels_.contains(control);
}

bool MainWindow::isLibraryTitleLabel(HWND control) const {
    if (!control || page_ != Page::Library) {
        return false;
    }
    for (const auto& item : layoutItems_) {
        if (item.sectionHeading && item.control == control) {
            return true;
        }
    }
    return false;
}
void MainWindow::updateStartupRegistration() {
    updateWindowsStartupRegistration(settings_.startWithWindowsMinimized);
}

void MainWindow::browseClipFolder() {
    std::wstring path;
    if (!pickFolder(window_, L"Choose where Backtrack saves clips", path)) {
        return;
    }
    setText(clipFolderEdit_, path);
    markSettingsDirty();
}

void MainWindow::browseRingDirectory() {
    if (!replayRingDirEdit_) {
        return;
    }
    std::wstring path;
    if (!pickFolder(window_, L"Choose where the replay ring files are kept", path)) {
        return;
    }
    setText(replayRingDirEdit_, path);
    markSettingsDirty();
}

void MainWindow::browseCustomNotificationSound() {
    COMDLG_FILTERSPEC filters[] = {
        {L"WAV files", L"*.wav"},
        {L"All files", L"*.*"},
    };
    std::filesystem::path path;
    if (!pickFileForOpen(window_, L"Choose custom notification sound", filters, static_cast<UINT>(_countof(filters)), L"wav", path)) {
        return;
    }
    if (customNotificationSoundEdit_) {
        setText(customNotificationSoundEdit_, path.wstring());
        markSettingsDirty();
    }
}

} // namespace backtrack
