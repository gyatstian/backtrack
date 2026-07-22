#pragma once

#include "app/RecorderController.h"
#include "clips/ClipManager.h"
#include "hotkeys/HotkeyService.h"
#include "integrations/LeagueOfLegendsIntegration.h"
#include "settings/SettingsStore.h"

#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace backtrack {

class MainWindow {
public:
    MainWindow(RecorderController& controller, SettingsStore& settingsStore);
    ~MainWindow();

    bool create(HINSTANCE instance, int showCommand, bool startMinimized);
    int runMessageLoop();

private:
    enum class Page {
        Capture = 0,
        Library = 1,
        Settings = 2,
        Diagnostics = 3,
    };

    enum class SettingsCategory {
        General = 0,
        Advanced = 1,
        SoundSeparation = 2,
        GameIntegrations = 3,
    };

    enum class LibraryViewMode {
        List,
        Gallery,
    };

    enum class ControllerActionKind {
        Startup,
        ApplySettings,
        ToggleRecording,
        SaveReplay,
        RecoverFailedRecording,
    };

    struct ControllerAction {
        ControllerActionKind kind = ControllerActionKind::Startup;
        AppSettings settings;
        bool hotkeysOk = true;
        std::wstring hotkeyError;
        std::wstring replayTag;
    };

    struct ControllerActionResult {
        ControllerActionKind kind = ControllerActionKind::Startup;
        bool ok = false;
        bool startedRecording = false;
        bool stoppedRecording = false;
        bool savedReplay = false;
        bool recoveredRecording = false;
        bool refreshLibrary = false;
        std::filesystem::path clipPath;
        std::wstring status;
    };

    struct LayoutItem {
        enum class Kind {
            Content,
            Footer,
        };

        HWND control = nullptr;
        RECT design{};
        int windowHeight = 0;
        Kind kind = Kind::Content;
    };

    struct ClipThumbnail {
        HBITMAP bitmap = nullptr;
        bool loading = false;
        bool unavailable = false;
    };

    struct ThumbnailCacheEntry {
        std::filesystem::file_time_type modifiedTime{};
        uintmax_t bytes = 0;
        HBITMAP bitmap = nullptr;
        bool unavailable = false;
    };

    struct ThumbnailRequest {
        uint64_t generation = 0;
        size_t clipIndex = static_cast<size_t>(-1);
        std::filesystem::path path;
        std::filesystem::file_time_type modifiedTime{};
        uintmax_t bytes = 0;
    };

    struct ThumbnailResult {
        uint64_t generation = 0;
        size_t clipIndex = static_cast<size_t>(-1);
        std::filesystem::path path;
        std::filesystem::file_time_type modifiedTime{};
        uintmax_t bytes = 0;
        HBITMAP bitmap = nullptr;
    };

