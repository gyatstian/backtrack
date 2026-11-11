#include "ui/MainWindow.h"
#include "core/Logger.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace backtrack {

struct MainWindowSettingsTestAccess {
    static AppSettings& draft(MainWindow& window) { return window.settings_; }
    static const AppSettings& saved(const MainWindow& window) { return window.savedSettings_; }
    static bool dirty(const MainWindow& window) { return window.settingsDirty_; }
    static const std::wstring& status(const MainWindow& window) { return window.statusText_; }
    static void markDirty(MainWindow& window) { window.markSettingsDirty(); }
    static void apply(MainWindow& window) { window.applyVisibleSettings(); }
    static void loggingEnabled(MainWindow& window, bool enabled) { window.setLoggingEnabled(enabled); }
    static void gallery(MainWindow& window, bool enabled) {
        window.setLibraryViewMode(enabled ? MainWindow::LibraryViewMode::Gallery : MainWindow::LibraryViewMode::List);
    }
    static bool commitDraft(MainWindow& window) { return window.commitSettings(window.settings_); }
    static void read(MainWindow& window, AppSettings& target) { window.readVisibleSettingsInto(target); }
    static void enumControls(MainWindow& window, HWND control) {
        window.codecCombo_ = control;
        window.encoderPresetCombo_ = control;
        window.encoderModeCombo_ = control;
        window.encoderProfileCombo_ = control;
        window.encoderMultipassCombo_ = control;
        window.gpuAdaptiveCombo_ = control;
        window.captureMethodCombo_ = control;
        window.voiceCommandCombo_ = control;
        window.discordRichPresenceCombo_ = control;
    }
    static void monitorControls(MainWindow& window, HWND multi, HWND focused, HWND mouse) {
        window.multiMonitorSupportCheck_ = multi;
        window.followFocusedMonitorCheck_ = focused;
        window.followMouseMonitorCheck_ = mouse;
    }
    static void bindVoice(MainWindow& window, HWND control) {
        window.voiceCommandCombo_ = control;
        window.pageControls_.push_back(control);
    }
    static HWND voice(const MainWindow& window) { return window.voiceCommandCombo_; }
    static void clearControls(MainWindow& window) { window.clearPageControls(); }
    static void rebind(MainWindow& window) { window.rebindPageControlPointers(); }
    static void switchPages(MainWindow& window) {
        window.clearSettingsDirty();
        window.switchPage(MainWindow::Page::Settings);
        window.switchPage(MainWindow::Page::Capture);
        window.clearPageControls();
    }
    static bool applyQueued(MainWindow& window) {
        if (window.controllerQueue_.empty()) {
            return false;
        }
        const auto action = std::move(window.controllerQueue_.front());
        window.controllerQueue_.pop_front();
        const auto result = window.executeControllerAction(action);
        window.controllerActionPending_ = false;
        return result.ok;
    }
    static bool poll(MainWindow& window, const std::vector<AudioSessionAppInfo>& openApps) {
        window.pollAutoRecord(&openApps);
        const bool queued = !window.controllerQueue_.empty();
        window.controllerQueue_.clear();
        window.controllerActionPending_ = false;
        window.autoRecordAppRunning_ = false;
        window.autoRecordOwnsRecording_ = false;
        window.autoRecordSuppressStart_ = false;
        return queued;
    }
};

} // namespace backtrack

namespace {

using namespace backtrack;

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path testRoot() {
    return std::filesystem::temp_directory_path() /
        (L"backtrack_ui_settings_test_" + std::to_wstring(GetCurrentProcessId()));
}

std::vector<char> readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    expect(input.is_open(), "could not read settings fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class ExclusiveFile {
public:
    explicit ExclusiveFile(const std::filesystem::path& path)
        : handle_(CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr)) {
        expect(handle_ != INVALID_HANDLE_VALUE, "could not lock settings fixture");
    }
    ~ExclusiveFile() { CloseHandle(handle_); }
    ExclusiveFile(const ExclusiveFile&) = delete;
    ExclusiveFile& operator=(const ExclusiveFile&) = delete;
private:
    HANDLE handle_;
};

