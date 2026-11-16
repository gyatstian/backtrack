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
namespace {
constexpr const wchar_t* kTabLabels[kTabCount] = {L"Capture", L"Library", L"Settings", L"Diagnostics"};
}

void MainWindow::buildTabs() {
    for (int index = 0; index < kTabCount; ++index) {
        HWND button = CreateWindowExW(
            0,
            L"BUTTON",
            kTabLabels[index],
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
            0,
            0,
            0,
            0,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTabButtonBaseId + index)),
            nullptr,
            nullptr);
        setDefaultFont(button);
        applyDarkTheme(button);
        tabButtons_.push_back(button);
    }
    updateTabChrome();
}

void MainWindow::storeCurrentPageCache() {
    const size_t index = static_cast<size_t>(page_);
    if (index >= pageCaches_.size()) {
        return;
    }
    auto& cache = pageCaches_[index];
    cache.host = pageHost_;
    cache.controls = pageControls_;
    cache.layoutItems = layoutItems_;
    cache.statusHelpTexts = statusHelpTexts_;
    cache.scrollY = pageScrollY_;
    cache.contentHeight = pageContentHeight_;
    cache.wheelRemainder = pageWheelRemainder_;
    cache.built = true;
}

void MainWindow::loadPageCache(Page page) {
    const size_t index = static_cast<size_t>(page);
    if (index >= pageCaches_.size()) {
        return;
    }
    auto& cache = pageCaches_[index];
    pageHost_ = cache.host;
    pageControls_ = cache.controls;
    layoutItems_ = cache.layoutItems;
    statusHelpTexts_ = cache.statusHelpTexts;
    pageScrollY_ = cache.scrollY;
    pageContentHeight_ = cache.contentHeight;
    pageWheelRemainder_ = cache.wheelRemainder;
    rebindPageControlPointers();
}

void MainWindow::resetPageControlMembers() {
    static constexpr HWND MainWindow::* kMembers[] = {
        &MainWindow::saveSettingsButton_,
        &MainWindow::startStopButton_,
        &MainWindow::bitrateEdit_,
        &MainWindow::codecCombo_,
        &MainWindow::recordHotkey_,
        &MainWindow::clipFolderEdit_,
        &MainWindow::organizeClipsByGameCheck_,
        &MainWindow::replayEnabledCheck_,
        &MainWindow::replaySecondsEdit_,
        &MainWindow::replayOverlapSecondsEdit_,
        &MainWindow::replayDiskBackedCheck_,
        &MainWindow::replayDiskBackedAudioCheck_,
        &MainWindow::replayRingDirEdit_,
        &MainWindow::replayHotkey_,
        &MainWindow::leagueKillReminderCheck_,
        &MainWindow::voiceCommandCombo_,
        &MainWindow::discordRichPresenceCombo_,
        &MainWindow::discordPresenceTimerCombo_,
        &MainWindow::statsLabel_,
        &MainWindow::capsLabel_,
        &MainWindow::diagnosticLogLabel_,
        &MainWindow::loggingEnabledCheck_,
        &MainWindow::clipList_,
        &MainWindow::listViewButton_,
        &MainWindow::galleryViewButton_,
        &MainWindow::renameEdit_,
        &MainWindow::renameEditButton_,
        &MainWindow::fpsEdit_,
        &MainWindow::resolutionModeCombo_,
        &MainWindow::widthEdit_,
        &MainWindow::heightEdit_,
        &MainWindow::multiMonitorSupportCheck_,
        &MainWindow::monitorFollowCombo_,
        &MainWindow::captureCursorCheck_,
        &MainWindow::systemAudioCheck_,
        &MainWindow::microphoneCheck_,
        &MainWindow::outputDeviceCombo_,
        &MainWindow::inputDeviceCombo_,
        &MainWindow::outputVolumeEdit_,
        &MainWindow::inputVolumeEdit_,
        &MainWindow::inputAutoGainCheck_,
#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
        &MainWindow::inputNoiseSuppressionCheck_,
#endif
        &MainWindow::startWithWindowsCheck_,
        &MainWindow::pruneStaleMicrophoneConsentEntriesCheck_,
        &MainWindow::exitToTrayCheck_,
        &MainWindow::checkForUpdatesCheck_,
        &MainWindow::notificationSoundVolumeEdit_,
        &MainWindow::customNotificationSoundEdit_,
        &MainWindow::encoderPresetCombo_,
        &MainWindow::encoderModeCombo_,
        &MainWindow::encoderProfileCombo_,
        &MainWindow::encoderLookaheadCheck_,
        &MainWindow::encoderLookaheadDepthEdit_,
        &MainWindow::encoderSpatialAQCheck_,
        &MainWindow::encoderAQStrengthEdit_,
        &MainWindow::encoderTemporalAQCheck_,
        &MainWindow::encoderMultipassCombo_,
        &MainWindow::encoderBFramesCheck_,
        &MainWindow::encoderAdaptiveBFramesCheck_,
        &MainWindow::encoderAdaptiveIFramesCheck_,
        &MainWindow::encoderZeroReorderDelayCheck_,
        &MainWindow::encoderGopSecondsEdit_,
        &MainWindow::encoderReferenceFramesEdit_,
        &MainWindow::gpuAdaptiveCombo_,
        &MainWindow::gpuFrameQueueLimitEdit_,
        &MainWindow::idleFrameCoalescingCheck_,
        &MainWindow::captureMethodCombo_,
        &MainWindow::stableMultimonitorFramesCheck_,
        &MainWindow::soundSeparationEnabledCheck_,
        &MainWindow::soundSeparationAppCombo_,
        &MainWindow::soundSeparationRefreshButton_,
        &MainWindow::soundSeparationManualButton_,
        &MainWindow::autoRecordEnabledCheck_,
        &MainWindow::autoRecordAppCombo_,
        &MainWindow::autoRecordRefreshButton_,
        &MainWindow::autoRecordManualButton_,
    };
    for (auto member : kMembers) {
        this->*member = nullptr;
    }
    settingsCategoryButtons_.clear();
    autoRecordRowsY_ = 0;
}

