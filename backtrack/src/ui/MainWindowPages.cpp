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
void MainWindow::buildTabs() {
    const wchar_t* labels[] = {L"Capture", L"Library", L"Settings", L"Diagnostics"};
    for (int index = 0; index < kTabCount; ++index) {
        HWND button = CreateWindowExW(
            0,
            L"BUTTON",
            labels[index],
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

void MainWindow::rebindPageControlPointers() {
    saveSettingsButton_ = nullptr;
    startStopButton_ = nullptr;
    bitrateEdit_ = nullptr;
    codecCombo_ = nullptr;
    recordHotkey_ = nullptr;
    clipFolderEdit_ = nullptr;
    replayEnabledCheck_ = nullptr;
    replaySecondsEdit_ = nullptr;
    replayHotkey_ = nullptr;
    leagueKillReminderCheck_ = nullptr;
    statsLabel_ = nullptr;
    capsLabel_ = nullptr;
    diagnosticLogLabel_ = nullptr;
    clipList_ = nullptr;
    listViewButton_ = nullptr;
    galleryViewButton_ = nullptr;
    settingsCategoryButtons_.clear();
    renameEdit_ = nullptr;
    fpsEdit_ = nullptr;
    resolutionModeCombo_ = nullptr;
    widthEdit_ = nullptr;
    heightEdit_ = nullptr;
    followFocusedMonitorCheck_ = nullptr;
    followMouseMonitorCheck_ = nullptr;
    captureCursorCheck_ = nullptr;
    systemAudioCheck_ = nullptr;
    microphoneCheck_ = nullptr;
    outputDeviceCombo_ = nullptr;
    inputDeviceCombo_ = nullptr;
    outputVolumeEdit_ = nullptr;
    inputVolumeEdit_ = nullptr;
    startWithWindowsCheck_ = nullptr;
    pruneStaleMicrophoneConsentEntriesCheck_ = nullptr;
    exitToTrayCheck_ = nullptr;
    notificationSoundVolumeEdit_ = nullptr;
    encoderPresetCombo_ = nullptr;
    encoderModeCombo_ = nullptr;
    encoderProfileCombo_ = nullptr;
    encoderLookaheadCheck_ = nullptr;
    encoderLookaheadDepthEdit_ = nullptr;
    encoderSpatialAQCheck_ = nullptr;
    encoderAQStrengthEdit_ = nullptr;
    encoderTemporalAQCheck_ = nullptr;
    encoderMultipassCombo_ = nullptr;
    encoderBFramesCheck_ = nullptr;
    encoderAdaptiveBFramesCheck_ = nullptr;
    encoderAdaptiveIFramesCheck_ = nullptr;
    encoderZeroReorderDelayCheck_ = nullptr;
    encoderGopSecondsEdit_ = nullptr;
    encoderReferenceFramesEdit_ = nullptr;
    gpuAdaptiveCombo_ = nullptr;
    gpuFrameQueueLimitEdit_ = nullptr;
    idleFrameCoalescingCheck_ = nullptr;
    captureMethodCombo_ = nullptr;
    stableMultimonitorFramesCheck_ = nullptr;
    soundSeparationEnabledCheck_ = nullptr;
    soundSeparationAppCombo_ = nullptr;
    soundSeparationRefreshButton_ = nullptr;
    soundSeparationManualButton_ = nullptr;
    soundSeparationRowsY_ = 0;

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
    replayHotkey_ = child(kReplayHotkeyId);
    saveSettingsButton_ = child(kSaveSettingsButtonId);
    bitrateEdit_ = child(kBitrateEditId);
    codecCombo_ = child(kCodecComboId);
    clipFolderEdit_ = child(kClipFolderEditId);
    fpsEdit_ = child(kFpsEditId);
    resolutionModeCombo_ = child(kResolutionModeComboId);
    widthEdit_ = child(kWidthEditId);
    heightEdit_ = child(kHeightEditId);
    followFocusedMonitorCheck_ = child(kFollowFocusedMonitorCheckId);
    followMouseMonitorCheck_ = child(kFollowMouseMonitorCheckId);
    captureCursorCheck_ = child(kCaptureCursorCheckId);
    systemAudioCheck_ = child(kSystemAudioCheckId);
    microphoneCheck_ = child(kMicrophoneCheckId);
    outputDeviceCombo_ = child(kOutputDeviceComboId);
    inputDeviceCombo_ = child(kInputDeviceComboId);
    outputVolumeEdit_ = child(kOutputVolumeEditId);
    inputVolumeEdit_ = child(kInputVolumeEditId);
    startWithWindowsCheck_ = child(kStartWithWindowsCheckId);
    pruneStaleMicrophoneConsentEntriesCheck_ = child(kPruneStaleMicrophoneConsentEntriesCheckId);
    exitToTrayCheck_ = child(kExitToTrayCheckId);
    notificationSoundVolumeEdit_ = child(kNotificationSoundVolumeEditId);
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
    statsLabel_ = child(kStatsLabelId);
    capsLabel_ = child(kCapsLabelId);
    diagnosticLogLabel_ = child(kDiagnosticLogLabelId);
    clipList_ = child(kClipListId);
    listViewButton_ = child(kClipListViewButtonId);
    galleryViewButton_ = child(kClipGalleryViewButtonId);
    renameEdit_ = child(kRenameEditId);

    for (int index = 0; index < kSettingsCategoryCount; ++index) {
        HWND button = child(kSettingsCategoryButtonBaseId + index);
        if (button) {
            settingsCategoryButtons_.push_back(button);
        }
    }

    if (page_ == Page::Settings && settingsCategory_ == SettingsCategory::SoundSeparation) {
        // Rows sit under the "Selected apps" section; fall back if layout scan fails.
        soundSeparationRowsY_ = 250;
        for (const auto& item : layoutItems_) {
            if (item.control == soundSeparationAppCombo_) {
                soundSeparationRowsY_ = static_cast<int>(item.design.bottom) + kPageRect.top + 54;
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
        cache.controls.clear();
        cache.layoutItems.clear();
        cache.statusHelpTexts.clear();
        cache.scrollY = 0;
        cache.contentHeight = 0;
        cache.wheelRemainder = 0;
        cache.built = false;
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
    cache.controls.clear();
    cache.layoutItems.clear();
    cache.statusHelpTexts.clear();
    cache.scrollY = 0;
    cache.contentHeight = 0;
    cache.wheelRemainder = 0;
    cache.built = false;
}

void MainWindow::updateTabChrome() {
    for (size_t index = 0; index < tabButtons_.size(); ++index) {
        HWND button = tabButtons_[index];
        if (!button) {
            continue;
        }
        const bool selected = index == static_cast<size_t>(page_);
        const bool dirtySettings = settingsDirty_ && index == static_cast<size_t>(Page::Settings);
        std::wstring label;
        switch (static_cast<Page>(index)) {
        case Page::Capture:
            label = L"Capture";
            break;
        case Page::Library:
            label = L"Library";
            break;
        case Page::Settings:
            label = dirtySettings ? L"Settings*" : L"Settings";
            break;
        case Page::Diagnostics:
            label = L"Diagnostics";
            break;
        }
        setText(button, label);
        EnableWindow(button, TRUE);
        InvalidateRect(button, nullptr, FALSE);
        (void)selected;
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
        // Do not keep processing Shell thumbnails after leaving the library. Results already
        // in thumbnailCache_ remain available when the page is rebuilt.
        clearClipThumbnails();
    }

    // Diagnostics has no asynchronous UI work. Library thumbnail callbacks can arrive after
    // a tab switch, so rebuild its HWND tree rather than keeping hidden controls alive.
    const bool previousCacheable = previousPage == Page::Diagnostics;
    if (previousCacheable && pageCaches_[static_cast<size_t>(previousPage)].built) {
        storeCurrentPageCache();
    } else if (previousHost) {
        SendMessageW(previousHost, WM_SETREDRAW, FALSE, 0);
        clearPageControls();
        auto& previousCache = pageCaches_[static_cast<size_t>(previousPage)];
        previousCache.controls.clear();
        previousCache.layoutItems.clear();
        previousCache.statusHelpTexts.clear();
        previousCache.scrollY = 0;
        previousCache.contentHeight = 0;
        previousCache.wheelRemainder = 0;
        previousCache.built = false;
        SendMessageW(previousHost, WM_SETREDRAW, TRUE, 0);
    }
    if (previousHost) {
        ShowWindow(previousHost, SW_HIDE);
    }

    page_ = page;
    clearButtonHover();
    hoveredHelpControl_ = nullptr;
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
    releaseSoundSeparationRowIcons();
    soundSeparationRows_.clear();
    pageControls_.clear();
    layoutItems_.clear();
    statusHelpTexts_.clear();
    hoveredHelpControl_ = nullptr;
    clearButtonHover();
    saveSettingsButton_ = nullptr;
    startStopButton_ = nullptr;
    bitrateEdit_ = nullptr;
    codecCombo_ = nullptr;
    recordHotkey_ = nullptr;
    clipFolderEdit_ = nullptr;
    replayEnabledCheck_ = nullptr;
    replaySecondsEdit_ = nullptr;
    replayHotkey_ = nullptr;
    leagueKillReminderCheck_ = nullptr;
    statsLabel_ = nullptr;
    capsLabel_ = nullptr;
    diagnosticLogLabel_ = nullptr;
    clipList_ = nullptr;
    listViewButton_ = nullptr;
    galleryViewButton_ = nullptr;
    settingsCategoryButtons_.clear();
    renameEdit_ = nullptr;
    selectedClipIndex_ = static_cast<size_t>(-1);
    fpsEdit_ = nullptr;
    resolutionModeCombo_ = nullptr;
    widthEdit_ = nullptr;
    heightEdit_ = nullptr;
    followFocusedMonitorCheck_ = nullptr;
    followMouseMonitorCheck_ = nullptr;
    captureCursorCheck_ = nullptr;
    systemAudioCheck_ = nullptr;
    microphoneCheck_ = nullptr;
    outputDeviceCombo_ = nullptr;
    inputDeviceCombo_ = nullptr;
    outputVolumeEdit_ = nullptr;
    inputVolumeEdit_ = nullptr;
    startWithWindowsCheck_ = nullptr;
    pruneStaleMicrophoneConsentEntriesCheck_ = nullptr;
    exitToTrayCheck_ = nullptr;
    notificationSoundVolumeEdit_ = nullptr;
    encoderPresetCombo_ = nullptr;
    encoderModeCombo_ = nullptr;
    encoderProfileCombo_ = nullptr;
    encoderLookaheadCheck_ = nullptr;
    encoderLookaheadDepthEdit_ = nullptr;
    encoderSpatialAQCheck_ = nullptr;
    encoderAQStrengthEdit_ = nullptr;
    encoderTemporalAQCheck_ = nullptr;
    encoderMultipassCombo_ = nullptr;
    encoderBFramesCheck_ = nullptr;
    encoderAdaptiveBFramesCheck_ = nullptr;
    encoderAdaptiveIFramesCheck_ = nullptr;
    encoderZeroReorderDelayCheck_ = nullptr;
    encoderGopSecondsEdit_ = nullptr;
    encoderReferenceFramesEdit_ = nullptr;
    gpuAdaptiveCombo_ = nullptr;
    gpuFrameQueueLimitEdit_ = nullptr;
    idleFrameCoalescingCheck_ = nullptr;
    captureMethodCombo_ = nullptr;
    stableMultimonitorFramesCheck_ = nullptr;
    soundSeparationEnabledCheck_ = nullptr;
    soundSeparationAppCombo_ = nullptr;
    soundSeparationRefreshButton_ = nullptr;
    soundSeparationManualButton_ = nullptr;
    soundSeparationAvailableApps_.clear();
    soundSeparationRowsY_ = 0;
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
    soundSeparationRows_.clear();
    pageControls_ = std::move(keptControls);
    layoutItems_ = std::move(keptLayout);
    statusHelpTexts_ = std::move(keptHelp);
    hoveredHelpControl_ = nullptr;
    clearButtonHover();

    bitrateEdit_ = nullptr;
    codecCombo_ = nullptr;
    clipFolderEdit_ = nullptr;
    leagueKillReminderCheck_ = nullptr;
    fpsEdit_ = nullptr;
    resolutionModeCombo_ = nullptr;
    widthEdit_ = nullptr;
    heightEdit_ = nullptr;
    followFocusedMonitorCheck_ = nullptr;
    followMouseMonitorCheck_ = nullptr;
    captureCursorCheck_ = nullptr;
    systemAudioCheck_ = nullptr;
    microphoneCheck_ = nullptr;
    outputDeviceCombo_ = nullptr;
    inputDeviceCombo_ = nullptr;
    outputVolumeEdit_ = nullptr;
    inputVolumeEdit_ = nullptr;
    startWithWindowsCheck_ = nullptr;
    pruneStaleMicrophoneConsentEntriesCheck_ = nullptr;
    exitToTrayCheck_ = nullptr;
    notificationSoundVolumeEdit_ = nullptr;
    encoderPresetCombo_ = nullptr;
    encoderModeCombo_ = nullptr;
    encoderProfileCombo_ = nullptr;
    encoderLookaheadCheck_ = nullptr;
    encoderLookaheadDepthEdit_ = nullptr;
    encoderSpatialAQCheck_ = nullptr;
    encoderAQStrengthEdit_ = nullptr;
    encoderTemporalAQCheck_ = nullptr;
    encoderMultipassCombo_ = nullptr;
    encoderBFramesCheck_ = nullptr;
    encoderAdaptiveBFramesCheck_ = nullptr;
    encoderAdaptiveIFramesCheck_ = nullptr;
    encoderZeroReorderDelayCheck_ = nullptr;
    encoderGopSecondsEdit_ = nullptr;
    encoderReferenceFramesEdit_ = nullptr;
    gpuAdaptiveCombo_ = nullptr;
    gpuFrameQueueLimitEdit_ = nullptr;
    idleFrameCoalescingCheck_ = nullptr;
    captureMethodCombo_ = nullptr;
    stableMultimonitorFramesCheck_ = nullptr;
    soundSeparationEnabledCheck_ = nullptr;
    soundSeparationAppCombo_ = nullptr;
    soundSeparationRefreshButton_ = nullptr;
    soundSeparationManualButton_ = nullptr;
    soundSeparationAvailableApps_.clear();
    soundSeparationRowsY_ = 0;
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

void MainWindow::setDefaultFont(HWND control) {
    if (control && font_) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
}

HWND MainWindow::addSectionLabel(const wchar_t* text, int x, int y, int width) {
    HWND label = addControl(L"STATIC", text, SS_LEFT, x, y, width, 22, -1);
    if (label && headingFont_) {
        SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(headingFont_), TRUE);
    }
    return label;
}

void MainWindow::buildSettingsPage() {
    if (!settingsDirty_) {
        settings_ = controller_.settings();
    }
    buildSettingsCategoryTabs(78);
    buildSettingsCategoryBody();
    addDirtySaveButton();
}

void MainWindow::buildSettingsCategoryBody() {
    if (settingsCategory_ == SettingsCategory::General) {
        buildSettingsGeneralPage();
    } else if (settingsCategory_ == SettingsCategory::Advanced) {
        buildSettingsAdvancedPage();
    } else if (settingsCategory_ == SettingsCategory::SoundSeparation) {
        buildSettingsSoundSeparationPage();
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
    clearSettingsBodyControls();
    pageScrollY_ = 0;
    pageWheelRemainder_ = 0;
    buildSettingsCategoryBody();
    buildingPage_ = false;
    updateSettingsCategoryChrome();
    updateSaveSettingsButton();
    layoutCurrentPage();
    storeCurrentPageCache();
    SendMessageW(pageHost_, WM_SETREDRAW, TRUE, 0);
    redrawWindowAndChildren(pageHost_, true);
}

void MainWindow::buildSettingsCategoryTabs(int y) {
    constexpr int kTabX = 44;
    constexpr int kTabWidth = 150;
    constexpr int kTabHeight = 30;
    constexpr int kTabGap = 8;
    const wchar_t* labels[] = {L"General", L"Advanced", L"Sound separation", L"Game integrations"};

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

void MainWindow::buildSettingsGeneralPage() {
    constexpr int kX = 44;
    constexpr int kLabelWidth = 160;
    constexpr int kControlX = 224;
    constexpr int kControlWidth = 360;
    constexpr int kWideControlWidth = 430;
    constexpr int kNumericWidth = 96;
    constexpr int kRowHeight = 38;
    constexpr int kSectionGap = 18;
    int y = 128;

    auto addRowLabel = [&](const wchar_t* text) {
        addControl(L"STATIC", text, 0, kX, y + 4, kLabelWidth, 22, -1);
    };
    auto addSection = [&](const wchar_t* text) {
        addSectionLabel(text, kX, y, 260);
        y += 36;
    };
    auto finishSection = [&]() {
        y += kSectionGap;
    };

    addSection(L"App");
    addRowLabel(L"Startup");
    startWithWindowsCheck_ = addControl(L"BUTTON", L"Start minimized with Windows", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kStartWithWindowsCheckId);
    SendMessageW(startWithWindowsCheck_, BM_SETCHECK, settings_.startWithWindowsMinimized ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(startWithWindowsCheck_, 0, 0, L"Adds Backtrack to the current user startup list and starts it minimized after Windows sign-in.");
    y += kRowHeight;

    addRowLabel(L"Microphone privacy");
    pruneStaleMicrophoneConsentEntriesCheck_ = addControl(
        L"BUTTON",
        L"Clean old Backtrack entries on startup",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        kControlX,
        y,
        kControlWidth,
        24,
        kPruneStaleMicrophoneConsentEntriesCheckId);
    SendMessageW(
        pruneStaleMicrophoneConsentEntriesCheck_,
        BM_SETCHECK,
        settings_.pruneStaleMicrophoneConsentEntries ? BST_CHECKED : BST_UNCHECKED,
        0);
    addSettingHelp(
        pruneStaleMicrophoneConsentEntriesCheck_,
        0,
        0,
        L"On primary Backtrack startup, removes Windows microphone privacy entries for other backtrack.exe paths. The running executable is never removed.");
    y += kRowHeight;

    addRowLabel(L"Close button");
    exitToTrayCheck_ = addControl(L"BUTTON", L"Exit into tray", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kExitToTrayCheckId);
    SendMessageW(exitToTrayCheck_, BM_SETCHECK, settings_.exitToTray ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(exitToTrayCheck_, 0, 0, L"Closing the window hides Backtrack in the tray instead of shutting down recording and replay services.");
    y += kRowHeight;

    addRowLabel(L"Notification volume");
    notificationSoundVolumeEdit_ = addControl(
        L"EDIT",
        std::to_wstring(settings_.notificationSoundVolumePercent).c_str(),
        WS_BORDER | ES_NUMBER | WS_TABSTOP,
        kControlX,
        y,
        kNumericWidth,
        24,
        kNotificationSoundVolumeEditId);
    addSettingHelp(
        notificationSoundVolumeEdit_,
        0,
        0,
        L"Volume of UI notification tones (record start/stop, replay save, errors). 0 mutes; 100 is full level.");
    y += kRowHeight;
    finishSection();

    addSection(L"Output");
    addRowLabel(L"Codec");
    codecCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kControlWidth, 120, kCodecComboId);
    SendMessageW(codecCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"H.264"));
    SendMessageW(codecCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"HEVC"));
    SendMessageW(codecCombo_, CB_SETCURSEL, settings_.video.codec == VideoCodec::H264 ? 0 : 1, 0);
    addSettingHelp(codecCombo_, 0, 0, L"Video codec for hardware encoder output. H.264 has broad compatibility; HEVC can improve quality per bit but may be less compatible.");
    y += kRowHeight;

    addRowLabel(L"Bitrate (Kbps)");
    bitrateEdit_ = addControl(L"EDIT", std::to_wstring(settings_.video.bitrateKbps).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, kNumericWidth, 24, kBitrateEditId);
    addSettingHelp(bitrateEdit_, 0, 0, L"Target video bitrate in kilobits per second. Higher values preserve detail but increase file size and encoder bandwidth.");
    y += kRowHeight;

    addRowLabel(L"Frame rate");
    fpsEdit_ = addControl(L"EDIT", std::to_wstring(settings_.video.fps).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, kNumericWidth, 24, kFpsEditId);
    addSettingHelp(fpsEdit_, 0, 0, L"Target recording frame rate. Higher FPS captures smoother motion but increases capture, encode, and replay-buffer work.");
    y += kRowHeight;

    addRowLabel(L"Resolution");
    resolutionModeCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kControlWidth, 220, kResolutionModeComboId);
    SendMessageW(resolutionModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Native display"));
    SendMessageW(resolutionModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"240p"));
    SendMessageW(resolutionModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"480p"));
    SendMessageW(resolutionModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"720p"));
    SendMessageW(resolutionModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"1080p"));
    SendMessageW(resolutionModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"2K"));
    SendMessageW(resolutionModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"4K"));
    SendMessageW(resolutionModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Custom size"));
    SendMessageW(resolutionModeCombo_, CB_SETCURSEL, static_cast<WPARAM>(settings_.video.resolutionMode), 0);
    addSettingHelp(resolutionModeCombo_, 0, 0, L"Native records at the captured display size. Presets scale to fixed 16:9 encode sizes. Custom enables Width and Height and may use a D3D11 scaling pass.");
    y += kRowHeight;

    addRowLabel(L"Custom size");
    addControl(L"STATIC", L"Width", 0, kControlX, y + 4, 48, 22, -1);
    widthEdit_ = addControl(L"EDIT", std::to_wstring(settings_.video.width).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX + 56, y, kNumericWidth, 24, kWidthEditId);
    addSettingHelp(widthEdit_, 0, 0, L"Custom encode width. Lower width reduces GPU encode load and bitrate demand but softens the image.");
    addControl(L"STATIC", L"Height", 0, kControlX + 190, y + 4, 56, 22, -1);
    heightEdit_ = addControl(L"EDIT", std::to_wstring(settings_.video.height).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX + 254, y, kNumericWidth, 24, kHeightEditId);
    addSettingHelp(heightEdit_, 0, 0, L"Custom encode height. Lower height reduces GPU encode load and bitrate demand but softens the image.");
    y += kRowHeight;

    addRowLabel(L"Monitor");
    followFocusedMonitorCheck_ = addControl(L"BUTTON", L"Follow focused monitor", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kFollowFocusedMonitorCheckId);
    SendMessageW(followFocusedMonitorCheck_, BM_SETCHECK, settings_.followFocusedMonitor ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(followFocusedMonitorCheck_, 0, 0, L"Enables monitor switching during one continuous recording or replay buffer. By default Backtrack records the monitor containing the foreground window and uses Windows Graphics Capture.");
    y += kRowHeight;

    followMouseMonitorCheck_ = addControl(L"BUTTON", L"Follow mouse", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kFollowMouseMonitorCheckId);
    SendMessageW(followMouseMonitorCheck_, BM_SETCHECK, settings_.followMouseMonitor ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(followMouseMonitorCheck_, 0, 0, L"Uses the mouse cursor instead of the foreground window to choose the recorded monitor when monitor following is enabled.");
    y += kRowHeight;

    captureCursorCheck_ = addControl(L"BUTTON", L"Capture cursor", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kCaptureCursorCheckId);
    SendMessageW(captureCursorCheck_, BM_SETCHECK, settings_.captureCursor ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(captureCursorCheck_, 0, 0, L"Includes the mouse cursor in Windows Graphics Capture. Desktop Duplication always composites the system cursor separately when drawn by the desktop.");
    y += kRowHeight;

    addRowLabel(L"Clip folder");
    clipFolderEdit_ = addControl(L"EDIT", settings_.clipDirectory.wstring().c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, kControlX, y, kWideControlWidth, 24, kClipFolderEditId);
    addSettingHelp(clipFolderEdit_, 0, 0, L"Folder where recordings and saved replay clips are written.");
    addControl(L"BUTTON", L"Browse", BS_PUSHBUTTON | WS_TABSTOP, kControlX + kWideControlWidth + 12, y - 2, 88, 30, kBrowseClipFolderButtonId);
    y += kRowHeight;
    updateResolutionControls();
    finishSection();

    addSection(L"Audio");
    addRowLabel(L"Output audio");
    systemAudioCheck_ = addControl(L"BUTTON", L"Capture output audio", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kSystemAudioCheckId);
    SendMessageW(systemAudioCheck_, BM_SETCHECK, settings_.captureSystemAudio ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(systemAudioCheck_, 0, 0, L"Records desktop/game audio from the selected output device.");
    y += kRowHeight;

    addRowLabel(L"Output device");
    outputDeviceCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kWideControlWidth, 140, kOutputDeviceComboId);
    addSettingHelp(outputDeviceCombo_, 0, 0, L"Output device used for system audio capture. Default follows the current Windows default output.");
    y += kRowHeight;

    addRowLabel(L"Output volume");
    outputVolumeEdit_ = addControl(L"EDIT", std::to_wstring(settings_.audioOutputVolumePercent).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, kNumericWidth, 24, kOutputVolumeEditId);
    addSettingHelp(outputVolumeEdit_, 0, 0, L"Volume applied to captured output audio before it is written to recordings and replay clips. 100 keeps the original level.");
    y += kRowHeight;

    addRowLabel(L"Microphone");
    microphoneCheck_ = addControl(L"BUTTON", L"Capture microphone", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kMicrophoneCheckId);
    SendMessageW(microphoneCheck_, BM_SETCHECK, settings_.captureMicrophone ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(microphoneCheck_, 0, 0, L"Records microphone audio from the selected input device.");
    y += kRowHeight;

    addRowLabel(L"Input device");
    inputDeviceCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kWideControlWidth, 140, kInputDeviceComboId);
    addSettingHelp(inputDeviceCombo_, 0, 0, L"Input device used for microphone capture. Default follows the current Windows default input.");
    y += kRowHeight;

    addRowLabel(L"Input volume");
    inputVolumeEdit_ = addControl(L"EDIT", std::to_wstring(settings_.audioInputVolumePercent).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, kNumericWidth, 24, kInputVolumeEditId);
    addSettingHelp(inputVolumeEdit_, 0, 0, L"Volume applied to captured microphone audio before it is written to recordings and replay clips. 100 keeps the original level.");
    loadAudioDeviceCombos();
}

void MainWindow::buildSettingsAdvancedPage() {
    constexpr int kX = 44;
    constexpr int kLabelWidth = 160;
    constexpr int kControlX = 224;
    constexpr int kControlWidth = 360;
    constexpr int kNumericWidth = 96;
    constexpr int kRowHeight = 38;
    constexpr int kSectionGap = 18;
    int y = 128;

    auto addRowLabel = [&](const wchar_t* text) {
        addControl(L"STATIC", text, 0, kX, y + 4, kLabelWidth, 22, -1);
    };
    auto addSection = [&](const wchar_t* text) {
        addSectionLabel(text, kX, y, 260);
        y += 36;
    };
    auto finishSection = [&]() {
        y += kSectionGap;
    };

    addSection(L"Encoder");
    addRowLabel(L"Profile");
    encoderProfileCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kControlWidth, 100, kEncoderProfileComboId);
    SendMessageW(encoderProfileCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Lowest GPU"));
    SendMessageW(encoderProfileCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Balanced"));
    SendMessageW(encoderProfileCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Custom"));
    SendMessageW(encoderProfileCombo_, CB_SETCURSEL, static_cast<WPARAM>(settings_.video.encoderProfile), 0);
    addSettingHelp(encoderProfileCombo_, 0, 0, L"Lowest GPU forces the lightest encoder settings. Balanced keeps a little more quality. Custom uses the controls below. GPU impact: profile-dependent.");
    y += kRowHeight;

    addRowLabel(L"Preset");
    encoderPresetCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kControlWidth, 180, kEncoderPresetComboId);
    SendMessageW(encoderPresetCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"P1 fastest"));
    SendMessageW(encoderPresetCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"P2 faster"));
    SendMessageW(encoderPresetCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"P3 fast"));
    SendMessageW(encoderPresetCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"P4 medium"));
    SendMessageW(encoderPresetCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"P5 slow"));
    SendMessageW(encoderPresetCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"P6 slower"));
    SendMessageW(encoderPresetCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"P7 slowest"));
    SendMessageW(encoderPresetCombo_, CB_SETCURSEL, static_cast<WPARAM>(settings_.video.encoderPreset), 0);
    addSettingHelp(encoderPresetCombo_, 0, 0, L"Encoder preset speed/quality level. Higher numbers spend more encoder work for quality. On AMD (AMF) presets map to nearest Speed/Balanced/Quality. GPU impact: Low to High.");
    y += kRowHeight;

    addRowLabel(L"Tuning mode");
    encoderModeCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kControlWidth, 160, kEncoderModeComboId);
    SendMessageW(encoderModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"High quality"));
    SendMessageW(encoderModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Low latency"));
    SendMessageW(encoderModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Ultra low latency"));
    SendMessageW(encoderModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Lossless"));
    SendMessageW(encoderModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Ultra high quality"));
    SendMessageW(encoderModeCombo_, CB_SETCURSEL, static_cast<WPARAM>(settings_.video.encoderMode), 0);
    addSettingHelp(encoderModeCombo_, 0, 0, L"Encoder tuning mode. Low-latency modes avoid extra buffering. Lossless and ultra-high-quality can be expensive. GPU impact: Low to High.");
    y += kRowHeight;

    addRowLabel(L"Multipass");
    encoderMultipassCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kControlWidth, 110, kEncoderMultipassComboId);
    SendMessageW(encoderMultipassCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Disabled"));
    SendMessageW(encoderMultipassCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Quarter resolution"));
    SendMessageW(encoderMultipassCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Full resolution"));
    SendMessageW(encoderMultipassCombo_, CB_SETCURSEL, static_cast<WPARAM>(settings_.video.encoderMultipass), 0);
    addSettingHelp(encoderMultipassCombo_, 0, 0, L"Runs an extra analysis pass before encoding. Disable for lowest GPU use. GPU impact: Significant to High.");
    y += kRowHeight;

    addRowLabel(L"Lookahead");
    encoderLookaheadCheck_ = addControl(L"BUTTON", L"Enabled", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, 120, 24, kEncoderLookaheadCheckId);
    SendMessageW(encoderLookaheadCheck_, BM_SETCHECK, settings_.video.encoderLookahead ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(encoderLookaheadCheck_, 0, 0, L"Allows the encoder to inspect future frames for better rate control and B-frame choices. Disable for lowest latency and GPU use.");
    addControl(L"STATIC", L"Depth", 0, kControlX + 168, y + 4, 56, 22, -1);
    encoderLookaheadDepthEdit_ = addControl(L"EDIT", std::to_wstring(settings_.video.encoderLookaheadDepth).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX + 232, y, kNumericWidth, 24, kEncoderLookaheadDepthEditId);
    addSettingHelp(encoderLookaheadDepthEdit_, 0, 0, L"Number of future frames the encoder may analyze when lookahead is enabled. Higher depth can improve quality but increases latency and work.");
    y += kRowHeight;

    addRowLabel(L"Adaptive frames");
    encoderAdaptiveIFramesCheck_ = addControl(L"BUTTON", L"I-frames", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, 150, 24, kEncoderAdaptiveIFramesCheckId);
    SendMessageW(encoderAdaptiveIFramesCheck_, BM_SETCHECK, settings_.video.encoderAdaptiveIFrames ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(encoderAdaptiveIFramesCheck_, 0, 0, L"Lets the encoder insert I-frames when scene changes need faster recovery. Can improve quality but may add encode spikes.");
    encoderAdaptiveBFramesCheck_ = addControl(L"BUTTON", L"B-frames", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX + 188, y, 150, 24, kEncoderAdaptiveBFramesCheckId);
    SendMessageW(encoderAdaptiveBFramesCheck_, BM_SETCHECK, settings_.video.encoderAdaptiveBFrames ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(encoderAdaptiveBFramesCheck_, 0, 0, L"Lets the encoder vary B-frame placement based on content. Requires B-frames and can improve compression at extra encoder cost.");
    y += kRowHeight;

    addRowLabel(L"Spatial AQ");
    encoderSpatialAQCheck_ = addControl(L"BUTTON", L"Enabled", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, 120, 24, kEncoderSpatialAQCheckId);
    SendMessageW(encoderSpatialAQCheck_, BM_SETCHECK, settings_.video.encoderSpatialAQ ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(encoderSpatialAQCheck_, 0, 0, L"Redistributes bits within each frame to protect detailed areas. Improves perceived quality but adds encoder work.");
    addControl(L"STATIC", L"Strength", 0, kControlX + 168, y + 4, 70, 22, -1);
    encoderAQStrengthEdit_ = addControl(L"EDIT", std::to_wstring(settings_.video.encoderAQStrength).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX + 244, y, kNumericWidth, 24, kEncoderAQStrengthEditId);
    addSettingHelp(encoderAQStrengthEdit_, 0, 0, L"Spatial AQ strength from 1 to 15. Higher values favor detailed regions more aggressively and may cost more quality elsewhere.");
    y += kRowHeight;

    addRowLabel(L"Temporal AQ");
    encoderTemporalAQCheck_ = addControl(L"BUTTON", L"Enabled", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kEncoderTemporalAQCheckId);
    SendMessageW(encoderTemporalAQCheck_, BM_SETCHECK, settings_.video.encoderTemporalAQ ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(encoderTemporalAQCheck_, 0, 0, L"Adjusts quality across frames over time to preserve important motion. Can improve quality but increases encoder work.");
    y += kRowHeight;

    addRowLabel(L"B-frames");
    encoderBFramesCheck_ = addControl(L"BUTTON", L"Enabled", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kEncoderBFramesCheckId);
    SendMessageW(encoderBFramesCheck_, BM_SETCHECK, settings_.video.encoderBFrames ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(encoderBFramesCheck_, 0, 0, L"Enables bidirectionally predicted frames for better compression. Adds latency and encoder work, so keep off for lowest GPU use.");
    y += kRowHeight;

    addRowLabel(L"Keyframe sec");
    encoderGopSecondsEdit_ = addControl(L"EDIT", std::to_wstring(settings_.video.gopSeconds).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, kNumericWidth, 24, kEncoderGopSecondsEditId);
    addSettingHelp(encoderGopSecondsEdit_, 0, 0, L"Seconds between forced keyframes. Longer intervals reduce keyframe spikes but may delay replay trimming and scene recovery.");
    y += kRowHeight;

    addRowLabel(L"Reference frames");
    encoderReferenceFramesEdit_ = addControl(L"EDIT", std::to_wstring(settings_.video.encoderReferenceFrames).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, kNumericWidth, 24, kEncoderReferenceFramesEditId);
    addSettingHelp(encoderReferenceFramesEdit_, 0, 0, L"Number of reference frames used for prediction. One is lowest GPU and VRAM; higher values can improve compression.");
    y += kRowHeight;

    addRowLabel(L"Reorder delay");
    encoderZeroReorderDelayCheck_ = addControl(L"BUTTON", L"Zero reorder delay", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kEncoderZeroReorderDelayCheckId);
    SendMessageW(encoderZeroReorderDelayCheck_, BM_SETCHECK, settings_.video.encoderZeroReorderDelay ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(encoderZeroReorderDelayCheck_, 0, 0, L"Requests output packets without frame reordering delay. Useful for low latency and replay timing, but may limit B-frame behavior.");
    y += kRowHeight;
    finishSection();

    addSection(L"GPU");
    addRowLabel(L"Adaptive GPU");
    gpuAdaptiveCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kControlWidth, 100, kGpuAdaptiveComboId);
    SendMessageW(gpuAdaptiveCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Disabled"));
    SendMessageW(gpuAdaptiveCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Conservative"));
    SendMessageW(gpuAdaptiveCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Aggressive"));
    SendMessageW(gpuAdaptiveCombo_, CB_SETCURSEL, static_cast<WPARAM>(settings_.gpu.adaptiveMode), 0);
    addSettingHelp(gpuAdaptiveCombo_, 0, 0, L"Drops capture work when encoder backlog appears to protect game frame time. Aggressive reacts earlier and may drop more frames.");
    y += kRowHeight;

    addRowLabel(L"Frame queue");
    gpuFrameQueueLimitEdit_ = addControl(L"EDIT", std::to_wstring(settings_.gpu.frameQueueLimit).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, kNumericWidth, 24, kGpuFrameQueueLimitEditId);
    addSettingHelp(gpuFrameQueueLimitEdit_, 0, 0, L"Maximum queued GPU frames before the encoder. Lower values reduce buffering and VRAM but can drop frames if encoding falls behind.");
    y += kRowHeight;

    addRowLabel(L"Idle frames");
    idleFrameCoalescingCheck_ = addControl(L"BUTTON", L"Lossless idle coalescing", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kIdleFrameCoalescingCheckId);
    SendMessageW(idleFrameCoalescingCheck_, BM_SETCHECK, settings_.gpu.allowIdleFrameSkipping ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(idleFrameCoalescingCheck_, 0, 0, L"Preserves the full 60 FPS timeline by extending unchanged samples instead of re-encoding the same captured texture. Periodic IDR heartbeats keep recordings and replays seekable.");
    y += kRowHeight;

    addRowLabel(L"Capture method");
    captureMethodCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kControlWidth, 100, kCaptureMethodComboId);
    SendMessageW(captureMethodCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"WGC"));
    SendMessageW(captureMethodCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"DXGI"));
    SendMessageW(
        captureMethodCombo_,
        CB_SETCURSEL,
        settings_.preferredCaptureBackend == CaptureBackend::DesktopDuplication ? 1 : 0,
        0);
    addSettingHelp(captureMethodCombo_, 0, 0, L"WGC uses Windows Graphics Capture. DXGI uses Desktop Duplication. WGC automatically falls back to DXGI when unavailable.");
    y += kRowHeight;

    addRowLabel(L"Monitor switches");
    stableMultimonitorFramesCheck_ = addControl(L"BUTTON", L"Stable multimonitor frames", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kStableMultimonitorFramesCheckId);
    SendMessageW(stableMultimonitorFramesCheck_, BM_SETCHECK, settings_.gpu.stableMultimonitorFrames ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(stableMultimonitorFramesCheck_, 0, 0, L"Waits for stable frames during monitor switches so recordings avoid transient wrong-monitor frames.");
    y += kRowHeight;

}

void MainWindow::buildSettingsSoundSeparationPage() {
    constexpr int kX = 44;
    constexpr int kLabelWidth = 160;
    constexpr int kControlX = 224;
    constexpr int kComboWidth = 360;
    constexpr int kRowHeight = 42;
    int y = 128;

    auto addRowLabel = [&](const wchar_t* text) {
        addControl(L"STATIC", text, 0, kX, y + 4, kLabelWidth, 22, -1);
    };
    auto addSection = [&](const wchar_t* text) {
        addSectionLabel(text, kX, y, 260);
        y += 36;
    };

    addSection(L"Sound separation");
    addRowLabel(L"Mode");
    soundSeparationEnabledCheck_ = addControl(
        L"BUTTON",
        L"Enable sound separation",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        kControlX,
        y,
        kComboWidth,
        24,
        kSoundSeparationEnabledCheckId);
    SendMessageW(soundSeparationEnabledCheck_, BM_SETCHECK, settings_.soundSeparationEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(soundSeparationEnabledCheck_, 0, 0, L"Turns app audio exclusion on or off. When off, Backtrack records normal system audio.");
    y += kRowHeight;

    addRowLabel(L"Active app");
    soundSeparationAppCombo_ = addControl(
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST | WS_TABSTOP,
        kControlX,
        y,
        kComboWidth,
        180,
        kSoundSeparationAppComboId);
    addSettingHelp(soundSeparationAppCombo_, 0, 0, L"Choose a running app to add it to sound separation. Apps only affect clips while sound separation is enabled and the app row is muted.");
    soundSeparationRefreshButton_ = addControl(
        L"BUTTON",
        L"Refresh",
        BS_PUSHBUTTON | WS_TABSTOP,
        kControlX + kComboWidth + 12,
        y - 2,
        88,
        30,
        kSoundSeparationRefreshButtonId);
    addSettingHelp(soundSeparationRefreshButton_, 0, 0, L"Scans audio sessions and visible running apps, then updates the app list.");
    soundSeparationManualButton_ = addControl(
        L"BUTTON",
        L"Manual",
        BS_PUSHBUTTON | WS_TABSTOP,
        kControlX + kComboWidth + 108,
        y - 2,
        88,
        30,
        kSoundSeparationManualButtonId);
    addSettingHelp(soundSeparationManualButton_, 0, 0, L"Select an executable manually when the app is not currently producing audio.");
    y += kRowHeight;

    addSection(L"Selected apps");
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
    int y = 128;

    auto addRowLabel = [&](const wchar_t* text) {
        addControl(L"STATIC", text, 0, kX, y + 4, kLabelWidth, 22, -1);
    };
    auto addSection = [&](const wchar_t* text) {
        addSectionLabel(text, kX, y, 260);
        y += 36;
    };

    addSection(L"Game integrations");
    addRowLabel(L"League of Legends");
    leagueKillReminderCheck_ = addControl(
        L"BUTTON",
        L"Beep on champion kills",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        kControlX,
        y,
        kControlWidth,
        24,
        kLeagueKillReminderCheckId);
    SendMessageW(leagueKillReminderCheck_, BM_SETCHECK, settings_.gameIntegrations.leagueOfLegendsKillReminder ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(leagueKillReminderCheck_, 0, 0, L"Polls League's local live-client API during active games. When your champion scores a kill, Backtrack beeps; if you save replay within 6 seconds, the clip is tagged as Kill clip.");
    y += kRowHeight;
}

void MainWindow::buildCapturePage() {
    if (!settingsDirty_) {
        settings_ = controller_.settings();
    }

    constexpr int kX = 44;
    constexpr int kLabelWidth = 160;
    constexpr int kControlX = 224;
    constexpr int kControlWidth = 430;
    constexpr int kNumericWidth = 120;
    constexpr int kRowHeight = 40;
    constexpr int kSectionGap = 22;
    int y = 78;

    auto addRowLabel = [&](const wchar_t* text) {
        addControl(L"STATIC", text, 0, kX, y + 4, kLabelWidth, 22, -1);
    };
    auto addSection = [&](const wchar_t* text) {
        addSectionLabel(text, kX, y, 260);
        y += 38;
    };
    auto finishSection = [&]() {
        y += kSectionGap;
    };

    addSection(L"Recording");
    addRowLabel(L"Hotkey");
    recordHotkey_ = addControl(L"EDIT", hotkeyDisplay(recordHotkeyModifiers_, recordHotkeyVirtualKey_).c_str(), WS_BORDER | ES_READONLY | ES_CENTER | WS_TABSTOP, kControlX, y, kControlWidth, 24, kRecordHotkeyId);
    addSettingHelp(recordHotkey_, 0, 0, L"Keyboard shortcut for starting or stopping a normal recording. Focus this field and press the desired key combination.");
    y += kRowHeight;

    addRowLabel(L"Action");
    startStopButton_ = addControl(L"BUTTON", controller_.isRecording() ? L"Stop Recording" : L"Start Recording", BS_PUSHBUTTON | WS_TABSTOP, kControlX, y - 4, 170, 34, kStartStopButtonId);
    addControl(L"BUTTON", L"Recover Failed", BS_PUSHBUTTON | WS_TABSTOP, kControlX + 188, y - 4, 170, 34, kRecoverFailedRecordingButtonId);
    y += kRowHeight;
    finishSection();

    addSection(L"Replay");
    addRowLabel(L"Buffer");
    replayEnabledCheck_ = addControl(L"BUTTON", L"Enable instant replay buffer", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kReplayEnabledId);
    SendMessageW(replayEnabledCheck_, BM_SETCHECK, settings_.replay.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(replayEnabledCheck_, 0, 0, L"Keeps a rolling encoded video/audio buffer so a recent clip can be saved without re-encoding.");
    y += kRowHeight;

    addRowLabel(L"Length seconds");
    replaySecondsEdit_ = addControl(L"EDIT", std::to_wstring(settings_.replay.seconds).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, kNumericWidth, 24, kReplaySecondsEditId);
    addSettingHelp(replaySecondsEdit_, 0, 0, L"How many seconds of encoded packets are retained for instant replay. Longer buffers use more system memory and disk work on save, not more VRAM.");
    y += kRowHeight;

    addRowLabel(L"Save hotkey");
    replayHotkey_ = addControl(L"EDIT", hotkeyDisplay(replayHotkeyModifiers_, replayHotkeyVirtualKey_).c_str(), WS_BORDER | ES_READONLY | ES_CENTER | WS_TABSTOP, kControlX, y, kControlWidth, 24, kReplayHotkeyId);
    addSettingHelp(replayHotkey_, 0, 0, L"Keyboard shortcut for saving the current replay buffer. Focus this field and press the desired key combination.");
    y += kRowHeight;

    addRowLabel(L"Action");
    addControl(L"BUTTON", L"Save Replay", BS_PUSHBUTTON | WS_TABSTOP, kControlX, y - 4, 150, 34, kSaveReplayButtonId);
    addDirtySaveButton();
}

void MainWindow::buildStatsPage() {
    addSectionLabel(L"Runtime", 44, 78, 220);
    statsLabel_ = addControl(L"STATIC", L"", SS_LEFT, 44, 112, 350, 330, kStatsLabelId);
    addSectionLabel(L"Encoder", 464, 78, 220);
    capsLabel_ = addControl(L"STATIC", L"", SS_LEFT, 464, 112, 350, 330, kCapsLabelId);
    addSectionLabel(L"Recent Log", 44, 484, 220);
    HWND logLevel = addControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 416, 480, 132, 160, kLogLevelComboId);
    for (const wchar_t* level : {L"Trace", L"Debug", L"Info", L"Warn", L"Error"}) {
        SendMessageW(logLevel, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(level));
    }
    SendMessageW(logLevel, CB_SETCURSEL, static_cast<int>(settings_.logLevel), 0);
    addControl(L"BUTTON", L"Open Folder", BS_PUSHBUTTON | WS_TABSTOP, 560, 480, 132, 30, kOpenLogFolderButtonId);
    addControl(L"BUTTON", L"Save Log", BS_PUSHBUTTON | WS_TABSTOP, 704, 480, 120, 30, kSaveLogButtonId);
    diagnosticLogLabel_ = addControl(L"STATIC", L"", SS_LEFT, 44, 518, 780, 150, kDiagnosticLogLabelId);
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
    setListItemHeightIfChanged(clipList_, clipListItemHeight_, libraryViewMode_ == LibraryViewMode::Gallery ? kGalleryClipMinimumItemHeight : kListClipItemHeight);
    if (clipList_) {
        SetWindowSubclass(clipList_, clipListSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
    }

    refreshClips();
}

} // namespace backtrack