AppSettings fixture(const std::filesystem::path& directory) {
    AppSettings settings;
    settings.clipDirectory = directory / L"clips";
    settings.replay.enabled = false;
    settings.replay.diskBackedVideo = false;
    settings.replay.diskBackedAudio = false;
    settings.hotkeys.startStopVirtualKey = 0;
    settings.hotkeys.saveReplayVirtualKey = 0;
    settings.video.fps = 120;
    settings.replay.seconds = 777;
    return sanitizeSettings(settings);
}

void testStartupSnapshotAndFailedCommitRetry() {
    const auto directory = testRoot() / L"retry";
    SettingsStore store(directory / L"settings.ini");
    const auto original = fixture(directory);
    expect(store.save(original), "could not save fixture");
    SettingsStore::LoadStatus status;
    const auto startup = store.load(&status);
    expect(status == SettingsStore::LoadStatus::Loaded, "could not load fixture");
    RecorderController controller;
    expect(controller.initialize(startup), "could not initialize controller");
    const auto before = readBytes(store.path());
    {
        ExclusiveFile lock(store.path());
        MainWindow window(controller, store, startup, status);
        expect(MainWindowSettingsTestAccess::draft(window) == startup &&
               MainWindowSettingsTestAccess::saved(window) == startup,
               "window did not use the successful startup snapshot");
        SettingsStore::LoadStatus secondStatus;
        store.load(&secondStatus);
        expect(secondStatus == SettingsStore::LoadStatus::Failed, "second load fixture was not locked");
        MainWindowSettingsTestAccess::draft(window).video.fps = 45;
        MainWindowSettingsTestAccess::markDirty(window);
        MainWindowSettingsTestAccess::apply(window);
        expect(MainWindowSettingsTestAccess::dirty(window), "failed save cleared dirty state");
        expect(MainWindowSettingsTestAccess::saved(window) == startup &&
               MainWindowSettingsTestAccess::draft(window).video.fps == 45,
               "failed save changed the baseline or lost the draft");
        expect(MainWindowSettingsTestAccess::status(window).find(L"could not be saved") != std::wstring::npos,
               "failed save did not report failure");
    }
    expect(readBytes(store.path()) == before, "failed save changed the settings file");

    MainWindow window(controller, store, startup, status);
    MainWindowSettingsTestAccess::draft(window).video.fps = 45;
    MainWindowSettingsTestAccess::markDirty(window);
    {
        ExclusiveFile lock(store.path());
        MainWindowSettingsTestAccess::apply(window);
    }
    MainWindowSettingsTestAccess::apply(window);
    expect(!MainWindowSettingsTestAccess::dirty(window), "successful retry remained dirty");
    expect(store.load().video.fps == 45 && MainWindowSettingsTestAccess::saved(window).video.fps == 45,
           "successful retry did not publish the draft");
    expect(MainWindowSettingsTestAccess::applyQueued(window), "queued retry was not applied");
    expect(controller.settings().video.fps == 45, "controller did not receive retried settings");
}

void testFailedStartupReadNeverOverwritesFile() {
    const auto directory = testRoot() / L"unreadable";
    SettingsStore store(directory / L"settings.ini");
    expect(store.save(fixture(directory)), "could not save unreadable fixture");
    const auto before = readBytes(store.path());
    SettingsStore::LoadStatus status;
    AppSettings fallback;
    {
        ExclusiveFile lock(store.path());
        fallback = store.load(&status);
    }
    expect(status == SettingsStore::LoadStatus::Failed, "startup read should have failed");
    RecorderController controller;
    MainWindow window(controller, store, fallback, status);
    MainWindowSettingsTestAccess::loggingEnabled(window, false);
    expect(MainWindowSettingsTestAccess::dirty(window), "blocked preference was not kept as a draft");
    expect(MainWindowSettingsTestAccess::status(window).find(L"restart") != std::wstring::npos,
           "unreadable startup did not explain how to recover");
    expect(readBytes(store.path()) == before, "fallback defaults overwrote unreadable settings");
}