void MainWindow::rebindPageControlPointers() {
    resetPageControlMembers();

    if (!pageHost_) {
        return;
    }

    auto child = [this](int id) -> HWND {
        return GetDlgItem(pageHost_, id);
    };

    startStopButton_ = child(kStartStopButtonId);
    recordHotkey_ = child(kRecordHotkeyId);
    replayEnabledCheck_ = child(kReplayEnabledId);
    replaySecondsEdit_ = child(kReplaySecondsEditId);
    replayOverlapSecondsEdit_ = child(kReplayOverlapSecondsEditId);
    replayDiskBackedCheck_ = child(kReplayDiskBackedCheckId);
    replayDiskBackedAudioCheck_ = child(kReplayDiskBackedAudioCheckId);
    replayRingDirEdit_ = child(kReplayRingDirEditId);
    replayHotkey_ = child(kReplayHotkeyId);
    saveSettingsButton_ = child(kSaveSettingsButtonId);
    bitrateEdit_ = child(kBitrateEditId);
    codecCombo_ = child(kCodecComboId);
    clipFolderEdit_ = child(kClipFolderEditId);
    organizeClipsByGameCheck_ = child(kOrganizeClipsByGameCheckId);
    fpsEdit_ = child(kFpsEditId);
    resolutionModeCombo_ = child(kResolutionModeComboId);
    widthEdit_ = child(kWidthEditId);
    heightEdit_ = child(kHeightEditId);
    multiMonitorSupportCheck_ = child(kMultiMonitorSupportCheckId);
    monitorFollowCombo_ = child(kMonitorFollowComboId);
    captureCursorCheck_ = child(kCaptureCursorCheckId);
    systemAudioCheck_ = child(kSystemAudioCheckId);
    microphoneCheck_ = child(kMicrophoneCheckId);
    outputDeviceCombo_ = child(kOutputDeviceComboId);
    inputDeviceCombo_ = child(kInputDeviceComboId);
    outputVolumeEdit_ = child(kOutputVolumeEditId);
    inputVolumeEdit_ = child(kInputVolumeEditId);
    inputAutoGainCheck_ = child(kInputAutoGainCheckId);
#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
    inputNoiseSuppressionCheck_ = child(kInputNoiseSuppressionCheckId);
#endif
    startWithWindowsCheck_ = child(kStartWithWindowsCheckId);
    pruneStaleMicrophoneConsentEntriesCheck_ = child(kPruneStaleMicrophoneConsentEntriesCheckId);
    exitToTrayCheck_ = child(kExitToTrayCheckId);
    checkForUpdatesCheck_ = child(kCheckForUpdatesCheckId);
    notificationSoundVolumeEdit_ = child(kNotificationSoundVolumeEditId);
    customNotificationSoundEdit_ = child(kCustomNotificationSoundEditId);
    encoderPresetCombo_ = child(kEncoderPresetComboId);
    encoderModeCombo_ = child(kEncoderModeComboId);
    encoderProfileCombo_ = child(kEncoderProfileComboId);
    encoderLookaheadCheck_ = child(kEncoderLookaheadCheckId);
    encoderLookaheadDepthEdit_ = child(kEncoderLookaheadDepthEditId);
    encoderSpatialAQCheck_ = child(kEncoderSpatialAQCheckId);
    encoderAQStrengthEdit_ = child(kEncoderAQStrengthEditId);
    encoderTemporalAQCheck_ = child(kEncoderTemporalAQCheckId);
    encoderMultipassCombo_ = child(kEncoderMultipassComboId);
    encoderBFramesCheck_ = child(kEncoderBFramesCheckId);
    encoderAdaptiveBFramesCheck_ = child(kEncoderAdaptiveBFramesCheckId);
    encoderAdaptiveIFramesCheck_ = child(kEncoderAdaptiveIFramesCheckId);
    encoderZeroReorderDelayCheck_ = child(kEncoderZeroReorderDelayCheckId);
    encoderGopSecondsEdit_ = child(kEncoderGopSecondsEditId);
    encoderReferenceFramesEdit_ = child(kEncoderReferenceFramesEditId);
    gpuAdaptiveCombo_ = child(kGpuAdaptiveComboId);
    gpuFrameQueueLimitEdit_ = child(kGpuFrameQueueLimitEditId);
    idleFrameCoalescingCheck_ = child(kIdleFrameCoalescingCheckId);
    captureMethodCombo_ = child(kCaptureMethodComboId);
    stableMultimonitorFramesCheck_ = child(kStableMultimonitorFramesCheckId);
    soundSeparationEnabledCheck_ = child(kSoundSeparationEnabledCheckId);
    soundSeparationAppCombo_ = child(kSoundSeparationAppComboId);
    soundSeparationRefreshButton_ = child(kSoundSeparationRefreshButtonId);
    soundSeparationManualButton_ = child(kSoundSeparationManualButtonId);
    leagueKillReminderCheck_ = child(kLeagueKillReminderCheckId);
    voiceCommandCombo_ = child(kVoiceCommandComboId);
    discordRichPresenceCombo_ = child(kDiscordRichPresenceComboId);
    discordPresenceTimerCombo_ = child(kDiscordPresenceTimerComboId);
    autoRecordEnabledCheck_ = child(kAutoRecordEnabledCheckId);
    autoRecordAppCombo_ = child(kAutoRecordAppComboId);
    autoRecordRefreshButton_ = child(kAutoRecordRefreshButtonId);
    autoRecordManualButton_ = child(kAutoRecordManualButtonId);
    statsLabel_ = child(kStatsLabelId);
    capsLabel_ = child(kCapsLabelId);
    diagnosticLogLabel_ = child(kDiagnosticLogLabelId);
    loggingEnabledCheck_ = child(kLoggingEnabledCheckId);
    clipList_ = child(kClipListId);
    listViewButton_ = child(kClipListViewButtonId);
    galleryViewButton_ = child(kClipGalleryViewButtonId);
    renameEdit_ = child(kRenameEditId);
    renameEditButton_ = child(kRenameClipButtonId);

    for (int index = 0; index < kSettingsCategoryCount; ++index) {
        HWND button = child(kSettingsCategoryButtonBaseId + index);
        if (button) {
            settingsCategoryButtons_.push_back(button);
        }
    }

    if (page_ == Page::Settings && settingsCategory_ == SettingsCategory::Audio) {
        // Rows sit below the selected-apps label; fall back if layout scan fails.
        soundSeparationRowsY_ = 250;
        for (const auto& item : layoutItems_) {
            if (item.control == soundSeparationAppCombo_) {
                soundSeparationRowsY_ = static_cast<int>(item.design.bottom) + kPageRect.top + 54;
                break;
            }
        }
    }
    if (page_ == Page::Settings && settingsCategory_ == SettingsCategory::GameIntegrations) {
        // Same row placement rule as the sound separation list above.
        autoRecordRowsY_ = 250;
        for (const auto& item : layoutItems_) {
            if (item.control == autoRecordAppCombo_) {
                autoRecordRowsY_ = static_cast<int>(item.design.bottom) + kPageRect.top + 54;
                break;
            }
        }
    }
}

void MainWindow::invalidatePageCache(Page page) {
    const size_t index = static_cast<size_t>(page);
    if (index >= pageCaches_.size()) {
        return;
    }
    auto& cache = pageCaches_[index];
    if (!cache.built && cache.controls.empty()) {
        return;
    }

    const bool wasCurrent = page_ == page && pageHost_ == cache.host;
    if (wasCurrent) {
        if (pageHost_) {
            SendMessageW(pageHost_, WM_SETREDRAW, FALSE, 0);
        }
        clearPageControls();
        pageScrollY_ = 0;
        pageContentHeight_ = 0;
        pageWheelRemainder_ = 0;
        resetPageCache(cache);
        if (pageHost_) {
            SendMessageW(pageHost_, WM_SETREDRAW, TRUE, 0);
        }
        return;
    }

    for (HWND control : cache.controls) {
        if (control && IsWindow(control)) {
            DestroyWindow(control);
        }
    }
    resetPageCache(cache);
}

void MainWindow::resetPageCache(PageCache& cache) {
    cache.controls.clear();
    cache.layoutItems.clear();
    cache.statusHelpTexts.clear();
    cache.scrollY = 0;
    cache.contentHeight = 0;
    cache.wheelRemainder = 0;
    cache.built = false;
}

void MainWindow::updateTabChrome() {
    for (size_t index = 0; index < tabButtons_.size() && index < kTabCount; ++index) {
        HWND button = tabButtons_[index];
        if (!button) {
            continue;
        }
        std::wstring label = kTabLabels[index];
        if (settingsDirty_ && index == static_cast<size_t>(Page::Settings)) {
            label += L'*';
        }
        setText(button, label);
        EnableWindow(button, TRUE);
        InvalidateRect(button, nullptr, FALSE);
    }
}

void MainWindow::updateSettingsCategoryChrome() {
    for (size_t index = 0; index < settingsCategoryButtons_.size(); ++index) {
        HWND button = settingsCategoryButtons_[index];
        if (!button) {
            continue;
        }
        EnableWindow(button, TRUE);
        InvalidateRect(button, nullptr, FALSE);
    }
}

void MainWindow::switchPage(Page page) {
    const bool alreadyVisible =
        page == page_ &&
        pageCaches_[static_cast<size_t>(page)].built &&
        pageHost_ &&
        IsWindowVisible(pageHost_);
    if (alreadyVisible) {
        updateTabChrome();
        return;
    }

    if (page != page_ && settingsDirty_) {
        if (!promptSaveSettingsIfDirty(true)) {
            updateTabChrome();
            return;
        }
    }

    HWND previousHost = pageHost_;
    const Page previousPage = page_;
    if (settingsDirty_ && (previousPage == Page::Capture || previousPage == Page::Settings)) {
        stashVisibleSettings();
    }
    if (previousPage == Page::Library) {
        cancelLibraryScan();
        releaseClipThumbnailCache();
    }

    // Diagnostics has no asynchronous UI work. Library thumbnail callbacks can arrive after
    // a tab switch, so rebuild its HWND tree rather than keeping hidden controls alive.
    const bool previousCacheable = previousPage == Page::Diagnostics;
    if (previousCacheable && pageCaches_[static_cast<size_t>(previousPage)].built) {
        storeCurrentPageCache();
    } else if (previousHost) {
        SendMessageW(previousHost, WM_SETREDRAW, FALSE, 0);
        clearPageControls();
        resetPageCache(pageCaches_[static_cast<size_t>(previousPage)]);
        SendMessageW(previousHost, WM_SETREDRAW, TRUE, 0);
    }
    if (previousHost) {
        ShowWindow(previousHost, SW_HIDE);
    }

    page_ = page;
    clearButtonHover();
    hoveredHelpControl_ = nullptr;
    hoverStickyStatusHeight_ = 0;
    clipListWheelRemainder_ = 0;
    loadPageCache(page_);

    auto& cache = pageCaches_[static_cast<size_t>(page_)];
    const bool canReuse =
        cache.built &&
        page_ == Page::Diagnostics;
    if (!canReuse) {
        pageScrollY_ = 0;
        pageWheelRemainder_ = 0;
        pageContentHeight_ = 0;
        pageControls_.clear();
        layoutItems_.clear();
        statusHelpTexts_.clear();
        if (pageHost_) {
            SendMessageW(pageHost_, WM_SETREDRAW, FALSE, 0);
        }
        buildingPage_ = true;
        switch (page_) {
        case Page::Capture:
            buildCapturePage();
            break;
        case Page::Library:
            buildClipsPage();
            break;
        case Page::Settings:
            buildSettingsPage();
            break;
        case Page::Diagnostics:
            buildStatsPage();
            break;
        }
        buildingPage_ = false;
        storeCurrentPageCache();
        if (pageHost_) {
            SendMessageW(pageHost_, WM_SETREDRAW, TRUE, 0);
        }
    }

    if (pageHost_) {
        ShowWindow(pageHost_, SW_SHOW);
    }
    updateTabChrome();
    updateSaveSettingsButton();
    applyStatusText(currentStatusDisplayText());
    layoutCurrentPage();
    if (pageHost_) {
        redrawWindowAndChildren(pageHost_, true);
    }
    for (HWND button : tabButtons_) {
        InvalidateRect(button, nullptr, FALSE);
    }
    updateDiagnosticsTimer();
    if (page_ == Page::Diagnostics) {
        updateStats();
    }
}

void MainWindow::switchSettingsCategory(SettingsCategory category) {
    if (page_ != Page::Settings) {
        settingsCategory_ = category;
        switchPage(Page::Settings);
        return;
    }
    if (category == settingsCategory_) {
        updateSettingsCategoryChrome();
        return;
    }
    stashVisibleSettings();
    settingsCategory_ = category;
    rebuildSettingsCategoryBody();
}

