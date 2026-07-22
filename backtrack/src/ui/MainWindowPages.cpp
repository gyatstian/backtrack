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
}

void MainWindow::switchPage(Page page) {
    page_ = page;
    settingsDirty_ = false;
    pageScrollY_ = 0;
    pageWheelRemainder_ = 0;
    clipListWheelRemainder_ = 0;
    if (pageHost_) {
        SendMessageW(pageHost_, WM_SETREDRAW, FALSE, 0);
    }
    clearPageControls();
    for (size_t index = 0; index < tabButtons_.size(); ++index) {
        EnableWindow(tabButtons_[index], index != static_cast<size_t>(page_));
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
    clearSettingsDirty();
    applyStatusText(statusText_);
    layoutCurrentPage();
    if (pageHost_) {
        SendMessageW(pageHost_, WM_SETREDRAW, TRUE, 0);
        redrawWindowAndChildren(pageHost_, true);
    }
    redrawWindowAndChildren(window_);
    updateDiagnosticsTimer();
}

void MainWindow::switchSettingsCategory(SettingsCategory category) {
    settingsCategory_ = category;
    switchPage(Page::Settings);
}

void MainWindow::clearPageControls() {
    for (HWND control : pageControls_) {
        DestroyWindow(control);
    }
    releaseSoundSeparationRowIcons();
    pageControls_.clear();
    layoutItems_.clear();
    statusHelpTexts_.clear();
    hoveredHelpControl_ = nullptr;
    applyStatusText(statusText_);
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
    clearClipThumbnails();
    fpsEdit_ = nullptr;
    resolutionModeCombo_ = nullptr;
    widthEdit_ = nullptr;
    heightEdit_ = nullptr;
    followFocusedMonitorCheck_ = nullptr;
    followMouseMonitorCheck_ = nullptr;
    systemAudioCheck_ = nullptr;
    microphoneCheck_ = nullptr;
    outputDeviceCombo_ = nullptr;
    inputDeviceCombo_ = nullptr;
    outputVolumeEdit_ = nullptr;
    inputVolumeEdit_ = nullptr;
    startWithWindowsCheck_ = nullptr;
    exitToTrayCheck_ = nullptr;
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
    wgcZeroCopyCheck_ = nullptr;
    stableMultimonitorFramesCheck_ = nullptr;
    soundSeparationEnabledCheck_ = nullptr;
    soundSeparationAppCombo_ = nullptr;
    soundSeparationRefreshButton_ = nullptr;
    soundSeparationManualButton_ = nullptr;
    soundSeparationAvailableApps_.clear();
    soundSeparationRowsY_ = 0;
    clipListItemHeight_ = 0;
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
    settings_ = controller_.settings();
    buildSettingsCategoryTabs(78);
    if (settingsCategory_ == SettingsCategory::General) {
        buildSettingsGeneralPage();
    } else if (settingsCategory_ == SettingsCategory::Advanced) {
        buildSettingsAdvancedPage();
    } else if (settingsCategory_ == SettingsCategory::SoundSeparation) {
        buildSettingsSoundSeparationPage();
    } else {
        buildSettingsGameIntegrationsPage();
    }
    addDirtySaveButton();
}

void MainWindow::buildSettingsCategoryTabs(int y) {
    constexpr int kTabX = 44;
    constexpr int kTabWidth = 150;
    constexpr int kTabHeight = 30;
    constexpr int kTabGap = 8;
    const wchar_t* labels[] = {L"General", L"Advanced", L"Sound seperation", L"Game integrations"};

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
        EnableWindow(button, index != static_cast<int>(settingsCategory_));
    }
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

    addRowLabel(L"Close button");
    exitToTrayCheck_ = addControl(L"BUTTON", L"Exit into tray", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kExitToTrayCheckId);
    SendMessageW(exitToTrayCheck_, BM_SETCHECK, settings_.exitToTray ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(exitToTrayCheck_, 0, 0, L"Closing the window hides Backtrack in the tray instead of shutting down recording and replay services.");
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

    addRowLabel(L"WGC capture");
    wgcZeroCopyCheck_ = addControl(L"BUTTON", L"Zero-copy textures", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kWgcZeroCopyCheckId);
    SendMessageW(wgcZeroCopyCheck_, BM_SETCHECK, settings_.gpu.wgcZeroCopy ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(wgcZeroCopyCheck_, 0, 0, L"Uses the Windows Graphics Capture frame-pool texture directly when its native BGRA format can be submitted without conversion. Scaling or experimental format conversion may use a separate pooled output texture.");
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

    addSection(L"Sound seperation");
    addRowLabel(L"Mode");
    soundSeparationEnabledCheck_ = addControl(
        L"BUTTON",
        L"Enable sound seperation",
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
    addSettingHelp(soundSeparationAppCombo_, 0, 0, L"Choose a running app to add it to sound seperation. Apps only affect clips while sound seperation is enabled and the app row is muted.");
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
    settings_ = controller_.settings();

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
    startStopButton_ = addControl(L"BUTTON", controller_.stats().recording ? L"Stop Recording" : L"Start Recording", BS_PUSHBUTTON | WS_TABSTOP, kControlX, y - 4, 170, 34, kStartStopButtonId);
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
    addControl(L"BUTTON", L"Save Log", BS_PUSHBUTTON | WS_TABSTOP, 712, 480, 112, 30, kSaveLogButtonId);
    diagnosticLogLabel_ = addControl(L"STATIC", L"", SS_LEFT, 44, 518, 780, 150, -1);
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