void testImmediatePreferencesPreserveDraftsAndSavedValues() {
    const auto directory = testRoot() / L"preferences";
    SettingsStore store(directory / L"settings.ini");
    const auto initial = fixture(directory);
    expect(store.save(initial), "could not save preference fixture");
    RecorderController controller;
    expect(controller.initialize(initial), "could not initialize preference controller");
    MainWindow window(controller, store, initial, SettingsStore::LoadStatus::Loaded);
    {
        ExclusiveFile lock(store.path());
        MainWindowSettingsTestAccess::loggingEnabled(window, false);
        expect(MainWindowSettingsTestAccess::dirty(window), "failed logging save was not dirty");
        expect(MainWindowSettingsTestAccess::status(window).find(L"could not be saved") != std::wstring::npos,
               "failed logging save reported success");
        {
            MainWindow viewWindow(controller, store, initial, SettingsStore::LoadStatus::Loaded);
            MainWindowSettingsTestAccess::gallery(viewWindow, true);
            expect(MainWindowSettingsTestAccess::dirty(viewWindow), "failed view save was not dirty");
            expect(MainWindowSettingsTestAccess::status(viewWindow).find(L"could not be saved") != std::wstring::npos,
                   "failed view save did not report failure");
        }
        MainWindowSettingsTestAccess::gallery(window, true);
        expect(MainWindowSettingsTestAccess::saved(window) == initial,
               "failed immediate save changed the baseline");
        expect(MainWindowSettingsTestAccess::dirty(window), "failed immediate saves were not dirty");
        expect(controller.settings().loggingEnabled == initial.loggingEnabled &&
               !controller.settings().libraryGalleryView &&
               Logger::instance().enabled() == initial.loggingEnabled,
               "failed immediate save changed an applied authority");
    }
    MainWindowSettingsTestAccess::draft(window).video.fps = 45;
    MainWindowSettingsTestAccess::markDirty(window);
    MainWindowSettingsTestAccess::loggingEnabled(window, false);
    auto saved = store.load();
    expect(saved.loggingEnabled == false && !saved.libraryGalleryView && saved.video.fps == 120,
           "logging retry persisted unrelated drafts");
    expect(MainWindowSettingsTestAccess::dirty(window), "logging retry cleared other drafts");
    MainWindowSettingsTestAccess::gallery(window, true);
    saved = store.load();
    expect(saved.loggingEnabled == false && saved.libraryGalleryView && saved.video.fps == 120,
           "view retry lost a committed preference or persisted an unrelated draft");
    expect(MainWindowSettingsTestAccess::draft(window).video.fps == 45 &&
           MainWindowSettingsTestAccess::dirty(window), "immediate saves lost a draft");
    expect(controller.settings().loggingEnabled == false && controller.settings().libraryGalleryView,
           "immediate saves did not update controller preferences");

    auto staleAction = initial;
    staleAction.video.fps = 90;
    expect(controller.updateSettings(staleAction), "stale action could not be applied");
    expect(controller.settings().loggingEnabled == false && controller.settings().libraryGalleryView,
           "stale queued action replaced committed preferences");
    expect(MainWindowSettingsTestAccess::commitDraft(window), "could not commit independent draft");
    controller.setUiPreferences(initial.libraryGalleryView, initial.loggingEnabled);
    MainWindowSettingsTestAccess::switchPages(window);
    expect(MainWindowSettingsTestAccess::draft(window).video.fps == 45 &&
           MainWindowSettingsTestAccess::saved(window).video.fps == 45 &&
           MainWindowSettingsTestAccess::draft(window).loggingEnabled == false &&
           MainWindowSettingsTestAccess::saved(window).libraryGalleryView,
           "page construction replaced the published snapshot with stale controller state");
    MainWindowSettingsTestAccess::loggingEnabled(window, true);
    saved = store.load();
    expect(saved.video.fps == 45 && saved.libraryGalleryView && saved.loggingEnabled == true &&
           controller.settings().libraryGalleryView && controller.settings().loggingEnabled == true,
           "later independent commit lost published settings");
}