void MainWindow::clearPageControls() {
    for (HWND control : pageControls_) {
        if (control && IsWindow(control)) {
            DestroyWindow(control);
        }
    }
    // Bulk teardown owns this too: any future invalidate-without-layout would
    // otherwise leave the previous page's cards painting behind the new one.
    sectionPanels_.clear();
    releaseSoundSeparationRowIcons();
    releaseAutoRecordRowIcons();
    pageControls_.clear();
    layoutItems_.clear();
    statusHelpTexts_.clear();
    mutedLabels_.clear();
    hoveredHelpControl_ = nullptr;
    hoverStickyStatusHeight_ = 0;
    clearButtonHover();
    resetPageControlMembers();
    autoRecordAvailableApps_.clear();
    clipListItemHeight_ = 0;
}

void MainWindow::clearSettingsBodyControls() {
    std::unordered_set<HWND> keep;
    keep.reserve(settingsCategoryButtons_.size() + 1);
    for (HWND button : settingsCategoryButtons_) {
        if (button) {
            keep.insert(button);
        }
    }
    if (saveSettingsButton_) {
        keep.insert(saveSettingsButton_);
    }

    std::vector<HWND> keptControls;
    std::vector<LayoutItem> keptLayout;
    std::unordered_map<HWND, std::wstring> keptHelp;
    keptControls.reserve(keep.size());
    for (HWND control : pageControls_) {
        if (keep.contains(control)) {
            keptControls.push_back(control);
            continue;
        }
        if (control && IsWindow(control)) {
            DestroyWindow(control);
        }
    }
    for (const auto& item : layoutItems_) {
        if (item.control && keep.contains(item.control)) {
            keptLayout.push_back(item);
        }
    }
    for (const auto& [control, text] : statusHelpTexts_) {
        if (keep.contains(control)) {
            keptHelp[control] = text;
        }
    }

    releaseSoundSeparationRowIcons();
    releaseAutoRecordRowIcons();
    pageControls_ = std::move(keptControls);
    layoutItems_ = std::move(keptLayout);
    statusHelpTexts_ = std::move(keptHelp);
    mutedLabels_.clear();
    hoveredHelpControl_ = nullptr;
    hoverStickyStatusHeight_ = 0;
    clearButtonHover();

    const HWND saveSettingsButton = saveSettingsButton_;
    auto categoryButtons = std::move(settingsCategoryButtons_);
    resetPageControlMembers();
    saveSettingsButton_ = saveSettingsButton;
    settingsCategoryButtons_ = std::move(categoryButtons);
    soundSeparationAvailableApps_.clear();
    soundSeparationRowsY_ = 0;
    autoRecordAvailableApps_.clear();
}

