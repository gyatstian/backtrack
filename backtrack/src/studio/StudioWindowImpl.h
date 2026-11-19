#pragma once

#include "studio/StudioWindow.h"
#include "studio/Navigation.h"
#include "studio/StudioMotion.h"
#include "ui_common/RecorderActions.h"

#include "app/RecorderController.h"
#include "clips/ClipManager.h"
#include "hotkeys/HotkeyService.h"
#include "integrations/DiscordRichPresence.h"
#include "integrations/LeagueOfLegendsIntegration.h"
#include "voice/VoiceCommandService.h"

#include <CommCtrl.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace backtrack {

// A disabled replay must not keep its chord registered: the key then belongs to
// whatever the user runs instead, while the field keeps its stored binding.
inline HotkeySettings registeredHotkeys(const AppSettings& settings) {
    auto keys = settings.hotkeys;
    if (!settings.replay.enabled) keys.saveReplayModifiers = keys.saveReplayVirtualKey = 0;
    return keys;
}

// LoadIcon returns a shared handle that must never be destroyed, so the chrome
// and tray need owned copies. LoadIconWithScaleDown/LoadIconMetric give the best
// small glyph, but only comctl32 v6 exports them and this app links the v5 copy
// unless a common-controls manifest is active, so they are resolved at runtime
// with LoadImage (which also returns an owned icon) as the fallback.
inline HICON loadOwnedAppIcon(HINSTANCE instance, int id, int size) {
    using ScaleDown = HRESULT(WINAPI*)(HINSTANCE, PCWSTR, int, int, HICON*);
    static const auto scaleDown = [] {
        const auto library = GetModuleHandleW(L"comctl32.dll");
        return library ? reinterpret_cast<ScaleDown>(GetProcAddress(library, "LoadIconWithScaleDown")) : nullptr;
    }();
    HICON icon = nullptr;
    if (scaleDown && SUCCEEDED(scaleDown(instance, MAKEINTRESOURCEW(id), size, size, &icon)) && icon) return icon;
    return static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(id), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
}

inline HICON loadOwnedSmallAppIcon(HINSTANCE instance, int id) {
    using LoadMetric = HRESULT(WINAPI*)(HINSTANCE, PCWSTR, int, HICON*);
    static const auto loadMetric = [] {
        const auto library = GetModuleHandleW(L"comctl32.dll");
        return library ? reinterpret_cast<LoadMetric>(GetProcAddress(library, "LoadIconMetric")) : nullptr;
    }();
    HICON icon = nullptr;
    if (loadMetric && SUCCEEDED(loadMetric(instance, MAKEINTRESOURCEW(id), LIM_SMALL, &icon)) && icon) return icon;
    return loadOwnedAppIcon(instance, id, GetSystemMetrics(SM_CXSMICON));
}

struct StudioWindow::Impl {
    using Page = StudioNavigation::Page;
    using Category = StudioNavigation::Category;
    using Action = std::function<void()>;
    using ControlEvent = std::function<void(HWND, UINT)>;

    enum : UINT {
        actionReady = WM_APP + 1, trayEvent, galleryReady, leagueKill, releaseReady,
        voiceReplay = kVoiceCommandMessage, syncHelp, sessionStopped, devicesReady
    };
    enum : UINT_PTR { statusTimer = 1, diagnosticsTimer, autoRecordTimer, shutdownTimer, motionTimer };

    enum class ButtonTone { Normal, Primary, Quiet };
    enum Motion { mainMarker, categoryMarker, recordGlyph, replayCheck, librarySlide, motionCount };
    enum class Glyph { None, Minimize, Close };
    struct Control {
        ControlEvent event;
        std::wstring help;
        int margin = 0;
        bool checkbox = false;
        bool combo = false;
        bool pointerMotion = false;
        ButtonTone tone = ButtonTone::Normal;
        Glyph glyph = Glyph::None;
    };
    struct Row {
        std::vector<HWND> cells;
        std::wstring heading;
        int height = 0;
        bool split = false;
        bool tail = false;
        int inset = 0;
        std::array<int, 4> widths{};
        int dependencyDepth = 0;
        bool hidden = false;
    };
    enum class ClipEdit { Favorite, Remove, Rename };
    struct Gallery;

    Impl(RecorderController& controller, SettingsStore& store, const AppSettings& initial,
         SettingsStore::LoadStatus loadStatus, Action prepareCapture);
    ~Impl();
    bool create(HINSTANCE instance, int showCommand, bool startMinimized);
    int run();
    void shutdown();
    static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK bodyProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK controlProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT message(UINT, WPARAM, LPARAM);
    LRESULT bodyMessage(HWND, UINT, WPARAM, LPARAM);