    struct SoundSeparationRowControls {
        HWND micButton = nullptr;
        HWND iconControl = nullptr;
        HWND nameLabel = nullptr;
        HWND mutedLabel = nullptr;
        HWND removeButton = nullptr;
        HICON icon = nullptr;
    };

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK pageHostProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK clipListSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData);
    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void buildTabs();
    void switchPage(Page page);
    void clearPageControls();
    HWND addControl(const wchar_t* className, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id);
    HWND addSectionLabel(const wchar_t* text, int x, int y, int width);
    void setDefaultFont(HWND control);

    void buildSettingsPage();
    void buildSettingsCategoryTabs(int y);
    void buildSettingsGeneralPage();
    void buildSettingsAdvancedPage();
    void buildSettingsSoundSeparationPage();
    void buildSettingsGameIntegrationsPage();
    void buildCapturePage();
    void buildStatsPage();
    void buildClipsPage();
    void switchSettingsCategory(SettingsCategory category);

    void applyVisibleSettings();
    void addDirtySaveButton();
    void markSettingsDirty();
    void clearSettingsDirty();
    void addSettingHelp(HWND control, int x, int y, const std::wstring& text);
    void addStatusHelp(HWND control, const std::wstring& text);
    void updateStatusHelp(HWND hoveredControl);
    void applyStatusText(const std::wstring& status);
    int statusHeightForWidth(int width) const;
    void layoutWindow();
    bool layoutCurrentPage();
    void scrollPageTo(int position);
    void scrollPageBy(int delta);
    void scrollPageWheel(WPARAM wParam);
    bool handleClipListMouseWheel(WPARAM wParam);
    void updateDiagnosticsTimer();
    void loadAudioDeviceCombos();
    void refreshSoundSeparationApps();
    void addSelectedSoundSeparationApp();
    void addManualSoundSeparationApp();
    void addSoundSeparationApp(std::wstring name, std::filesystem::path executablePath);
    void rebuildSoundSeparationRows();
    void destroySoundSeparationRows();
    void releaseSoundSeparationRowIcons();
    void toggleSoundSeparationApp(size_t index);
    void removeSoundSeparationApp(size_t index);
    bool isSoundSeparationMutedLabel(HWND control) const;
    void updateResolutionControls();
    void updateStartupRegistration();
    void browseClipFolder();
    void saveLog();
    void updateStats();
    void refreshClips();
    void setLibraryViewMode(LibraryViewMode mode);
    void clearClipThumbnails();
    void releaseClipThumbnailCache();
    void pruneClipThumbnailCache();
    void loadClipThumbnails();
    void startThumbnailWorker();
    void stopThumbnailWorker();
    void thumbnailWorkerLoop();
    void handleClipThumbnailReady(ThumbnailResult& result);
    size_t clipIndexFromListItem(UINT itemId) const;
    size_t clipIndexAtPoint(POINT point) const;
    void updateGallerySelectionFromCursor();
    void armClipDrag(POINT point);
    void beginClipDrag();
    void onClipSelectionChanged();
    void openSelectedClip();
    void deleteSelectedClip();
    void renameSelectedClip();
    void toggleSelectedFavorite();
    std::filesystem::path selectedClipPath() const;
    void handleHotkey(int id);
    bool handleShortcut(MSG& message);
    void updateGameIntegrations();
    void stopGameIntegrations();
    void handleLeagueKillDetected();
    void startControllerWorker();
    void stopControllerWorker();
    bool queueControllerAction(ControllerAction action, const std::wstring& busyStatus);
    void controllerWorkerLoop();
    ControllerActionResult executeControllerAction(const ControllerAction& action);
    void handleControllerActionComplete(const ControllerActionResult& result);
    void addTrayIcon();
    void removeTrayIcon();
    void restoreFromTray();
    void showTrayMenu();
    void drawButtonItem(const DRAWITEMSTRUCT& item);
    void drawClipItem(const DRAWITEMSTRUCT& item);
    void applyDarkTheme(HWND control);
    void setStatus(const std::wstring& status);
    std::wstring currentStatusDisplayText() const;
    int statusHeightForText(int width, const std::wstring& text) const;
    void invalidateClipRow(size_t clipIndex);

    RecorderController& controller_;
    SettingsStore& settingsStore_;
    AppSettings settings_;
    ClipManager clipManager_;
    HotkeyService hotkeys_;
    LeagueOfLegendsIntegration leagueIntegration_;

    HWND window_ = nullptr;
    HWND pageHost_ = nullptr;
    HWND tabs_ = nullptr;
    std::vector<HWND> tabButtons_;
    std::vector<HWND> settingsCategoryButtons_;
    HWND status_ = nullptr;
    HWND saveSettingsButton_ = nullptr;
    HWND startStopButton_ = nullptr;
    HFONT font_ = nullptr;
    HFONT headingFont_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH controlBrush_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    HBRUSH selectionBrush_ = nullptr;
    HBRUSH favoriteBrush_ = nullptr;
    HPEN outlinePen_ = nullptr;
    HPEN selectedOutlinePen_ = nullptr;
    Page page_ = Page::Capture;
    SettingsCategory settingsCategory_ = SettingsCategory::General;
    std::vector<HWND> pageControls_;
    std::vector<ClipInfo> clips_;

    HWND bitrateEdit_ = nullptr;
    HWND codecCombo_ = nullptr;
    HWND recordHotkey_ = nullptr;
    HWND clipFolderEdit_ = nullptr;
    HWND replayEnabledCheck_ = nullptr;
    HWND replaySecondsEdit_ = nullptr;
    HWND replayHotkey_ = nullptr;
    HWND leagueKillReminderCheck_ = nullptr;
    HWND statsLabel_ = nullptr;
    HWND capsLabel_ = nullptr;
    HWND diagnosticLogLabel_ = nullptr;
    HWND clipList_ = nullptr;
    HWND listViewButton_ = nullptr;
    HWND galleryViewButton_ = nullptr;
    HWND renameEdit_ = nullptr;
    HWND fpsEdit_ = nullptr;
    HWND resolutionModeCombo_ = nullptr;
    HWND widthEdit_ = nullptr;
    HWND heightEdit_ = nullptr;
    HWND followFocusedMonitorCheck_ = nullptr;
    HWND followMouseMonitorCheck_ = nullptr;
    HWND systemAudioCheck_ = nullptr;
    HWND microphoneCheck_ = nullptr;
    HWND outputDeviceCombo_ = nullptr;
    HWND inputDeviceCombo_ = nullptr;
    HWND outputVolumeEdit_ = nullptr;
    HWND inputVolumeEdit_ = nullptr;
    HWND startWithWindowsCheck_ = nullptr;
    HWND exitToTrayCheck_ = nullptr;
    HWND encoderPresetCombo_ = nullptr;
    HWND encoderModeCombo_ = nullptr;
    HWND encoderProfileCombo_ = nullptr;
    HWND encoderLookaheadCheck_ = nullptr;
    HWND encoderLookaheadDepthEdit_ = nullptr;
    HWND encoderSpatialAQCheck_ = nullptr;
    HWND encoderAQStrengthEdit_ = nullptr;
    HWND encoderTemporalAQCheck_ = nullptr;
    HWND encoderMultipassCombo_ = nullptr;
    HWND encoderBFramesCheck_ = nullptr;
    HWND encoderAdaptiveBFramesCheck_ = nullptr;
    HWND encoderAdaptiveIFramesCheck_ = nullptr;
    HWND encoderZeroReorderDelayCheck_ = nullptr;
    HWND encoderGopSecondsEdit_ = nullptr;
    HWND encoderReferenceFramesEdit_ = nullptr;
    HWND gpuAdaptiveCombo_ = nullptr;
    HWND gpuFrameQueueLimitEdit_ = nullptr;
    HWND idleFrameCoalescingCheck_ = nullptr;
    HWND wgcZeroCopyCheck_ = nullptr;
    HWND stableMultimonitorFramesCheck_ = nullptr;
    HWND soundSeparationEnabledCheck_ = nullptr;
    HWND soundSeparationAppCombo_ = nullptr;
    HWND soundSeparationRefreshButton_ = nullptr;
    HWND soundSeparationManualButton_ = nullptr;
    bool trayVisible_ = false;
    bool exitFromTray_ = false;
    bool settingsDirty_ = false;
    bool buildingPage_ = false;
    bool diagnosticsTimerActive_ = false;
    LibraryViewMode libraryViewMode_ = LibraryViewMode::List;
    size_t selectedClipIndex_ = static_cast<size_t>(-1);
    int pageScrollY_ = 0;
    int pageWheelRemainder_ = 0;
    int pageContentHeight_ = 0;
    int clipListItemHeight_ = 0;
    int clipListWheelRemainder_ = 0;
    bool clipDragArmed_ = false;
    POINT clipDragStartPoint_{};
    std::filesystem::path clipDragPath_;
    std::wstring statusText_ = L"Ready";
    std::wstring displayedStatusText_;
    HWND hoveredHelpControl_ = nullptr;
    std::unordered_map<HWND, std::wstring> statusHelpTexts_;
    std::vector<LayoutItem> layoutItems_;
    std::vector<AudioDeviceInfo> outputDevices_;
    std::vector<AudioDeviceInfo> inputDevices_;
    std::vector<AudioSessionAppInfo> soundSeparationAvailableApps_;
    std::vector<SoundSeparationRowControls> soundSeparationRows_;
    std::vector<ClipThumbnail> clipThumbnails_;
    std::unordered_map<std::wstring, ThumbnailCacheEntry> thumbnailCache_;
    uint32_t recordHotkeyModifiers_ = 0;
    uint32_t recordHotkeyVirtualKey_ = 0;
    uint32_t replayHotkeyModifiers_ = 0;
    uint32_t replayHotkeyVirtualKey_ = 0;
    int soundSeparationRowsY_ = 0;
    std::thread controllerWorker_;
    std::mutex controllerQueueMutex_;
    std::condition_variable controllerQueueCv_;
    std::deque<ControllerAction> controllerQueue_;
    std::atomic<bool> controllerActionPending_{false};
    bool controllerWorkerStopping_ = false;
    std::thread thumbnailWorker_;
    std::mutex thumbnailQueueMutex_;
    std::condition_variable thumbnailQueueCv_;
    std::deque<ThumbnailRequest> thumbnailQueue_;
    std::atomic<uint64_t> thumbnailGeneration_{0};
    bool thumbnailWorkerStopping_ = false;
    SteadyClock::time_point killClipTagDeadline_ = SteadyClock::time_point::min();
};

} // namespace backtrack