HWND MainWindow::addControl(const wchar_t* className, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id) {
    DWORD finalStyle = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style;
    if (lstrcmpW(className, L"BUTTON") == 0 && (style & BS_AUTOCHECKBOX) == 0) {
        finalStyle &= ~BS_PUSHBUTTON;
        finalStyle |= BS_OWNERDRAW;
    }
    const bool comboBox = lstrcmpW(className, WC_COMBOBOXW) == 0;
    const bool staticText = lstrcmpW(className, L"STATIC") == 0;
    const int layoutHeight = comboBox ? kComboClosedHeight : height;
    const int windowHeight = comboBox ? height : layoutHeight;

    RECT design{
        x - kPageRect.left,
        y - kPageRect.top,
        x - kPageRect.left + width,
        y - kPageRect.top + layoutHeight,
    };

    HWND control = CreateWindowExW(
        0,
        className,
        text,
        finalStyle,
        design.left,
        design.top,
        width,
        windowHeight,
        pageHost_ ? pageHost_ : window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        nullptr,
        nullptr);
    setDefaultFont(control);
    if (!staticText) {
        applyDarkTheme(control);
    }
    if (comboBox) {
        SendMessageW(control, CB_SETMINVISIBLE, kComboVisibleItems, 0);
        SetWindowSubclass(control, comboBoxSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
    }
    pageControls_.push_back(control);
    layoutItems_.push_back(LayoutItem{control, design, windowHeight, LayoutItem::Kind::Content});
    SetWindowPos(control, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return control;
}

HWND MainWindow::addCheckbox(const wchar_t* text, int x, int y, int width, int height, int id, bool checked) {
    HWND control = addControl(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, width, height, id);
    SendMessageW(control, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    return control;
}

HWND MainWindow::addRowLabel(const wchar_t* text, int x, int y, int width) {
    // SS_NOTIFY lets hovering the label show the same help as its control.
    return addControl(L"STATIC", text, SS_LEFT | SS_NOTIFY, x, y, width, 22, -1);
}

void MainWindow::addRowHelp(HWND label, HWND control, const std::wstring& text) {
    addStatusHelp(control, text);
    if (label) {
        addStatusHelp(label, text);
    }
}

void MainWindow::setDefaultFont(HWND control) {
    if (control && font_) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
}

HWND MainWindow::addSectionLabel(const wchar_t* text, int x, int y, int width) {
    HWND label = addControl(L"STATIC", text, SS_CENTER, x, y, width, 22, -1);
    if (label && headingFont_) {
        SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(headingFont_), TRUE);
    }
    if (label && !layoutItems_.empty() && layoutItems_.back().control == label) {
        layoutItems_.back().sectionHeading = true;
    }
    return label;
}

void MainWindow::buildSettingsPage() {
    buildSettingsCategoryTabs(78);
    buildSettingsCategoryBody();
    addDirtySaveButton();
}

void MainWindow::buildSettingsCategoryBody() {
    if (settingsCategory_ == SettingsCategory::App) {
        buildSettingsAppPage();
    } else if (settingsCategory_ == SettingsCategory::Capture) {
        buildSettingsCapturePage();
    } else if (settingsCategory_ == SettingsCategory::Video) {
        buildSettingsVideoPage();
    } else if (settingsCategory_ == SettingsCategory::Audio) {
        buildSettingsAudioPage();
    } else {
        buildSettingsGameIntegrationsPage();
    }
}

void MainWindow::rebuildSettingsCategoryBody() {
    if (!pageHost_) {
        return;
    }
    SendMessageW(pageHost_, WM_SETREDRAW, FALSE, 0);
    buildingPage_ = true;
    // The focused control dies with the body; restore focus to its rebuilt
    // counterpart so keyboard users do not lose their place.
    const int focusedId = GetDlgCtrlID(GetFocus());
    clearSettingsBodyControls();
    pageScrollY_ = 0;
    pageWheelRemainder_ = 0;
    buildSettingsCategoryBody();
    buildingPage_ = false;
    updateSettingsCategoryChrome();
    updateSaveSettingsButton();
    layoutCurrentPage();
    storeCurrentPageCache();
    if (focusedId > 0) {
        if (HWND again = GetDlgItem(pageHost_, focusedId)) {
            SetFocus(again);
        }
    }
    SendMessageW(pageHost_, WM_SETREDRAW, TRUE, 0);
    redrawWindowAndChildren(pageHost_, true);
}

void MainWindow::buildSettingsCategoryTabs(int y) {
    constexpr int kTabX = 44;
    constexpr int kTabWidth = 150;
    constexpr int kTabHeight = 30;
    constexpr int kTabGap = 8;
    const wchar_t* labels[] = {L"App", L"Capture", L"Video", L"Audio", L"Integrations"};

    settingsCategoryButtons_.clear();
    for (int index = 0; index < kSettingsCategoryCount; ++index) {
        HWND button = addControl(
            L"BUTTON",
            labels[index],
            BS_PUSHBUTTON | WS_TABSTOP,
            kTabX + index * (kTabWidth + kTabGap),
            y,
            kTabWidth,
            kTabHeight,
            kSettingsCategoryButtonBaseId + index);
        settingsCategoryButtons_.push_back(button);
        EnableWindow(button, TRUE);
    }
    updateSettingsCategoryChrome();
}

void MainWindow::buildSettingsAppPage() {
    constexpr int kX = 44;
    constexpr int kLabelWidth = 160;
    constexpr int kControlX = 224;
    constexpr int kControlWidth = 360;
    constexpr int kRowHeight = 38;
    int y = 128;

    auto addSection = [&](const wchar_t* text) {
        addSectionLabel(text, kX, y, 260);
        y += 36;
    };

    addSection(L"App");
    HWND startWithWindowsLabel = addRowLabel(L"Start with Windows", kX, y + 4, kLabelWidth);
    startWithWindowsCheck_ = addCheckbox(L"Start minimized with Windows", kControlX, y, kControlWidth, 24, kStartWithWindowsCheckId, settings_.startWithWindowsMinimized);
    addRowHelp(startWithWindowsLabel, startWithWindowsCheck_, L"Adds Backtrack to the current user startup list and starts it minimized after Windows sign-in.");
    y += kRowHeight;

    HWND micPrivacyLabel = addRowLabel(L"Microphone privacy", kX, y + 4, kLabelWidth);
    pruneStaleMicrophoneConsentEntriesCheck_ = addCheckbox(
        L"Clean old Backtrack entries on startup", kControlX, y, kControlWidth, 24,
        kPruneStaleMicrophoneConsentEntriesCheckId, settings_.pruneStaleMicrophoneConsentEntries);
    addRowHelp(
        micPrivacyLabel,
        pruneStaleMicrophoneConsentEntriesCheck_,
        L"On primary Backtrack startup, removes Windows microphone privacy entries for other backtrack.exe paths. The running executable is never removed.");
    y += kRowHeight;

    HWND exitToTrayLabel = addRowLabel(L"Exit to tray", kX, y + 4, kLabelWidth);
    exitToTrayCheck_ = addCheckbox(L"Exit to tray", kControlX, y, kControlWidth, 24, kExitToTrayCheckId, settings_.exitToTray);
    addRowHelp(exitToTrayLabel, exitToTrayCheck_, L"Closing the window hides Backtrack in the tray instead of shutting down recording and replay services.");
    y += kRowHeight;

    HWND updatesLabel = addRowLabel(L"Updates", kX, y + 4, kLabelWidth);
    checkForUpdatesCheck_ = addCheckbox(L"Check for updates on startup", kControlX, y, kControlWidth, 24, kCheckForUpdatesCheckId, settings_.checkForUpdates);
    addRowHelp(updatesLabel, checkForUpdatesCheck_, L"Once per launch, asks GitHub for the newest Backtrack release and offers it from the tray. Turn off to stop Backtrack making that request at all.");
    y += kRowHeight;
}

void MainWindow::buildSettingsVideoPage() {
    constexpr int kX = 44;
    constexpr int kLabelWidth = 160;
    constexpr int kControlX = 224;
    constexpr int kControlWidth = 360;
    constexpr int kNumericWidth = 96;
    constexpr int kRowHeight = 38;
    constexpr int kSectionGap = 18;
    int y = 128;

    auto addSection = [&](const wchar_t* text) { addSectionLabel(text, kX, y, 260); y += 36; };
    auto finishSection = [&]() { y += kSectionGap; };

    addSection(L"Quality");
    HWND resolutionLabel = addRowLabel(L"Resolution", kX, y + 4, kLabelWidth);
    resolutionModeCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kControlWidth, 220, kResolutionModeComboId);
    fillEnumCombo(resolutionModeCombo_, kResolutionPresetComboEntries, static_cast<int>(settings_.video.resolutionMode));
    // Append one entry per monitor after the presets. Selecting one sets encode
    // width/height to that monitor's native size (resolution shortcut only; it
    // does not change which monitor is captured).
    resolutionMonitorEntries_ = enumerateMonitors();
    for (const MonitorEnumEntry& monitor : resolutionMonitorEntries_) {
        std::wstring label = std::to_wstring(monitor.width) + L"x" + std::to_wstring(monitor.height) +
            L" " + monitor.deviceName;
        if (monitor.primary) {
            label = L"MAIN " + label;
        }
        SendMessageW(resolutionModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    addRowHelp(resolutionLabel, resolutionModeCombo_, L"Presets scale to fixed 16:9 encode sizes. Monitor entries match that display's native size. Custom enables Width and Height.");
    y += kRowHeight;

    if (settings_.video.resolutionMode == ResolutionMode::Custom) {
        HWND customSizeLabel = addRowLabel(L"Custom size", kX, y + 4, kLabelWidth);
        addControl(L"STATIC", L"Width", 0, kControlX, y + 4, 48, 22, -1);
        widthEdit_ = addControl(L"EDIT", std::to_wstring(settings_.video.width).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX + 56, y, kNumericWidth, 24, kWidthEditId);
        addControl(L"STATIC", L"Height", 0, kControlX + 190, y + 4, 56, 22, -1);
        heightEdit_ = addControl(L"EDIT", std::to_wstring(settings_.video.height).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX + 254, y, kNumericWidth, 24, kHeightEditId);
        addRowHelp(
            customSizeLabel,
            widthEdit_,
            L"Encode size when Custom is selected (Height uses the same help). Larger frames need more VRAM and more GPU time for scaling and encoding each frame. Recommended: your monitor's native size, or 1080p on weaker GPUs.");
        if (heightEdit_) {
            addStatusHelp(heightEdit_, L"Encode height when Custom is selected. Larger frames need more VRAM and more GPU scaling/encode time per frame. Keep in sync with Width for the aspect ratio you want.");
        }
        y += kRowHeight;
    }

    HWND fpsLabel = addRowLabel(L"Framerate", kX, y + 4, kLabelWidth);
    fpsEdit_ = addControl(L"EDIT", std::to_wstring(settings_.video.fps).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, kNumericWidth, 24, kFpsEditId);
    addRowHelp(
        fpsLabel,
        fpsEdit_,
        L"Frames encoded per second. Encoder work and file size scale roughly linearly with this; 60 is a good default. If Diagnostics shows dropped frames or encode times above one frame interval, lower this before touching quality settings.");
    y += kRowHeight;
    HWND bitrateLabel = addRowLabel(L"Bitrate", kX, y + 4, kLabelWidth);
    bitrateEdit_ = addControl(L"EDIT", std::to_wstring(settings_.video.bitrateKbps).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, kNumericWidth, 24, kBitrateEditId);
    addRowHelp(
        bitrateLabel,
        bitrateEdit_,
        L"Target video bitrate in kilobits per second. Higher = better quality and bigger files; direct GPU cost is small because rate control only changes quantization. Rough guide: 1080p60 ~24000, 1440p60 ~40000, 4K60 ~80000.");
    y += kRowHeight;
    finishSection();

    addSection(L"GPU");
    HWND adaptiveGpuLabel = addRowLabel(L"Adaptive GPU", kX, y + 4, kLabelWidth);
    gpuAdaptiveCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kControlWidth, 100, kGpuAdaptiveComboId);
    fillEnumCombo(gpuAdaptiveCombo_, kGpuAdaptiveComboEntries, static_cast<int>(settings_.gpu.adaptiveMode));
    addRowHelp(
        adaptiveGpuLabel,
        gpuAdaptiveCombo_,
        L"Auto-protects the GPU when the encoder falls behind the capture rate. Conservative caps the frame queue at 4 and skips duplicate frames first. Aggressive caps it at 2 and additionally drops every other fresh frame when near-full. Disabled uses Frame queue as-is and never skips early. Recommended: Conservative; Aggressive for very weak GPUs or 4K capture.");
    y += kRowHeight;
    HWND frameQueueLabel = addRowLabel(L"Frame queue", kX, y + 4, kLabelWidth);
    gpuFrameQueueLimitEdit_ = addControl(L"EDIT", std::to_wstring(settings_.gpu.frameQueueLimit).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, kNumericWidth, 24, kGpuFrameQueueLimitEditId);
    addRowHelp(
        frameQueueLabel,
        gpuFrameQueueLimitEdit_,
        L"Maximum number of captured frames buffered between the capture thread and the hardware encoder. Each slot holds one full-resolution frame texture (about 8 MB VRAM at 1080p), so higher values smooth over short encode stalls at the cost of memory. Valid range 1-8, default 4 works on almost all GPUs. Raise toward 6-8 only if Diagnostics shows drops from GPU protection with a queue that is constantly full; lower to 1-2 on very low-VRAM GPUs. Adaptive GPU can cap this value further at runtime.");
    y += kRowHeight;
    HWND idleFramesLabel = addRowLabel(L"Idle frames", kX, y + 4, kLabelWidth);
    idleFrameCoalescingCheck_ = addCheckbox(L"Lossless idle coalescing", kControlX, y, kControlWidth, 24, kIdleFrameCoalescingCheckId, settings_.gpu.allowIdleFrameSkipping);
    addRowHelp(
        idleFramesLabel,
        idleFrameCoalescingCheck_,
        L"When the screen has not changed, reuses the last encoded frame instead of scaling and encoding a duplicate every tick. Output stays lossless (timestamps are extended); static content saves large amounts of GPU and encoder time. Needs Zero reorder delay enabled; leave On unless troubleshooting.");
    y += kRowHeight;
    finishSection();

    addSection(L"Format");
    HWND codecLabel = addRowLabel(L"Codec", kX, y + 4, kLabelWidth);
    codecCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kControlWidth, 120, kCodecComboId);
    fillEnumCombo(codecCombo_, kVideoCodecComboEntries, static_cast<int>(settings_.video.codec));
    addRowHelp(codecLabel, codecCombo_, L"Video codec for hardware encoder output. H.264 has broad compatibility; HEVC can improve quality per bit but may be less compatible. Both run on the same GPU encode block, so GPU cost is similar.");
    y += kRowHeight;

    HWND encoderProfileLabel = addRowLabel(L"Encoder profile", kX, y + 4, kLabelWidth);
    encoderProfileCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kControlWidth, 100, kEncoderProfileComboId);
    fillEnumCombo(encoderProfileCombo_, kEncoderProfileComboEntries, static_cast<int>(settings_.video.encoderProfile));
    addRowHelp(
        encoderProfileLabel,
        encoderProfileCombo_,
        L"Bundled encoder configurations. Lowest GPU: preset P1, ultra-low-latency tuning, 1 reference frame - the lightest possible hardware encode. Balanced: P3, low-latency tuning, 2 reference frames - good quality with modest GPU use. Custom: exposes every knob in the Encoder section below. Recommended: Balanced for quality; Lowest GPU on laptops or while gaming on the same GPU.");
    y += kRowHeight;

    if (settings_.video.encoderProfile != EncoderProfile::Custom) {
        return;
    }

    finishSection();
    addSection(L"Encoder");
    HWND presetLabel = addRowLabel(L"Preset", kX, y + 4, kLabelWidth);
    encoderPresetCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kControlWidth, 180, kEncoderPresetComboId);
    fillEnumCombo(encoderPresetCombo_, kEncoderPresetComboEntries, static_cast<int>(settings_.video.encoderPreset));
    addRowHelp(
        presetLabel,
        encoderPresetCombo_,
        L"NVIDIA quality/speed ladder. Higher numbers compress better per bit but spend more GPU time encoding each frame. P4 balances both; P1-P3 for high frame rates or when a game runs on the same GPU; P5-P7 only with clear GPU headroom.");
    y += kRowHeight;
    HWND tuningLabel = addRowLabel(L"Tuning mode", kX, y + 4, kLabelWidth);
    encoderModeCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kControlWidth, 160, kEncoderModeComboId);
    fillEnumCombo(encoderModeCombo_, kEncoderModeComboEntries, static_cast<int>(settings_.video.encoderMode));
    addRowHelp(
        tuningLabel,
        encoderModeCombo_,
        L"Rate-control tuning preset. High Quality favors visual fidelity; Low/Ultra Low Latency trade compression efficiency for minimal buffering delay; Lossless needs very high bitrates; Ultra High Quality maximizes fidelity at heavy GPU cost. Recommended: High Quality for recordings, Ultra Low Latency only when latency matters more than file size.");
    y += kRowHeight;
    HWND multipassLabel = addRowLabel(L"Multipass", kX, y + 4, kLabelWidth);
    encoderMultipassCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kControlWidth, 110, kEncoderMultipassComboId);
    fillEnumCombo(encoderMultipassCombo_, kEncoderMultipassComboEntries, static_cast<int>(settings_.video.encoderMultipass));
    addRowHelp(
        multipassLabel,
        encoderMultipassCombo_,
        L"Extra analysis pass before final rate decisions: Quarter resolution is cheap, Full resolution most accurate bitrate targeting. Both roughly double encode time versus Disabled. Recommended: Disabled - recording does not need two-pass precision.");
    y += kRowHeight;
    HWND lookaheadLabel = addRowLabel(L"Lookahead", kX, y + 4, kLabelWidth);
    encoderLookaheadCheck_ = addCheckbox(L"Enabled", kControlX, y, 120, 24, kEncoderLookaheadCheckId, settings_.video.encoderLookahead);
    addControl(L"STATIC", L"Depth", 0, kControlX + 168, y + 4, 56, 22, -1);
    encoderLookaheadDepthEdit_ = addControl(L"EDIT", std::to_wstring(settings_.video.encoderLookaheadDepth).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX + 232, y, kNumericWidth, 24, kEncoderLookaheadDepthEditId);
    addRowHelp(
        lookaheadLabel,
        encoderLookaheadCheck_,
        L"Lets the encoder inspect up to Depth future frames to place keyframes and B-frames smarter. Costs extra VRAM, latency and GPU encode time; the adaptive I/B-frame options require it. Recommended: Off for instant replay workflows; if enabled, depth 8-20 works well (max 31).");
    if (encoderLookaheadDepthEdit_) {
        addStatusHelp(encoderLookaheadDepthEdit_, L"How many future frames Lookahead may buffer (0-31). Higher = smarter frame decisions but more VRAM, latency and GPU work. Only used when Lookahead is enabled.");
    }
    y += kRowHeight;
    HWND adaptiveFramesLabel = addRowLabel(L"Adaptive frames", kX, y + 4, kLabelWidth);
    encoderAdaptiveIFramesCheck_ = addCheckbox(L"I-frames", kControlX, y, 150, 24, kEncoderAdaptiveIFramesCheckId, settings_.video.encoderAdaptiveIFrames);
    encoderAdaptiveBFramesCheck_ = addCheckbox(L"B-frames", kControlX + 188, y, 150, 24, kEncoderAdaptiveBFramesCheckId, settings_.video.encoderAdaptiveBFrames);
    addRowHelp(
        adaptiveFramesLabel,
        encoderAdaptiveIFramesCheck_,
        L"With Lookahead active, inserts I-frames (full refreshes) or B-frames only at scene changes instead of fixed intervals - better quality per bit on gameplay cuts for a small extra GPU cost. Requires Lookahead; ignored otherwise.");
    if (encoderAdaptiveBFramesCheck_) {
        addStatusHelp(encoderAdaptiveBFramesCheck_, L"With Lookahead active, places B-frames adaptively at scene changes for better compression per bit. Small extra GPU cost; requires Lookahead.");
    }
    y += kRowHeight;
    HWND spatialAqLabel = addRowLabel(L"Spatial AQ", kX, y + 4, kLabelWidth);
    encoderSpatialAQCheck_ = addCheckbox(L"Enabled", kControlX, y, 120, 24, kEncoderSpatialAQCheckId, settings_.video.encoderSpatialAQ);
    addControl(L"STATIC", L"Strength", 0, kControlX + 168, y + 4, 70, 22, -1);
    encoderAQStrengthEdit_ = addControl(L"EDIT", std::to_wstring(settings_.video.encoderAQStrength).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX + 244, y, kNumericWidth, 24, kEncoderAQStrengthEditId);
    addRowHelp(
        spatialAqLabel,
        encoderSpatialAQCheck_,
        L"Spatial adaptive quantization: shifts bits from flat areas to detailed areas within each frame, keeping dark/flat game scenes clean at the same bitrate. Strength is 1-15 (default 8). Minor GPU cost, usually worth enabling.");
    if (encoderAQStrengthEdit_) {
        addStatusHelp(encoderAQStrengthEdit_, L"Spatial AQ strength, 1-15. Default 8 balances detail boost against banding risk in gradients. Only used when Spatial AQ is enabled.");
    }
    y += kRowHeight;
    HWND temporalAqLabel = addRowLabel(L"Temporal AQ", kX, y + 4, kLabelWidth);
    encoderTemporalAQCheck_ = addCheckbox(L"Enabled", kControlX, y, kControlWidth, 24, kEncoderTemporalAQCheckId, settings_.video.encoderTemporalAQ);
    addRowHelp(
        temporalAqLabel,
        encoderTemporalAQCheck_,
        L"Like Spatial AQ but tracks motion across frames and protects newly revealed detail from being smoothed away. Minor GPU cost. Works best together with Spatial AQ on games with camera movement.");
    y += kRowHeight;
    HWND bFramesLabel = addRowLabel(L"B-frames", kX, y + 4, kLabelWidth);
    encoderBFramesCheck_ = addCheckbox(L"Enabled", kControlX, y, kControlWidth, 24, kEncoderBFramesCheckId, settings_.video.encoderBFrames);
    addRowHelp(
        bFramesLabel,
        encoderBFramesCheck_,
        L"Bidirectionally predicted frames improve compression noticeably at equal quality but add encode latency, VRAM use and GPU work, and some older players decode them poorly. Recommended: Off for lowest-latency clipping, On when file size matters more than latency.");
    y += kRowHeight;
    HWND gopLabel = addRowLabel(L"Keyframe sec", kX, y + 4, kLabelWidth);
    encoderGopSecondsEdit_ = addControl(L"EDIT", std::to_wstring(settings_.video.gopSeconds).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, kNumericWidth, 24, kEncoderGopSecondsEditId);
    addRowHelp(
        gopLabel,
        encoderGopSecondsEdit_,
        L"Seconds between forced keyframes (GOP length), 1-30. Shorter intervals make clip starts/seeks cleaner and replay saves faster but grow files and lower quality per bit slightly. Default 2 s suits replay-based clipping.");
    y += kRowHeight;
    HWND refFramesLabel = addRowLabel(L"Reference frames", kX, y + 4, kLabelWidth);
    encoderReferenceFramesEdit_ = addControl(L"EDIT", std::to_wstring(settings_.video.encoderReferenceFrames).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, kNumericWidth, 24, kEncoderReferenceFramesEditId);
    addRowHelp(
        refFramesLabel,
        encoderReferenceFramesEdit_,
        L"How many previous frames the encoder may reference for prediction (1-4). More references improve quality per bit but increase VRAM and encode time. The Lowest GPU profile uses 1, Balanced uses 2; go to 3-4 only with GPU headroom.");
    y += kRowHeight;
    HWND reorderLabel = addRowLabel(L"Reorder delay", kX, y + 4, kLabelWidth);
    encoderZeroReorderDelayCheck_ = addCheckbox(L"Zero reorder delay", kControlX, y, kControlWidth, 24, kEncoderZeroReorderDelayCheckId, settings_.video.encoderZeroReorderDelay);
    addRowHelp(
        reorderLabel,
        encoderZeroReorderDelayCheck_,
        L"Streams captured frames straight into the encoder without a reorder buffer: lowest capture-to-file latency, and Lossless idle coalescing requires it. Unchecking allows classic B-frame reordering (slightly better compression, added latency). Keep On for instant replays.");
}