    int px(int dip) const { return MulDiv(dip, static_cast<int>(dpi), 96); }
    int resizeGrip() const { return IsZoomed(window) ? 0 : px(6); }
    void updateFont();
    void updateAppIcon();
    void saveWindowPlacement();
    bool keyboardCuesHidden() const;
    void buildCaptionGlyphs();
    void layout(bool newPage = false);
    void layoutFooter(const RECT& client);
    void layoutRows(bool heightOnly = false, bool repaint = true);
    void scrollTo(int value, bool onPaint = false);
    void ensureVisible(HWND control);
    void open(Page page, bool pointer);
    void buildPage();
    void buildPage(StudioNavigation next);
    void syncNav(bool pointer);
    void updateMotionPreference();
    void stopMotion();
    void tickMotion();
    void setMotion(Motion slot, float target, bool animate);
    // Slides the library viewport in from the side the navigation came from.
    void startGallerySlide(bool forward);
    int gallerySlide() const;
    void invalidateMotion(Motion slot);
    float motionValue(Motion slot) const;
    void setHovered(HWND control);
    RECT markerTrack(Motion slot) const;
    RECT recordingGlyphBox(const RECT& client) const;
    void paintTabMarker(HDC dc, Motion slot);
    void paintRecordingGlyph(HDC dc, const RECT& box, bool disabled);
    void paintReplayCheck(HDC dc);
    void paintFrame(HDC dc, const RECT& dirty);
    void paintTitleBar(HDC dc, int width);
    void paintCaptionButton(const DRAWITEMSTRUCT& item, Glyph glyph);
    void paintButton(const DRAWITEMSTRUCT& item);
    void paintCheckbox(HWND control, HDC dc);
    void paintCombo(const DRAWITEMSTRUCT& item);
    void setStatus(std::wstring value, bool replaySaved = false, bool pointer = false);
    void paintFooter(bool animateCheck = false);
    bool handleShortcut(const MSG& message);
    bool editHotkey(HWND field, uint32_t modifiers, uint32_t key);

    HWND make(const wchar_t* type, const wchar_t* label, DWORD style, ControlEvent event = {},
              const wchar_t* help = L"", HWND parent = nullptr);
    HWND button(const wchar_t* label, Action click, const wchar_t* help = L"", HWND parent = nullptr,
                ButtonTone tone = ButtonTone::Normal);
    HWND captionButton(Glyph glyph, Action click);
    HWND editField(const std::wstring& value, DWORD style, ControlEvent onChange, const wchar_t* help);
    HWND edit(const wchar_t* label, const std::wstring& value, DWORD style, ControlEvent onChange,
              const wchar_t* help = L"");
    void section(const wchar_t* name);
    void row(std::initializer_list<HWND> controls, bool split = false, int height = 0);
    HWND checkbox(const wchar_t* title, bool& value, const wchar_t* help);
    void check(const wchar_t* title, bool& value, const wchar_t* help = L"");
    void number(const wchar_t* title, uint32_t& value, const uint32_t& fallback, const wchar_t* help = L"");
    HWND numberField(uint32_t& value, const uint32_t& fallback, const wchar_t* help);
    void path(const wchar_t* title, std::filesystem::path& value, bool folder, const wchar_t* help = L"");
    using Choices = std::vector<std::pair<std::wstring, Action>>;
    HWND choices(const wchar_t* title, const Choices& values, int selected, const wchar_t* help = L"");
    HWND hotkeyBox(uint32_t& modifiers, uint32_t& key);
    void appList(bool audio);
    void device(AudioTrack track, std::wstring& id);
    void startDeviceDiscovery();
    void completeDeviceDiscovery();
    void fillDeviceCombo(size_t index, const std::wstring& id);
    HWND lastField() const;
    void enabledWhen(std::function<bool()> predicate, int depth = 1);
    void visibleWhen(HWND control, std::function<bool()> predicate);
    HWND invalidNumber() const;
    bool dirty() const;
    void changed();
    bool saveSettings(bool refreshPage = true);
    bool leaveSettings(bool refreshPage);
    void buildCapture();
    void syncRecoveryAction();
    void addUpdateAction();
    void buildSettings();
    void buildDiagnostics();
    void layoutDiagnostics();
    void updateDiagnostics();
    void saveLog();

    void buildLibrary();
    void closeLibrary();
    void requestLibraryStop();
    bool libraryWorkerStopped();
    void destroyLibrary();
    void refreshLibrary();
    void selectClip(size_t index, bool focus = false);
    void openClip();
    void beginRename();
    void endRename();
    void editClip(ClipEdit operation);
    void updateGallery();
    // Page-wide library input (wheel, side mouse buttons); false on other pages.
    bool libraryInput(UINT message, WPARAM wp);
    void openFolder(size_t index);
    void backFolder();
    // Leaves any open folder for the library root. False when already there.
    bool libraryToRoot();
    void showLibraryFolder(std::vector<std::filesystem::path> stack, bool forward);
    void galleryChanged();
    bool galleryShortcut(const MSG& message);
    static LRESULT CALLBACK galleryProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    void layoutGallery();
    bool drawGallery(const DRAWITEMSTRUCT& item);

