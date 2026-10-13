#pragma once

#include "app/RecorderController.h"
#include "clips/ClipManager.h"
#include "hotkeys/HotkeyService.h"
#include "integrations/DiscordRichPresence.h"
#include "integrations/LeagueOfLegendsIntegration.h"
#include "platform/Win32Util.h"
#include "settings/SettingsStore.h"
#include "voice/VoiceCommandService.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
        App = 0,
        Capture = 1,
        Video = 2,
        Audio = 3,
        GameIntegrations = 4,
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
        bool sectionHeading = false;
        // Last client-space rect applied to the control by layoutCurrentPage().
        // Compared against the computed rect so wheel scrolling does not pay a
        // GetWindowRect+MapWindowPoints round trip per control per tick. Lives
        // inside LayoutItem so it is discarded together with the controls it
        // describes whenever a page is rebuilt.
        RECT placed{};
        bool placedValid = false;
    };

    struct ClipThumbnail {
        HBITMAP bitmap = nullptr;
        // Load-time mirrors of the matching thumbnailCache_ entry's tile copy
        // (see ThumbnailCacheEntry::tileBitmap). Non-owning and not used for
        // drawing: paint code resolves the owning entry directly so the tile
        // cache is regenerated on exactly one owner.
        HBITMAP tileBitmap = nullptr;
        int tileWidth = 0;
        int tileHeight = 0;
        bool loading = false;
        bool unavailable = false;
    };

    struct ThumbnailCacheEntry {
        std::filesystem::file_time_type modifiedTime{};
        uintmax_t bytes = 0;
        HBITMAP bitmap = nullptr;
        // Pre-scaled copy at the size last shown in a Gallery tile. Scaling a
        // 320x180 thumbnail with StretchBlt(HALFTONE) per tile per paint is
        // expensive; scale once here, then plain BitBlt. Regenerated whenever
        // the drawn size changes (window resize).
        HBITMAP tileBitmap = nullptr;
        int tileWidth = 0;
        int tileHeight = 0;
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

    // Auto record row: icon + name + remove, no mute toggle.
    struct AppListRowControls {
        HWND iconControl = nullptr;
        HWND nameLabel = nullptr;
        HWND removeButton = nullptr;
        HICON icon = nullptr;
    };

    struct PageCache {
        HWND host = nullptr;
        bool built = false;
        std::vector<HWND> controls;
        std::vector<LayoutItem> layoutItems;
        std::unordered_map<HWND, std::wstring> statusHelpTexts;
        int scrollY = 0;
        int contentHeight = 0;
        int wheelRemainder = 0;
    };

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK pageHostProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK clipListSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData);
    static LRESULT CALLBACK comboBoxSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData);
    static LRESULT CALLBACK statusHelpSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData);
    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void buildTabs();
    void switchPage(Page page);
    void clearPageControls();
    void clearSettingsBodyControls();
    void storeCurrentPageCache();
    void loadPageCache(Page page);
    void rebindPageControlPointers();
    void invalidatePageCache(Page page);
    void updateTabChrome();
    void updateSettingsCategoryChrome();
    void updateButtonHover(HWND control);
    void clearButtonHover();
    HWND addControl(const wchar_t* className, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id);
    HWND addSectionLabel(const wchar_t* text, int x, int y, int width);
    void setDefaultFont(HWND control);
    void createUpdateButton();
    void startUpdateCheck();
    void stopUpdateCheck();
    void handleUpdateCheckComplete(bool updateAvailable);

    void buildSettingsPage();
    void buildSettingsCategoryTabs(int y);
    void buildSettingsCategoryBody();
    void rebuildSettingsCategoryBody();
    void buildSettingsAppPage();
    void buildSettingsCapturePage();
    void buildSettingsVideoPage();
    void buildSettingsAudioPage();
    void buildSettingsGameIntegrationsPage();
    void buildCapturePage();
    void buildStatsPage();
    void buildClipsPage();
    void switchSettingsCategory(SettingsCategory category);

    void readVisibleSettingsInto(AppSettings& target);
    // Folder validation plus creation for the save path only. Kept out of
    // readVisibleSettingsInto so the per-keystroke dirty pass never touches
    // the filesystem. Reads the folders readVisibleSettingsInto just stored.
    bool ensureFoldersExist();
    // Single owner of sanitize + startup registration + save + dirty baseline,
    // so no save site can persist without normalizing or leave dirty tracking
    // stale. Hotkey registration stays in applyVisibleSettings: its result
    // feeds the ApplySettings controller action, not persistence.
    void commitSettings();
    bool computeSettingsDirty();
    void stashVisibleSettings();
    void applyVisibleSettings();
    void addDirtySaveButton();
    void updateSaveSettingsButton();
    void markSettingsDirty();
    void clearSettingsDirty();
    bool promptSaveSettingsIfDirty(bool allowCancel);
    void addStatusHelp(HWND control, const std::wstring& text);
    void updateStatusHelp(HWND hoveredControl);
    void applyStatusText(const std::wstring& status);
    int statusHeightForWidth(int width) const;
    void layoutWindow();
    bool layoutCurrentPage();
    void detachPageControl(HWND control);
    void scrollPageTo(int position);
    void scrollPageBy(int delta);
    void scrollPageWheel(WPARAM wParam);
    bool handleClipListMouseWheel(WPARAM wParam);
    void updateDiagnosticsTimer();
    void loadAudioDeviceCombos();
    // Shared executable-list core so a third list reuses instead of copying:
    // refreshAvailableApps fills the picker combo (hasMuteToggle selects audio-
    // session enumeration like sound separation uses; without it only visible
    // windows are listed like auto record), build/destroyAppRowCommon own the
    // icon + name + Remove row, and pick/selected/commit own the add paths.
    void refreshAvailableApps(HWND combo, std::vector<AudioSessionAppInfo>& available, bool hasMuteToggle);
    const AudioSessionAppInfo* selectedAvailableApp(HWND combo, const std::vector<AudioSessionAppInfo>& available);
    bool pickAppExecutable(std::filesystem::path& out);
    bool validateAppExecutablePath(const std::filesystem::path& executablePath);
    void commitAppListEdit(void (MainWindow::*rebuild)(), const wchar_t* status);
    void buildAppRowCommon(
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
        HWND& removeButton);
    void destroyAppRowCommon(HWND iconControl, HWND nameLabel, HWND removeButton, HICON& icon);
    void resetAppRowHover();
    template <typename Row>
    void buildEmptyAppListRow(std::vector<Row>& rows, int y, HWND combo);
    void refreshSoundSeparationApps();
    void addSelectedSoundSeparationApp();
    void addManualSoundSeparationApp();
    void addSoundSeparationApp(std::wstring name, std::filesystem::path executablePath);
    void rebuildSoundSeparationRows();
    void destroySoundSeparationRows();
    void parkFocusOnCombo(HWND combo);
    void releaseSoundSeparationRowIcons();
    void toggleSoundSeparationApp(size_t index);
    void removeSoundSeparationApp(size_t index);
    bool isSoundSeparationMutedLabel(HWND control) const;
    // The Library page has no section cards, so its title label paints text
    // directly on the page background instead of a panel-colored box.
    bool isLibraryTitleLabel(HWND control) const;
    void refreshAutoRecordApps();
    void addSelectedAutoRecordApp();
    void addManualAutoRecordApp();
    void addAutoRecordApp(std::wstring name, std::filesystem::path executablePath);
    void rebuildAutoRecordRows();
    void destroyAutoRecordRows();
    void releaseAutoRecordRowIcons();
    void removeAutoRecordApp(size_t index);
    void updateStartupRegistration();
    void browseClipFolder();
    void browseRingDirectory();
    void browseCustomNotificationSound();
    void saveLog();
    void updateStats();
    // Edge-triggered audio-liveness warning shown in the status bar while a
    // recording receives no packets from an enabled audio input (>2s).
    void updateAudioLivenessWarning(const RecordingStats& stats);
    void refreshClips();
    void populateClipListRows();
    void setLibraryViewMode(LibraryViewMode mode);
    void clearClipThumbnails();
    void releaseClipThumbnailCache();
    void pruneClipThumbnailCache();
    void loadClipThumbnails();
    // Re-queues missing thumbnails without bumping the generation, so rapid
    // single-clip edits do not starve the worker by discarding in-flight work.
    void refreshThumbnailsAfterLibraryEdit();
    void startThumbnailWorker();
    void stopThumbnailWorker();
    void thumbnailWorkerLoop();
    void handleClipThumbnailReady(ThumbnailResult& result);
    size_t clipIndexFromListItem(UINT itemId) const;
    size_t clipIndexAtPoint(POINT point) const;
    size_t clipIndexForPath(const std::filesystem::path& path) const;
    // Gallery tile geometry. The painter and the hit test both go through
    // these so a click can never land on a different tile than the one drawn.
    static RECT galleryTileRect(const RECT& row, int column);
    static int galleryTileColumnAt(const RECT& row, int x);
    void armClipDrag(POINT point);
    void beginClipDrag();
    void onClipSelectionChanged();
    void openSelectedClip();
    void deleteSelectedClip();
    void renameSelectedClip();
    void toggleSelectedFavorite();
    std::filesystem::path selectedClipPath() const;
    void handleHotkey(int id);
    void requestSaveReplay(const wchar_t* source);
    bool handleShortcut(MSG& message);
    void updateGameIntegrations();
    void stopGameIntegrations();
    void handleLeagueKillDetected();
    // Auto record watcher: polls running apps on a timer and toggles recording
    // through the controller queue. See pollAutoRecord().
    void pollAutoRecord();
    std::optional<std::wstring> runningAutoRecordAppName() const;
    void startRecordingForAutoRecord(const std::wstring& appName);
    void stopRecordingForAutoRecord();
    void startControllerWorker();
    void stopControllerWorker();
    bool queueControllerAction(ControllerAction action, const std::wstring& busyStatus);
    void armControllerBusyTimer(ControllerActionKind kind);
    void clearControllerBusyTimer();
    void handleControllerBusySoftTimeout();
    void controllerWorkerLoop();
    ControllerActionResult executeControllerAction(const ControllerAction& action);
    void handleControllerActionComplete(const ControllerActionResult& result);
    bool addTrayIcon();
    void removeTrayIcon();
    void restoreFromTray();
    void showTrayMenu();
    void drawButtonItem(const DRAWITEMSTRUCT& item);
    void drawClipItem(const DRAWITEMSTRUCT& item);
    void updateLibraryActionButtons();
    void applyDarkTheme(HWND control);
    void setStatus(const std::wstring& status);
    std::wstring currentStatusDisplayText() const;
    int statusHeightForText(int width, const std::wstring& text) const;
    void invalidateClipRow(size_t clipIndex);

    RecorderController& controller_;
    SettingsStore& settingsStore_;
    AppSettings settings_;
    // Pristine baseline of the last saved/loaded settings. The Save button is
    // shown only when the visible settings differ from this snapshot, so
    // reverting an edit back to its saved value hides the button again.
    AppSettings savedSettings_;
    ClipManager clipManager_;
    HotkeyService hotkeys_;
    LeagueOfLegendsIntegration leagueIntegration_;
    DiscordRichPresence discordRichPresence_;
    VoiceCommandService voiceCommandService_;

    HWND window_ = nullptr;
    HWND pageHost_ = nullptr;
    HWND tabs_ = nullptr;
    std::vector<HWND> tabButtons_;
    std::vector<HWND> settingsCategoryButtons_;
    HWND status_ = nullptr;
    HWND saveSettingsButton_ = nullptr;
    HWND updateButton_ = nullptr;
    HWND startStopButton_ = nullptr;
    HFONT font_ = nullptr;
    HFONT headingFont_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH controlBrush_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    HBRUSH selectionBrush_ = nullptr;
    HBRUSH favoriteBrush_ = nullptr;
    HBRUSH tabActiveBrush_ = nullptr;
    HBRUSH buttonHoverBrush_ = nullptr;
    HBRUSH buttonPressedBrush_ = nullptr;
    HBRUSH saveButtonBrush_ = nullptr;
    HPEN outlinePen_ = nullptr;
    HPEN selectedOutlinePen_ = nullptr;
    HPEN tabActiveOutlinePen_ = nullptr;
    // Cached pens/brushes for the sound-separation mute button icon, one per
    // state (muted / normal / disabled). Previously created and deleted on
    // every owner-draw paint.
    HPEN muteIconPenMuted_ = nullptr;
    HPEN muteIconPenNormal_ = nullptr;
    HPEN muteIconPenDisabled_ = nullptr;
    HBRUSH muteIconBrushMuted_ = nullptr;
    HBRUSH muteIconBrushNormal_ = nullptr;
    HBRUSH muteIconBrushDisabled_ = nullptr;
    Page page_ = Page::Capture;
    SettingsCategory settingsCategory_ = SettingsCategory::App;
    std::array<PageCache, 4> pageCaches_{};
    std::vector<HWND> pageControls_;
    std::vector<ClipInfo> clips_;

    HWND bitrateEdit_ = nullptr;
    HWND codecCombo_ = nullptr;
    HWND recordHotkey_ = nullptr;
    HWND clipFolderEdit_ = nullptr;
    HWND organizeClipsByGameCheck_ = nullptr;
    HWND replayEnabledCheck_ = nullptr;
    HWND replaySecondsEdit_ = nullptr;
    HWND replayDiskBackedCheck_ = nullptr;
    HWND replayDiskBackedAudioCheck_ = nullptr;
    HWND replayRingDirEdit_ = nullptr;
    HWND replayHotkey_ = nullptr;
    HWND leagueKillReminderCheck_ = nullptr;
    HWND voiceCommandCombo_ = nullptr;
    HWND discordRichPresenceCombo_ = nullptr;
    HWND discordPresenceTimerCombo_ = nullptr;
    HWND statsLabel_ = nullptr;
    HWND capsLabel_ = nullptr;
    HWND diagnosticLogLabel_ = nullptr;
    HWND clipList_ = nullptr;
    HWND listViewButton_ = nullptr;
    HWND galleryViewButton_ = nullptr;
    HWND renameEdit_ = nullptr;
    HWND renameEditButton_ = nullptr;
    HWND fpsEdit_ = nullptr;
    HWND resolutionModeCombo_ = nullptr;
    // Preset items (from kResolutionPresetComboEntries) precede the monitor
    // entries in resolutionModeCombo_. Combo index for monitor entry i is the
    // preset table size + i. Selecting one sets encode width/height to that
    // monitor's native size (mode stays Custom).
    std::vector<MonitorEnumEntry> resolutionMonitorEntries_;
    HWND widthEdit_ = nullptr;
    HWND heightEdit_ = nullptr;
    HWND followFocusedMonitorCheck_ = nullptr;
    HWND multiMonitorSupportCheck_ = nullptr;
    HWND followMouseMonitorCheck_ = nullptr;
    HWND captureCursorCheck_ = nullptr;
    HWND systemAudioCheck_ = nullptr;
    HWND microphoneCheck_ = nullptr;
    HWND outputDeviceCombo_ = nullptr;
    HWND inputDeviceCombo_ = nullptr;
    HWND outputVolumeEdit_ = nullptr;
    HWND inputVolumeEdit_ = nullptr;
    HWND inputAutoGainCheck_ = nullptr;