void MainWindow::buildSettingsCapturePage() {
    constexpr int kNumericWidth = 96;
    constexpr int kX = 44;
    constexpr int kLabelWidth = 160;
    constexpr int kControlX = 224;
    constexpr int kControlWidth = 360;
    constexpr int kWideControlWidth = 430;
    constexpr int kRowHeight = 38;
    constexpr int kSectionGap = 18;
    int y = 128;

    auto addSection = [&](const wchar_t* text) {
        addSectionLabel(text, kX, y, 260);
        y += 36;
    };
    auto finishSection = [&]() {
        y += kSectionGap;
    };

    addSection(L"Capture");
    HWND clipFolderLabel = addRowLabel(L"Clip folder", kX, y + 4, kLabelWidth);
    clipFolderEdit_ = addControl(L"EDIT", settings_.clipDirectory.wstring().c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, kControlX, y, kWideControlWidth, 24, kClipFolderEditId);
    addControl(L"BUTTON", L"Browse", BS_PUSHBUTTON | WS_TABSTOP, kControlX + kWideControlWidth + 12, y - 2, 88, 30, kBrowseClipFolderButtonId);
    addRowHelp(clipFolderLabel, clipFolderEdit_, L"Folder where finished clips are written. Pick a fast drive; slow disks can stall saving large replays while the file is finalized.");
    y += kRowHeight;
    HWND gameFoldersLabel = addRowLabel(L"Game folders", kX, y + 4, kLabelWidth);
    organizeClipsByGameCheck_ = addCheckbox(L"Save each game in its own folder", kControlX, y, kWideControlWidth, 24, kOrganizeClipsByGameCheckId, settings_.organizeClipsByGame);
    addRowHelp(gameFoldersLabel, organizeClipsByGameCheck_, L"When on, each replay is saved inside a folder named after the game it was clipped from; the folder is created when missing. When off, all clips land directly in the clip folder above.");
    y += kRowHeight;
    HWND captureMethodLabel = addRowLabel(L"Capture method", kX, y + 4, kLabelWidth);
    captureMethodCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kControlWidth, 100, kCaptureMethodComboId);
    fillEnumCombo(captureMethodCombo_, kCaptureBackendComboEntries, static_cast<int>(settings_.preferredCaptureBackend));
    addRowHelp(
        captureMethodLabel,
        captureMethodCombo_,
        L"Screen capture backend. WGC (Windows Graphics Capture) is the modern API and supports zero-copy frames into the encoder. DXGI (Desktop Duplication) is the legacy path, sometimes lighter on old systems but must copy every frame. Recommended: WGC; Backtrack falls back automatically if the selected backend fails to start.");
    y += kRowHeight;
    HWND cursorLabel = addRowLabel(L"Capture cursor", kX, y + 4, kLabelWidth);
    captureCursorCheck_ = addCheckbox(L"Capture cursor", kControlX, y, kControlWidth, 24, kCaptureCursorCheckId, settings_.captureCursor);
    addRowHelp(cursorLabel, captureCursorCheck_, L"Draws the mouse cursor into recorded frames. Negligible GPU impact either way; disable for clean gameplay footage.");
    y += kRowHeight;
    HWND diskBackedLabel = addRowLabel(L"Buffer on disk", kX, y + 4, kLabelWidth);
    replayDiskBackedCheck_ = addCheckbox(L"Video", kControlX, y, 140, 24, kReplayDiskBackedCheckId, settings_.replay.diskBackedVideo);
    replayDiskBackedAudioCheck_ = addCheckbox(L"Sound", kControlX + 150, y, 140, 24, kReplayDiskBackedAudioCheckId, settings_.replay.diskBackedAudio);
    // The checkboxes reflect the settings, which stay on across a fallback, so say
    // so here rather than letting a ticked box imply the track is on disk.
    std::wstring diskBackedHelp =
        L"Stores encoded replay video and/or sound in fixed-size ring files instead of system memory. "
        L"Cuts replay buffer memory usage to almost zero. Changing a checkbox clears that track's buffered replay. "
        L"Changing the buffer length below does not. "
        L"Pick the folder below to keep the continuous writes off a game drive, e.g. on an SSD. "
        L"If the folder cannot be used, that track falls back to memory and the Diagnostics page says so.";
    if (const std::wstring fallback = trimText(controller_.replayStorageFallback()); !fallback.empty()) {
        diskBackedHelp += L"  Currently: " + fallback;
    }
    addRowHelp(diskBackedLabel, replayDiskBackedCheck_, diskBackedHelp);
    addStatusHelp(replayDiskBackedAudioCheck_, diskBackedHelp);
    y += kRowHeight;
    if (settings_.replay.diskBackedVideo || settings_.replay.diskBackedAudio) {
        HWND ringDirLabel = addRowLabel(L"Buffer folder", kX, y + 4, kLabelWidth);
        replayRingDirEdit_ = addControl(L"EDIT", settings_.replayRingDirectory.wstring().c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, kControlX, y, kWideControlWidth - 100, 24, kReplayRingDirEditId);
        addControl(L"BUTTON", L"Browse", BS_PUSHBUTTON | WS_TABSTOP, kControlX + kWideControlWidth - 88, y - 2, 88, 30, kBrowseRingDirButtonId);
        addRowHelp(ringDirLabel, replayRingDirEdit_,
            L"Where the ring files live while either option above is enabled. Leave empty to follow the clip library folder "
            L"(currently " + settings_.clipDirectory.wstring() + L"). "
            L"A fast SSD is recommended when gaming on an HDD; avoid cloud-synced folders. Changing it clears the current replay buffer.");
        y += kRowHeight;
    }
    HWND overlapLabel = addRowLabel(L"Clip overlap (seconds)", kX, y + 4, kLabelWidth);
    replayOverlapSecondsEdit_ = addControl(L"EDIT", std::to_wstring(settings_.replay.overlapSeconds).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, kNumericWidth, 24, kReplayOverlapSecondsEditId);
    addRowHelp(overlapLabel, replayOverlapSecondsEdit_, L"When saving again within the replay window, save only new footage plus this much of the previous clip.");
    y += kRowHeight;
    finishSection();
    addSection(L"Multi-monitor");
    HWND multiMonitorLabel = addRowLabel(L"Multi-monitor support", kX, y + 4, kLabelWidth);
    multiMonitorSupportCheck_ = addCheckbox(L"Enable multi-monitor support", kControlX, y, kControlWidth, 24, kMultiMonitorSupportCheckId, settings_.multiMonitorSupport);
    addRowHelp(multiMonitorLabel, multiMonitorSupportCheck_, L"Allows capturing any connected monitor instead of a fixed one and unlocks the monitor choice below. Adds a brief rebind hitch when the active monitor changes.");
    y += kRowHeight;
    if (settings_.multiMonitorSupport) {
        HWND monitorFollowLabel = addRowLabel(L"Record which monitor", kX, y + 4, kLabelWidth);
        monitorFollowCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kControlWidth, 100, kMonitorFollowComboId);
        fillEnumCombo(monitorFollowCombo_, kMonitorFollowComboEntries, settings_.followMouseMonitor ? 1 : 0);
        addRowHelp(monitorFollowLabel, monitorFollowCombo_, L"Focused monitor records whichever display owns the active window; focus changes move the recording after a short stability delay. Follow mouse records the display under the cursor instead.");
        y += kRowHeight;
        HWND stableFramesLabel = addRowLabel(L"Stable multi-monitor frames", kX, y + 4, kLabelWidth);
        stableMultimonitorFramesCheck_ = addCheckbox(L"Stable multi-monitor frames", kControlX, y, kControlWidth, 24, kStableMultimonitorFramesCheckId, settings_.gpu.stableMultimonitorFrames);
        addRowHelp(stableFramesLabel, stableMultimonitorFramesCheck_, L"Forces an owned copy of every frame so monitor switches never tear or flash stale colors. Costs one extra GPU copy per frame; disable only if you want absolute lowest overhead and accept switch glitches. Only applies with WGC capture.");
        y += kRowHeight;
    }
    finishSection();
}