void testMissingCheckboxesPreserveSettings() {
    const auto directory = testRoot() / L"missing_controls";
    SettingsStore store(directory / L"settings.ini");
    const auto initial = fixture(directory);
    RecorderController controller;
    MainWindow window(controller, store, initial, SettingsStore::LoadStatus::Missing);
    auto target = initial;
    bool* fields[] = {
        &target.multiMonitorSupport, &target.followFocusedMonitor, &target.followMouseMonitor,
        &target.captureCursor, &target.captureSystemAudio, &target.captureMicrophone,
        &target.audioInputAutoGainEnabled,
#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
        &target.audioInputNoiseSuppressionEnabled,
#endif
        &target.startWithWindowsMinimized, &target.pruneStaleMicrophoneConsentEntries,
        &target.exitToTray, &target.checkForUpdates,
        &target.video.encoderLookahead, &target.video.encoderSpatialAQ, &target.video.encoderTemporalAQ,
        &target.video.encoderBFrames, &target.video.encoderAdaptiveBFrames,
        &target.video.encoderAdaptiveIFrames, &target.video.encoderZeroReorderDelay,
        &target.gpu.allowIdleFrameSkipping, &target.gpu.stableMultimonitorFrames,
        &target.replay.enabled, &target.replay.diskBackedVideo, &target.replay.diskBackedAudio,
        &target.gameIntegrations.leagueOfLegendsKillReminder, &target.autoRecordEnabled,
        &target.soundSeparationEnabled, &target.organizeClipsByGame,
    };
    for (bool value : {true, false}) {
        for (bool* field : fields) {
            *field = value;
        }
        const auto before = target;
        MainWindowSettingsTestAccess::read(window, target);
        expect(target == before, "absent checkbox changed its fallback setting");
    }
}

void testEnumCombosPreserveFallbacks() {
    const auto directory = testRoot() / L"enum_controls";
    SettingsStore store(directory / L"settings.ini");
    const auto initial = fixture(directory);
    RecorderController controller;
    MainWindow window(controller, store, initial, SettingsStore::LoadStatus::Missing);
    auto fallback = initial;
    fallback.video.codec = VideoCodec::Hevc;
    fallback.video.encoderPreset = EncoderPreset::P2;
    fallback.video.encoderMode = EncoderMode::LowLatency;
    fallback.video.encoderProfile = EncoderProfile::Balanced;
    fallback.video.encoderMultipass = EncoderMultipass::QuarterResolution;
    fallback.gpu.adaptiveMode = GpuAdaptiveMode::Conservative;
    fallback.preferredCaptureBackend = CaptureBackend::DesktopDuplication;
    fallback.gameIntegrations.voiceCommand = GameIntegrationSettings::VoiceCommandMode::ClipThat;
    fallback.gameIntegrations.discordRichPresence = GameIntegrationSettings::DiscordRichPresenceMode::FullscreenOnly;
    auto target = fallback;
    MainWindowSettingsTestAccess::read(window, target);
    expect(target == fallback, "absent enum combos changed fallback settings");

    const HWND combo = CreateWindowExW(0, L"COMBOBOX", L"", WS_POPUP | CBS_DROPDOWNLIST,
        0, 0, 100, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    expect(combo != nullptr, "could not create enum combo fixture");
    MainWindowSettingsTestAccess::bindVoice(window, combo);
    MainWindowSettingsTestAccess::enumControls(window, combo);
    // a real selectable row beyond every enum table exercises the bounds check.
    for (int index = 0; index < 8; ++index) {
        expect(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"entry")) == index,
               "could not populate enum combo fixture");
    }
    expect(SendMessageW(combo, CB_SETCURSEL, 0, 0) == 0, "could not select enum combo row");
    auto expected = initial;
    expected.video.codec = VideoCodec::H264;
    expected.video.encoderPreset = EncoderPreset::P1;
    expected.video.encoderMode = EncoderMode::HighQuality;
    expected.video.encoderProfile = EncoderProfile::LowestGpu;
    expected.video.encoderMultipass = EncoderMultipass::Disabled;
    expected.gpu.adaptiveMode = GpuAdaptiveMode::Disabled;
    expected.preferredCaptureBackend = CaptureBackend::WindowsGraphicsCapture;
    expected.gameIntegrations.voiceCommand = GameIntegrationSettings::VoiceCommandMode::Disabled;
    expected.gameIntegrations.discordRichPresence = GameIntegrationSettings::DiscordRichPresenceMode::Always;
    MainWindowSettingsTestAccess::read(window, target);
    expect(target == expected, "selected enum combos did not map through their tables");
    expect(SendMessageW(combo, CB_SETCURSEL, 1, 0) == 1, "could not select second enum row");
    MainWindowSettingsTestAccess::read(window, target);
    expect(target == fallback, "second enum combo selection did not map through its table");
    for (int selected : {-1, 7}) {
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
        expect(SendMessageW(combo, CB_GETCURSEL, 0, 0) == selected,
               "enum fallback fixture selection was not set");
        target = fallback;
        MainWindowSettingsTestAccess::read(window, target);
        expect(target == fallback, "unselected or out-of-range enum combo changed its fallback");
    }
    MainWindowSettingsTestAccess::clearControls(window);
}