#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
    HWND inputNoiseSuppressionCheck_ = nullptr;
#endif
    HWND startWithWindowsCheck_ = nullptr;
    HWND pruneStaleMicrophoneConsentEntriesCheck_ = nullptr;
    HWND exitToTrayCheck_ = nullptr;
    HWND checkForUpdatesCheck_ = nullptr;
    HWND notificationSoundVolumeEdit_ = nullptr;
    HWND customNotificationSoundEdit_ = nullptr;
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
    HWND captureMethodCombo_ = nullptr;
    HWND stableMultimonitorFramesCheck_ = nullptr;
    HWND soundSeparationEnabledCheck_ = nullptr;
    HWND soundSeparationAppCombo_ = nullptr;
    HWND soundSeparationRefreshButton_ = nullptr;
    HWND soundSeparationManualButton_ = nullptr;
    HWND autoRecordEnabledCheck_ = nullptr;
    HWND autoRecordAppCombo_ = nullptr;
    HWND autoRecordRefreshButton_ = nullptr;
    HWND autoRecordManualButton_ = nullptr;
    bool trayVisible_ = false;
    bool exitFromTray_ = false;
    bool settingsDirty_ = false;
    bool updateAvailable_ = false;
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
    // Gap-click suppression: armClipDrag runs before the listbox moves its own
    // row cursor, so a gap click records the pre-click cursor and asks
    // onClipSelectionChanged to restore it instead of following to the row.
    bool suppressGalleryFollow_ = false;
    int gapPrevRow_ = 0;
    std::wstring statusText_ = L"Ready";
    std::wstring displayedStatusText_;
    // Status-bar height held onto while a help text is hovered. Moving the
    // mouse across controls whose help texts wrap differently must not run a
    // full window relayout per hover change: the height only grows while the
    // hover session lasts and shrinks back when the pointer leaves.
    int hoverStickyStatusHeight_ = 0;
    // Current audio-liveness warning (empty when healthy). Appended to the
    // status bar text by currentStatusDisplayText().
    std::wstring audioLivenessWarning_;
    HWND hoveredHelpControl_ = nullptr;
    HWND hoveredButton_ = nullptr;
    bool buttonHoverTracking_ = false;
    std::unordered_map<HWND, std::wstring> statusHelpTexts_;
    std::vector<LayoutItem> layoutItems_;
    std::vector<RECT> sectionPanels_;
    std::vector<AudioDeviceInfo> outputDevices_;
    std::vector<AudioDeviceInfo> inputDevices_;
    std::vector<AudioSessionAppInfo> soundSeparationAvailableApps_;
    std::vector<SoundSeparationRowControls> soundSeparationRows_;
    // HWNDs currently showing the muted state. The CTLCOLOR handler themes by
    // membership here instead of reading the label text back out of the UI.
    std::unordered_set<HWND> mutedLabels_;
    std::vector<AudioSessionAppInfo> autoRecordAvailableApps_;
    std::vector<AppListRowControls> autoRecordRows_;
    std::vector<ClipThumbnail> clipThumbnails_;
    std::unordered_map<std::wstring, ThumbnailCacheEntry> thumbnailCache_;
    uint32_t recordHotkeyModifiers_ = 0;
    uint32_t recordHotkeyVirtualKey_ = 0;
    uint32_t replayHotkeyModifiers_ = 0;
    uint32_t replayHotkeyVirtualKey_ = 0;
    int soundSeparationRowsY_ = 0;
    int autoRecordRowsY_ = 0;
    // Auto record watcher state. ownsRecording_ marks a recording that auto
    // record started (so it stops it when the app exits); suppressStart_ holds
    // off restarting after the user manually stopped an owned recording while
    // the app is still running.
    bool autoRecordAppRunning_ = false;
    bool autoRecordOwnsRecording_ = false;
    bool autoRecordSuppressStart_ = false;
    std::thread controllerWorker_;
    std::mutex controllerQueueMutex_;
    std::condition_variable controllerQueueCv_;
    std::deque<ControllerAction> controllerQueue_;
    std::atomic<bool> controllerActionPending_{false};
    bool controllerWorkerStopping_ = false;
    bool controllerBusyTimerActive_ = false;
    std::wstring controllerBusyStatus_;
    std::thread updateCheckThread_;
    std::atomic<bool> updateCheckStopping_{false};
    std::thread thumbnailWorker_;
    std::mutex thumbnailQueueMutex_;
    std::condition_variable thumbnailQueueCv_;
    std::deque<ThumbnailRequest> thumbnailQueue_;
    std::atomic<uint64_t> thumbnailGeneration_{0};
    bool thumbnailWorkerStopping_ = false;
    SteadyClock::time_point killClipTagDeadline_ = SteadyClock::time_point::min();
};

} // namespace backtrack