void MainWindow::buildSettingsAudioPage() {
    constexpr int kX = 44;
    constexpr int kLabelWidth = 160;
    constexpr int kControlX = 224;
    constexpr int kComboWidth = 360;
    constexpr int kRowHeight = 42;
    constexpr int kSectionGap = 18;
    int y = 128;

    auto addSection = [&](const wchar_t* text) {
        addSectionLabel(text, kX, y, 260);
        y += 36;
    };
    auto finishSection = [&]() {
        y += kSectionGap;
    };

    addSection(L"App");
    HWND notificationVolumeLabel = addRowLabel(L"Notification volume", kX, y + 4, kLabelWidth);
    notificationSoundVolumeEdit_ = addControl(L"EDIT", std::to_wstring(settings_.notificationSoundVolumePercent).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, 96, 24, kNotificationSoundVolumeEditId);
    addRowHelp(notificationVolumeLabel, notificationSoundVolumeEdit_, L"Volume (0-100%) of the sound played when a replay finishes saving. Affects only the notification, never recordings.");
    y += kRowHeight;
    HWND customSoundLabel = addRowLabel(L"Custom notification sound", kX, y + 4, kLabelWidth);
    constexpr int kSoundPathWidth = kComboWidth - 188;
    customNotificationSoundEdit_ = addControl(
        L"EDIT",
        settings_.customNotificationSoundPath.wstring().c_str(),
        WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
        kControlX,
        y,
        kSoundPathWidth,
        24,
        kCustomNotificationSoundEditId);
    addControl(
        L"BUTTON",
        L"Browse",
        BS_PUSHBUTTON | WS_TABSTOP,
        kControlX + kSoundPathWidth + 8,
        y - 2,
        80,
        30,
        kBrowseCustomNotificationSoundButtonId);
    addControl(
        L"BUTTON",
        L"Reset",
        BS_PUSHBUTTON | WS_TABSTOP,
        kControlX + kSoundPathWidth + 96,
        y - 2,
        72,
        30,
        kResetCustomNotificationSoundButtonId);
    addRowHelp(customSoundLabel, customNotificationSoundEdit_, L"WAV file played instead of the built-in tone when a replay saves. Empty = built-in tone.");
    y += kRowHeight;

    finishSection();
    addSection(L"Devices");
    HWND systemAudioLabel = addRowLabel(L"Output audio", kX, y + 4, kLabelWidth);
    systemAudioCheck_ = addCheckbox(L"Capture output audio", kControlX, y, kComboWidth, 24, kSystemAudioCheckId, settings_.captureSystemAudio);
    addRowHelp(systemAudioLabel, systemAudioCheck_, L"Records desktop/game audio as a track in clips. Disabling saves a small amount of CPU/disk but records silence for game sound.");
    y += kRowHeight;
    HWND outputDeviceLabel = addRowLabel(L"Output device", kX, y + 4, kLabelWidth);
    outputDeviceCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kComboWidth, 180, kOutputDeviceComboId);
    y += kRowHeight;
    addRowHelp(outputDeviceLabel, outputDeviceCombo_, L"Which playback device is recorded as desktop audio. Leave on Default unless game audio plays through another device.");
    HWND outputGainLabel = addRowLabel(L"Desktop gain (%)", kX, y + 4, kLabelWidth);
    outputVolumeEdit_ = addControl(L"EDIT", std::to_wstring(settings_.audioOutputVolumePercent).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, 96, 24, kOutputVolumeEditId);
    addRowHelp(outputGainLabel, outputVolumeEdit_, L"Desktop audio gain for recordings and replays. 100% is unchanged; 0% mutes it; up to 200% may clip audio.");
    y += kRowHeight;
    HWND micLabel = addRowLabel(L"Input audio", kX, y + 4, kLabelWidth);
    microphoneCheck_ = addCheckbox(L"Capture input audio", kControlX, y, kComboWidth, 24, kMicrophoneCheckId, settings_.captureMicrophone);
    addRowHelp(micLabel, microphoneCheck_, L"Records your microphone as a separate track in clips. Windows shows the privacy icon while active.");
    y += kRowHeight;
    HWND inputDeviceLabel = addRowLabel(L"Input device", kX, y + 4, kLabelWidth);
    inputDeviceCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kComboWidth, 180, kInputDeviceComboId);
    y += kRowHeight;
    addRowHelp(inputDeviceLabel, inputDeviceCombo_, L"Which microphone is recorded. Leave on Default unless you use a dedicated mic that is not the system default.");
    HWND inputGainLabel = addRowLabel(L"Microphone gain (%)", kX, y + 4, kLabelWidth);
    inputVolumeEdit_ = addControl(L"EDIT", std::to_wstring(settings_.audioInputVolumePercent).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, 96, 24, kInputVolumeEditId);
    addRowHelp(inputGainLabel, inputVolumeEdit_, L"Manual microphone gain for recordings and replays. 100% is unchanged; 0% mutes it; up to 200% may clip audio. Raise this to make your microphone louder than desktop audio.");
    y += kRowHeight;
    HWND autoGainLabel = addRowLabel(L"Auto mic gain", kX, y + 4, kLabelWidth);
    inputAutoGainCheck_ = addCheckbox(L"Normalize microphone volume (AGC)", kControlX, y, kComboWidth, 24, kInputAutoGainCheckId, settings_.audioInputAutoGainEnabled);
    addRowHelp(autoGainLabel, inputAutoGainCheck_, L"Automatically boosts quiet microphones to a consistent level. Applies after the manual gain above; recommended when the raw microphone level is low.");
    y += kRowHeight;