struct Checkbox {
    HWND handle = CreateWindowExW(0, L"BUTTON", L"", WS_POPUP | BS_AUTOCHECKBOX,
        0, 0, 100, 24, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Checkbox() { expect(handle != nullptr, "could not create checkbox fixture"); }
    ~Checkbox() { if (IsWindow(handle)) DestroyWindow(handle); }
    void check(bool value) const { SendMessageW(handle, BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0); }
};

void testFollowMouseGateAndPageReset() {
    const auto directory = testRoot() / L"monitor_controls";
    SettingsStore store(directory / L"settings.ini");
    const auto initial = fixture(directory);
    RecorderController controller;
    Checkbox multi, focused, mouse;
    MainWindow window(controller, store, initial, SettingsStore::LoadStatus::Missing);
    MainWindowSettingsTestAccess::monitorControls(window, multi.handle, focused.handle, mouse.handle);
    for (int bits = 0; bits < 8; ++bits) {
        multi.check((bits & 1) != 0);
        focused.check((bits & 2) != 0);
        mouse.check((bits & 4) != 0);
        auto target = initial;
        MainWindowSettingsTestAccess::read(window, target);
        expect(target.multiMonitorSupport == ((bits & 1) != 0) &&
               target.followFocusedMonitor == ((bits & 2) != 0) &&
               target.followMouseMonitor == (bits == 7), "follow-mouse gate or checkbox read changed");
    }
    MainWindowSettingsTestAccess::bindVoice(window, mouse.handle);
    MainWindowSettingsTestAccess::clearControls(window);
    expect(MainWindowSettingsTestAccess::voice(window) == nullptr, "page teardown retained the voice control");
    expect(!IsWindow(mouse.handle), "page teardown did not destroy its control");
    MainWindowSettingsTestAccess::bindVoice(window, focused.handle);
    MainWindowSettingsTestAccess::rebind(window);
    expect(MainWindowSettingsTestAccess::voice(window) == nullptr, "empty-host rebind retained the voice control");
    MainWindowSettingsTestAccess::clearControls(window);
}

void testAutoRecordUsesAppliedSettings() {
    const auto directory = testRoot() / L"auto_record";
    SettingsStore store(directory / L"settings.ini");
    auto applied = fixture(directory);
    applied.autoRecordEnabled = true;
    applied.autoRecordApps = {{L"Applied", L"C:\\Games\\applied.exe"}};
    RecorderController controller;
    expect(controller.initialize(applied), "could not initialize auto-record controller");
    MainWindow window(controller, store, applied, SettingsStore::LoadStatus::Loaded);
    auto& draft = MainWindowSettingsTestAccess::draft(window);
    draft.autoRecordEnabled = false;
    draft.autoRecordApps = {{L"Draft", L"C:\\Games\\draft.exe"}};
    MainWindowSettingsTestAccess::markDirty(window);
    const std::vector<AudioSessionAppInfo> openApplied = {{L"Applied", L"C:\\Games\\applied.exe", 1}};
    const std::vector<AudioSessionAppInfo> openDraft = {{L"Draft", L"C:\\Games\\draft.exe", 2}};
    expect(MainWindowSettingsTestAccess::poll(window, openApplied),
           "unsaved removal or disable changed applied auto-record behavior");
    expect(!MainWindowSettingsTestAccess::poll(window, openDraft),
           "unsaved app was used for auto record");
    draft.autoRecordEnabled = true;
    expect(controller.updateSettings(draft), "could not apply auto-record draft");
    expect(!MainWindowSettingsTestAccess::poll(window, openApplied) &&
           MainWindowSettingsTestAccess::poll(window, openDraft),
           "applied auto-record edits were not used");
}

} // namespace

int main() {
    try {
        std::filesystem::remove_all(testRoot());
        testStartupSnapshotAndFailedCommitRetry();
        testFailedStartupReadNeverOverwritesFile();
        testImmediatePreferencesPreserveDraftsAndSavedValues();
        testAutoRecordUsesAppliedSettings();
        testMissingCheckboxesPreserveSettings();
        testEnumCombosPreserveFallbacks();
        testFollowMouseGateAndPageReset();
        std::filesystem::remove_all(testRoot());
        std::cout << "MainWindowSettingsTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove_all(testRoot(), ignored);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
