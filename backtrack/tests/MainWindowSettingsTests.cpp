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