#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
    HWND noiseSuppressionLabel = addRowLabel(L"Mic noise suppression", kX, y + 4, kLabelWidth);
    inputNoiseSuppressionCheck_ = addCheckbox(L"Suppress microphone noise (RNNoise)", kControlX, y, kComboWidth, 24, kInputNoiseSuppressionCheckId, settings_.audioInputNoiseSuppressionEnabled);
    addRowHelp(noiseSuppressionLabel, inputNoiseSuppressionCheck_, L"Removes steady background noise such as fans and hiss from the microphone track with an RNNoise neural network. Applies before the gain options above; system audio is never processed. Adds a small CPU cost while recording.");
    y += kRowHeight;
#endif
    loadAudioDeviceCombos();

    finishSection();
    addSection(L"Sound separation");
    HWND separationModeLabel = addRowLabel(L"Mode", kX, y + 4, kLabelWidth);
    soundSeparationEnabledCheck_ = addCheckbox(
        L"Enable sound separation", kControlX, y, kComboWidth, 24,
        kSoundSeparationEnabledCheckId, settings_.soundSeparationEnabled);
    addRowHelp(separationModeLabel, soundSeparationEnabledCheck_, L"Turns app audio exclusion on or off. When off, Backtrack records normal system audio.");
    y += kRowHeight;

    HWND activeAppLabel = addRowLabel(L"Active app", kX, y + 4, kLabelWidth);
    soundSeparationAppCombo_ = addControl(
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST | WS_TABSTOP,
        kControlX,
        y,
        kComboWidth,
        180,
        kSoundSeparationAppComboId);
    addRowHelp(activeAppLabel, soundSeparationAppCombo_, L"Choose a running app to add it to sound separation. Apps only affect clips while sound separation is enabled and the app row is muted.");
    soundSeparationRefreshButton_ = addControl(
        L"BUTTON",
        L"Refresh",
        BS_PUSHBUTTON | WS_TABSTOP,
        kControlX + kComboWidth + 12,
        y - 2,
        88,
        30,
        kSoundSeparationRefreshButtonId);
    addStatusHelp(soundSeparationRefreshButton_, L"Scans audio sessions and visible running apps, then updates the app list.");
    soundSeparationManualButton_ = addControl(
        L"BUTTON",
        L"Manual",
        BS_PUSHBUTTON | WS_TABSTOP,
        kControlX + kComboWidth + 108,
        y - 2,
        88,
        30,
        kSoundSeparationManualButtonId);
    addStatusHelp(soundSeparationManualButton_, L"Select an executable manually when the app is not currently producing audio.");
    y += kRowHeight;

    addRowLabel(L"Selected apps", kX, y + 4, kLabelWidth);
    y += 36;
    soundSeparationRowsY_ = y;
    refreshSoundSeparationApps();
    rebuildSoundSeparationRows();
}