    void startSession();
    void requestSessionStop();
    void finishClose();
    void actionLoop(std::stop_token stop);
    bool submit(ControllerAction value, const wchar_t* status, bool pointer = false);
    void submit(ControllerActionKind kind, const wchar_t* status, bool pointer = false);
    void completeAction();
    void requestReplay(bool pointer = false);
    void configureIntegrations(const AppSettings& applied);
    void pollAutoRecord();
    bool addTray();
    void removeTray();
    void restore();
    void trayMenu();
    void close(bool forceExit);

    RecorderController& controller;
    SettingsStore& store;
    Action prepareCapture;
    AppSettings draft;
    AppSettings saved;
    bool readable;
    StudioNavigation nav;
    HWND window = nullptr;
    HWND body = nullptr;
    HWND titleMinimize = nullptr;
    HWND titleClose = nullptr;
    HICON appIcon = nullptr;
    HWND footer = nullptr;
    HWND save = nullptr;
    HWND record = nullptr;
    HWND replayButton = nullptr;
    HWND recover = nullptr;
    size_t recoveryRow = 0;
    bool recoveryAvailable = false;
    HWND diagnostics[3]{};
    std::array<HWND, 2> resolutionFields{};
    struct DeviceCombo {
        HWND hwnd = nullptr;
        std::vector<std::wstring> ids;
        bool refresh = false;
    };
    std::array<DeviceCombo, 2> deviceCombos;
    // Only the worker accesses the endpoint lists until the UI joins it.
    std::array<std::vector<AudioDeviceInfo>, 2> deviceEndpoints;
    std::atomic<bool> deviceWorkerDone{false};
    std::jthread deviceWorker;
    HWND hovered = nullptr;
    HWND focused = nullptr;
    HWND lastFocus = nullptr;
    // Rendered alpha for the two caption glyphs; rebuilt when the DPI changes.
    std::array<std::vector<BYTE>, 2> captionGlyphAlpha;
    std::array<SIZE, 2> captionGlyphSize{};
    unsigned captionGlyphDpi = 0;
    std::optional<HWND> clickOrigin;
    bool motionEnabled = false;
    std::array<StudioMotion, motionCount> motions;
    HWND update = nullptr;
    std::wstring status;
    // Last recorder or hotkey failure, kept on screen until a later action
    // succeeds; ordinary status text expires with statusTimer, an error must not.
    std::wstring persistentWarning;
    bool replaySaved = false;
    RECT replayCheckBox{};
    std::wstring audioWarning;
    std::array<HWND, 4> pageButtons{};
    std::array<HWND, 5> categoryButtons{};
    std::unordered_map<HWND, Control> controls;
    std::vector<Row> rows;
    std::vector<RECT> headingRects;
    std::unordered_map<HWND, std::pair<uint32_t*, uint32_t*>> hotkeyFields;
    std::unordered_set<HWND> invalidNumbers;
    std::vector<std::pair<size_t, std::function<bool()>>> conditions;
    std::vector<std::pair<HWND, std::function<bool()>>> visibility;
    unsigned dpi = 96;
    SIZE layoutSize{};
    int layoutGrip = 0;
    unsigned layoutDpi = 0;
    HFONT font = nullptr;
    HBRUSH surfaceBrush = nullptr;
    HBRUSH fieldBrush = nullptr;
    HBRUSH activeBrush = nullptr;
    int scroll = 0;
    int wheel = 0;
    int rowWidth = 0;
    std::optional<int> pendingScroll;
    bool sizing = false;
    bool building = false;
    bool arranging = false;
    bool started = false;
    bool tray = false;
    bool closing = false;
    bool shuttingDown = false;
    UINT taskbarCreated = 0;
    HICON trayIcon = nullptr;
    Gallery* gallery = nullptr;

    HotkeyService hotkeys;
    AutoRecordOwnership autoRecord;
    AudioSessionAppInfo matchedApp;
    SteadyClock::time_point killDeadline = SteadyClock::time_point::min();
    LeagueOfLegendsIntegration league;
    DiscordRichPresence discord;
    VoiceCommandService voice;
    std::atomic<bool> voiceEvents{false};
    std::atomic<bool> leagueEvents{false};
    std::atomic<bool> checkStopping{false};
    std::atomic<bool> releaseWorkerDone{false};
    std::atomic<bool> actionWorkerDone{false};
    std::jthread releaseWorker;
    std::jthread actionWorker;
    std::mutex actionMutex;
    std::condition_variable_any actionCv;
    std::optional<ControllerAction> pendingAction;
    bool actionAccepting = true;
    std::optional<ControllerActionResult> actionResult;
    std::optional<bool> recoveryResult;
    std::optional<AudioSessionAppInfo> autoProbe;
    std::optional<AudioSessionAppInfo> autoResult;
    uint64_t actionRevision = 0;
    bool busy = false;
    bool applyingSettings = false;
    bool actionFromPointer = false;
    // The encoder report only changes when an action applies settings or a
    // backend is re-initialized, so it is rebuilt per action revision, not per
    // diagnostics poll.
    std::wstring encoderText;
    uint64_t encoderTextRevision = 0;
};

} // namespace backtrack