void MainWindow::buildSettingsGameIntegrationsPage() {
    constexpr int kX = 44;
    constexpr int kLabelWidth = 160;
    constexpr int kControlX = 224;
    constexpr int kControlWidth = 430;
    constexpr int kRowHeight = 42;
    constexpr int kSectionGap = 22;
    int y = 128;

    auto addSection = [&](const wchar_t* text) {
        addSectionLabel(text, kX, y, 260);
        y += 38;
    };
    auto finishSection = [&]() {
        y += kSectionGap;
    };

    addSection(L"League of Legends");
    HWND killReminderLabel = addRowLabel(L"Kill reminder", kX, y + 4, kLabelWidth);
    leagueKillReminderCheck_ = addCheckbox(
        L"Beep on champion kills", kControlX, y, kControlWidth, 24,
        kLeagueKillReminderCheckId, settings_.gameIntegrations.leagueOfLegendsKillReminder);
    addRowHelp(killReminderLabel, leagueKillReminderCheck_, L"Polls League's local live-client API during active games. When your champion scores a kill, Backtrack beeps; if you save replay within 6 seconds, the clip is tagged as Kill clip.");
    y += kRowHeight;
    finishSection();

    addSection(L"Voice command");
    HWND triggerPhraseLabel = addRowLabel(L"Trigger phrase", kX, y + 4, kLabelWidth);
    voiceCommandCombo_ = addControl(
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST | WS_TABSTOP,
        kControlX,
        y,
        kControlWidth,
        120,
        kVoiceCommandComboId);
    fillEnumCombo(voiceCommandCombo_, kVoiceCommandComboEntries, static_cast<int>(settings_.gameIntegrations.voiceCommand));
    addRowHelp(
        triggerPhraseLabel,
        voiceCommandCombo_,
        L"Uses offline Windows speech recognition for one exact phrase (Backtrack clip that or Record video). Either phrase saves the current replay buffer; Record video does not start continuous recording. Audio is not uploaded or stored by Backtrack.");
    y += kRowHeight;
    finishSection();

    addSection(L"Discord");
    HWND richPresenceLabel = addRowLabel(L"Rich presence", kX, y + 4, kLabelWidth);
    discordRichPresenceCombo_ = addControl(
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST | WS_TABSTOP,
        kControlX,
        y,
        kControlWidth,
        120,
        kDiscordRichPresenceComboId);
    fillEnumCombo(
        discordRichPresenceCombo_, kDiscordPresenceComboEntries, static_cast<int>(settings_.gameIntegrations.discordRichPresence));
    addRowHelp(
        richPresenceLabel,
        discordRichPresenceCombo_,
        L"Shows \"Clipping <game>\" on Discord with a button leading to the github repo.");
    y += kRowHeight;
    if (settings_.gameIntegrations.discordRichPresence !=
        GameIntegrationSettings::DiscordRichPresenceMode::Off) {
        HWND presenceTimerLabel = addRowLabel(L"Elapsed timer", kX, y + 4, kLabelWidth);
        discordPresenceTimerCombo_ = addControl(
            WC_COMBOBOXW,
            L"",
            CBS_DROPDOWNLIST | WS_TABSTOP,
            kControlX,
            y,
            kControlWidth,
            120,
            kDiscordPresenceTimerComboId);
        SendMessageW(discordPresenceTimerCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Reset timer on app switch"));
        SendMessageW(discordPresenceTimerCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Keep timer running"));
        SendMessageW(
            discordPresenceTimerCombo_,
            CB_SETCURSEL,
            settings_.gameIntegrations.discordRichPresenceResetTimerOnSwitch ? 0 : 1,
            0);
        addRowHelp(
            presenceTimerLabel,
            discordPresenceTimerCombo_,
            L"Whether the Discord elapsed timer restarts when the foreground app changes. Reset starts it over on every switch; Keep lets one timer run until Rich presence is turned off.");
        y += kRowHeight;
    }
    finishSection();

    addSection(L"Auto record");
    constexpr int kComboWidth = 360;
    HWND autoRecordModeLabel = addRowLabel(L"Enable", kX, y + 4, kLabelWidth);
    autoRecordEnabledCheck_ = addCheckbox(
        L"Enable auto record", kControlX, y, kComboWidth, 24,
        kAutoRecordEnabledCheckId, settings_.autoRecordEnabled);
    addRowHelp(autoRecordModeLabel, autoRecordEnabledCheck_,
        L"Starts continuous recording automatically when one of the selected apps below is running and stops it when the last one exits. "
        L"You can still start and stop manually; auto record never restarts a recording you stopped yourself while the app stays open.");
    y += kRowHeight;

    HWND activeAppLabel = addRowLabel(L"Active app", kX, y + 4, kLabelWidth);
    autoRecordAppCombo_ = addControl(
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST | WS_TABSTOP,
        kControlX,
        y,
        kComboWidth,
        180,
        kAutoRecordAppComboId);
    addRowHelp(activeAppLabel, autoRecordAppCombo_, L"Choose a running app to add it to auto record. Matching is by executable path.");
    autoRecordRefreshButton_ = addControl(
        L"BUTTON",
        L"Refresh",
        BS_PUSHBUTTON | WS_TABSTOP,
        kControlX + kComboWidth + 12,
        y - 2,
        88,
        30,
        kAutoRecordRefreshButtonId);
    addStatusHelp(autoRecordRefreshButton_, L"Scans visible running apps, then updates the app list.");
    autoRecordManualButton_ = addControl(
        L"BUTTON",
        L"Manual",
        BS_PUSHBUTTON | WS_TABSTOP,
        kControlX + kComboWidth + 108,
        y - 2,
        88,
        30,
        kAutoRecordManualButtonId);
    addStatusHelp(autoRecordManualButton_, L"Select an executable manually when the app is not currently running.");
    y += kRowHeight;

    addRowLabel(L"Selected apps", kX, y + 4, kLabelWidth);
    y += 36;
    autoRecordRowsY_ = y;
    refreshAutoRecordApps();
    rebuildAutoRecordRows();
}

void MainWindow::buildCapturePage() {
    constexpr int kX = 44;
    constexpr int kLabelWidth = 160;
    constexpr int kControlX = 224;
    constexpr int kControlWidth = 430;
    constexpr int kNumericWidth = 120;
    constexpr int kRowHeight = 40;
    constexpr int kSectionGap = 22;
    int y = 78;

    auto addSection = [&](const wchar_t* text) {
        addSectionLabel(text, kX, y, 260);
        y += 38;
    };
    auto finishSection = [&]() {
        y += kSectionGap;
    };

    addSection(L"Recording");
    HWND hotkeyLabel = addRowLabel(L"Hotkey", kX, y + 4, kLabelWidth);
    recordHotkey_ = addControl(L"EDIT", hotkeyDisplay(recordHotkeyModifiers_, recordHotkeyVirtualKey_).c_str(), WS_BORDER | ES_READONLY | ES_CENTER | WS_TABSTOP, kControlX, y, kControlWidth, 24, kRecordHotkeyId);
    addRowHelp(hotkeyLabel, recordHotkey_, L"Keyboard shortcut for starting or stopping a normal recording. Focus this field and press the desired key combination.");
    y += kRowHeight;

    addRowLabel(L"Action", kX, y + 4, kLabelWidth);
    startStopButton_ = addControl(L"BUTTON", controller_.isRecording() ? L"Stop Recording" : L"Start Recording", BS_PUSHBUTTON | WS_TABSTOP, kControlX, y - 4, 170, 34, kStartStopButtonId);
    addControl(L"BUTTON", L"Recover Failed", BS_PUSHBUTTON | WS_TABSTOP, kControlX + 188, y - 4, 170, 34, kRecoverFailedRecordingButtonId);
    y += kRowHeight;
    finishSection();

    addSection(L"Replay");
    HWND bufferLabel = addRowLabel(L"Buffer", kX, y + 4, kLabelWidth);
    replayEnabledCheck_ = addCheckbox(L"Enable instant replay buffer", kControlX, y, kControlWidth, 24, kReplayEnabledId, settings_.replay.enabled);
    addRowHelp(bufferLabel, replayEnabledCheck_, L"Keeps a rolling encoded video/audio buffer so a recent clip can be saved without re-encoding.");
    y += kRowHeight;

    HWND lengthLabel = addRowLabel(L"Length seconds", kX, y + 4, kLabelWidth);
    replaySecondsEdit_ = addControl(L"EDIT", std::to_wstring(settings_.replay.seconds).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, kNumericWidth, 24, kReplaySecondsEditId);
    addRowHelp(lengthLabel, replaySecondsEdit_, L"How many seconds of encoded packets are retained for instant replay. Longer buffers use more system memory and disk work on save, not more VRAM.");
    y += kRowHeight;

    HWND saveHotkeyLabel = addRowLabel(L"Save hotkey", kX, y + 4, kLabelWidth);
    replayHotkey_ = addControl(L"EDIT", hotkeyDisplay(replayHotkeyModifiers_, replayHotkeyVirtualKey_).c_str(), WS_BORDER | ES_READONLY | ES_CENTER | WS_TABSTOP, kControlX, y, kControlWidth, 24, kReplayHotkeyId);
    addRowHelp(saveHotkeyLabel, replayHotkey_, L"Keyboard shortcut for saving the current replay buffer. Focus this field and press the desired key combination.");
    y += kRowHeight;

    addRowLabel(L"Action", kX, y + 4, kLabelWidth);
    addControl(L"BUTTON", L"Save Replay", BS_PUSHBUTTON | WS_TABSTOP, kControlX, y - 4, 150, 34, kSaveReplayButtonId);
    addDirtySaveButton();
}

void MainWindow::buildStatsPage() {
    // Read-only multiline edits: they wrap to the column width and scroll
    // internally; the subclass keeps Tab/Shift+Tab moving focus.
    auto makeReadOnlyEdit = [&](const wchar_t* text, int x, int y, int width, int height, int id) {
        HWND edit = addControl(L"EDIT", text, WS_BORDER | ES_MULTILINE | ES_READONLY | ES_LEFT | WS_VSCROLL | WS_TABSTOP, x, y, width, height, id);
        SetWindowSubclass(edit, readOnlyEditSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
        return edit;
    };
    addSectionLabel(L"Runtime", 44, 78, 220);
    statsLabel_ = makeReadOnlyEdit(L"", 44, 112, 350, 330, kStatsLabelId);
    addSectionLabel(L"Encoder", 464, 78, 220);
    capsLabel_ = makeReadOnlyEdit(L"", 464, 112, 350, 330, kCapsLabelId);
    addSectionLabel(L"Recent Log", 44, 484, 220);
    loggingEnabledCheck_ = addCheckbox(L"Enable logging", 44, 484, 160, 26, kLoggingEnabledCheckId, settings_.loggingEnabled);
    addControl(L"BUTTON", L"Open Folder", BS_PUSHBUTTON | WS_TABSTOP, 560, 480, 132, 30, kOpenLogFolderButtonId);
    addControl(L"BUTTON", L"Save Log", BS_PUSHBUTTON | WS_TABSTOP, 704, 480, 120, 30, kSaveLogButtonId);
    diagnosticLogLabel_ = makeReadOnlyEdit(L"", 44, 518, 780, 150, kDiagnosticLogLabelId);
    updateStats();
}

void MainWindow::buildClipsPage() {
    addSectionLabel(L"Library", 44, 78, 220);
    addControl(L"BUTTON", L"Delete", BS_PUSHBUTTON | WS_TABSTOP, 274, 76, 78, 28, kDeleteClipButtonId);
    addControl(L"BUTTON", L"Favorite", BS_PUSHBUTTON | WS_TABSTOP, 360, 76, 90, 28, kFavoriteClipButtonId);
    addControl(L"BUTTON", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, 458, 76, 86, 28, kRefreshClipsButtonId);
    listViewButton_ = addControl(L"BUTTON", L"List", BS_PUSHBUTTON | WS_TABSTOP, 628, 76, 76, 28, kClipListViewButtonId);
    galleryViewButton_ = addControl(L"BUTTON", L"Gallery", BS_PUSHBUTTON | WS_TABSTOP, 712, 76, 76, 28, kClipGalleryViewButtonId);
    clipList_ = addControl(L"LISTBOX", L"", WS_BORDER | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, 44, 112, 780, 370, kClipListId);
    renameEdit_ = addControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 44, 508, 340, 24, kRenameEditId);
    renameEditButton_ = addControl(L"BUTTON", L"Rename", BS_PUSHBUTTON | WS_TABSTOP, 392, 506, 86, 28, kRenameClipButtonId);
    setListItemHeightIfChanged(clipList_, clipListItemHeight_, libraryViewMode_ == LibraryViewMode::Gallery ? kGalleryClipMinimumItemHeight : kListClipItemHeight);
    if (clipList_) {
        SetWindowSubclass(clipList_, clipListSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
    }

    refreshClips();
}

} // namespace backtrack

