#include "ui/MainWindow.h"
#include "ui/UiHelpers.h"
#include "ui/UiIds.h"
#include "core/Logger.h"

#include <Windows.h>

#include <algorithm>
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
    static void monitorControls(MainWindow& window, HWND multi, HWND followCombo) {
        window.multiMonitorSupportCheck_ = multi;
        window.monitorFollowCombo_ = followCombo;
    }
    static HWND monitorFollow(const MainWindow& window) { return window.monitorFollowCombo_; }
    static void bindVoice(MainWindow& window, HWND control) {
        window.voiceCommandCombo_ = control;
        window.pageControls_.push_back(control);
    }
    static HWND voice(const MainWindow& window) { return window.voiceCommandCombo_; }
    static HWND overlap(const MainWindow& window) { return window.replayOverlapSecondsEdit_; }
    static void buildCaptureSettings(MainWindow& window, HWND host) {
        window.pageHost_ = host;
        window.buildSettingsCapturePage();
    }
    static void buildCaptureTab(MainWindow& window) { window.buildCapturePage(); }
    static void clearBody(MainWindow& window) { window.clearSettingsBodyControls(); }
    static const std::vector<HWND>& controls(const MainWindow& window) { return window.pageControls_; }
    static std::wstring help(const MainWindow& window, HWND control) {
        const auto found = window.statusHelpTexts_.find(control);
        return found != window.statusHelpTexts_.end() ? found->second : L"";
    }
    static void clearControls(MainWindow& window) { window.clearPageControls(); }
    static void rebind(MainWindow& window) { window.rebindPageControlPointers(); }
    static void switchPages(MainWindow& window) {
        window.clearSettingsDirty();
        window.switchPage(MainWindow::Page::Settings);
        window.switchPage(MainWindow::Page::Capture);
        window.clearPageControls();
    }
    static void bindDeviceCombos(MainWindow& window, HWND output, HWND input) {
        window.outputDeviceCombo_ = output;
        window.inputDeviceCombo_ = input;
    }
    static void loadDeviceCombos(MainWindow& window) { window.loadAudioDeviceCombos(); }
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
        &target.multiMonitorSupport, &target.followMouseMonitor,
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
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"entry"));
    }
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
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
    SendMessageW(combo, CB_SETCURSEL, 1, 0);
    MainWindowSettingsTestAccess::read(window, target);
    expect(target == fallback, "second enum combo selection did not map through its table");
    for (int selected : {-1, 7}) {
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
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

struct Combo {
    HWND handle = CreateWindowExW(0, L"COMBOBOX", L"", WS_POPUP | CBS_DROPDOWNLIST,
        0, 0, 100, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Combo() { expect(handle != nullptr, "could not create combo fixture"); }
    ~Combo() { if (IsWindow(handle)) DestroyWindow(handle); }
};

void testUnavailableAudioDeviceSurvivesUnrelatedSave() {
    const std::vector<AudioDeviceInfo> enumerated = {{L"{mic-a}", L"USB microphone"}, {L"{mic-b}", L"Headset"}};
    const auto choices = deviceChoices(enumerated, L"{mic-gone}");
    expect(choices.size() == 3 && choices.back().id == L"{mic-gone}",
           "an unplugged device was dropped from the combo choices");
    expect(deviceChoices(enumerated, L"").size() == 2, "the default selection gained an entry");
    expect(deviceChoices(enumerated, L"{mic-b}").size() == 2, "a listed device was appended twice");

    const auto directory = testRoot() / L"audio_devices";
    SettingsStore store(directory / L"settings.ini");
    auto initial = fixture(directory);
    initial.audioInputDeviceId = L"{0.0.0.00000000}.{unplugged-mic}";
    initial.audioOutputDeviceId = L"{0.0.0.00000000}.{unplugged-speaker}";
    RecorderController controller;
    MainWindow window(controller, store, initial, SettingsStore::LoadStatus::Missing);
    Combo output, input;
    MainWindowSettingsTestAccess::bindDeviceCombos(window, output.handle, input.handle);
    MainWindowSettingsTestAccess::loadDeviceCombos(window);

    // An unrelated audio edit must not rewrite either pinned id.
    auto target = initial;
    target.audioInputVolumePercent = 42;
    MainWindowSettingsTestAccess::read(window, target);
    expect(target.audioInputDeviceId == initial.audioInputDeviceId &&
           target.audioOutputDeviceId == initial.audioOutputDeviceId,
           "a save that left the device combos alone rewrote the pinned devices");

    SendMessageW(input.handle, CB_SETCURSEL, 0, 0);
    SendMessageW(output.handle, CB_SETCURSEL, 0, 0);
    MainWindowSettingsTestAccess::read(window, target);
    expect(target.audioInputDeviceId.empty() && target.audioOutputDeviceId.empty(),
           "explicitly picking Default did not clear the pinned devices");
}

void testAutoRecordLivenessCheckIgnoresRetainedHandle() {
    expect(isProcessStillRunning(GetCurrentProcessId()), "liveness check reported a running process as gone");

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    std::wstring commandLine = L"cmd.exe /c exit 7";
    expect(CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
               nullptr, nullptr, &startup, &child) != FALSE,
           "could not start the liveness fixture process");
    expect(WaitForSingleObject(child.hProcess, 10000) == WAIT_OBJECT_0,
           "the liveness fixture process did not exit");
    // The handle stays open after exit, the way a launcher or debugger holds one:
    // opening the process still succeeds, so only the exit status proves it ended.
    expect(!isProcessStillRunning(child.dwProcessId),
           "an exited process with a retained handle was reported alive");
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
}

void testFollowMouseGateAndPageReset() {
    const auto directory = testRoot() / L"monitor_controls";
    SettingsStore store(directory / L"settings.ini");
    const auto initial = fixture(directory);
    RecorderController controller;
    Checkbox multi;
    Combo follow;
    fillEnumCombo(follow.handle, kMonitorFollowComboEntries, 0);
    MainWindow window(controller, store, initial, SettingsStore::LoadStatus::Missing);
    MainWindowSettingsTestAccess::monitorControls(window, multi.handle, follow.handle);
    for (int bits = 0; bits < 4; ++bits) {
        const bool multiOn = (bits & 1) != 0;
        const bool followMouse = (bits & 2) != 0;
        multi.check(multiOn);
        SendMessageW(follow.handle, CB_SETCURSEL, followMouse ? 1 : 0, 0);
        auto target = initial;
        MainWindowSettingsTestAccess::read(window, target);
        expect(target.multiMonitorSupport == multiOn &&
               target.followMouseMonitor == (multiOn && followMouse),
               "monitor follow gate or combo read changed");
    }
    MainWindowSettingsTestAccess::bindVoice(window, follow.handle);
    MainWindowSettingsTestAccess::clearControls(window);
    expect(MainWindowSettingsTestAccess::monitorFollow(window) == nullptr,
           "page teardown retained the follow combo");
    expect(!IsWindow(follow.handle), "page teardown did not destroy its control");
    MainWindowSettingsTestAccess::monitorControls(window, multi.handle, multi.handle);
    MainWindowSettingsTestAccess::rebind(window);
    expect(MainWindowSettingsTestAccess::monitorFollow(window) == nullptr,
           "empty-host rebind retained the follow combo");
    MainWindowSettingsTestAccess::clearControls(window);
}

void testClipOverlapSetting() {
    expect(AppSettings{}.replay.overlapSeconds == 3, "clip overlap default changed");
    expect(isSettingsControlId(kReplayOverlapSecondsEditId), "clip overlap does not mark settings dirty");
    for (bool expanded : {false, true}) {
        const auto directory = testRoot() / L"clip_overlap";
        SettingsStore store(directory / L"settings.ini");
        auto initial = fixture(directory);
        initial.multiMonitorSupport = expanded;
        initial.replay.diskBackedVideo = expanded;
        initial.replay.overlapSeconds = 17;
        RecorderController controller;
        MainWindow window(controller, store, initial, SettingsStore::LoadStatus::Missing);
        struct Host {
            HWND handle = CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
                0, 0, 900, 900, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            ~Host() { if (handle) DestroyWindow(handle); }
        } host;
        expect(host.handle != nullptr, "could not create clip overlap page host");
        MainWindowSettingsTestAccess::buildCaptureSettings(window, host.handle);
        const HWND edit = MainWindowSettingsTestAccess::overlap(window);
        expect(edit && GetDlgItem(host.handle, kReplayOverlapSecondsEditId) == edit,
               "Settings Capture did not create clip overlap");
        expect((GetWindowLongPtrW(edit, GWL_STYLE) & (ES_NUMBER | WS_TABSTOP)) == (ES_NUMBER | WS_TABSTOP),
               "clip overlap is not a keyboard-accessible numeric field");
        expect(readText(edit) == L"17", "clip overlap did not display the draft value");
        const auto& controls = MainWindowSettingsTestAccess::controls(window);
        const auto editAt = std::find(controls.begin(), controls.end(), edit);
        expect(editAt != controls.end() && editAt != controls.begin(),
               "clip overlap is not a page control");
        // Last row of the Capture section: the next control built is the
        // Multi-monitor heading.
        expect(editAt + 1 != controls.end() && readText(*(editAt + 1)) == L"Multi-monitor",
               "clip overlap is not the last Capture setting");
        const HWND label = *(editAt - 1);
        expect(readText(label) == L"Clip overlap (seconds)", "clip overlap label changed");
        const std::wstring help = L"When saving again within the replay window, save only new footage plus this much of the previous clip.";
        expect(MainWindowSettingsTestAccess::help(window, edit) == help &&
               MainWindowSettingsTestAccess::help(window, label) == help,
               "clip overlap label/control footer help changed");
        MainWindowSettingsTestAccess::rebind(window);
        expect(MainWindowSettingsTestAccess::overlap(window) == edit, "clip overlap was not rebound");
        MainWindowSettingsTestAccess::markDirty(window);
        expect(!MainWindowSettingsTestAccess::dirty(window), "unchanged clip overlap is dirty");
        for (uint32_t value : {0u, 3600u, 3601u}) {
            SetWindowTextW(edit, std::to_wstring(value).c_str());
            auto target = initial;
            MainWindowSettingsTestAccess::read(window, target);
            expect(target.replay.overlapSeconds == value, "clip overlap reader changed or clamped the value");
            expect(sanitizeSettings(target).replay.overlapSeconds == (value > 3600 ? 3600 : value),
                   "clip overlap did not use shared sanitization");
            MainWindowSettingsTestAccess::markDirty(window);
            expect(MainWindowSettingsTestAccess::dirty(window), "edited clip overlap is not dirty");
        }
        SetWindowTextW(edit, L"17");
        MainWindowSettingsTestAccess::markDirty(window);
        expect(!MainWindowSettingsTestAccess::dirty(window), "restored clip overlap remained dirty");
        MainWindowSettingsTestAccess::clearBody(window);
        expect(!IsWindow(edit) && MainWindowSettingsTestAccess::overlap(window) == nullptr,
               "settings body teardown retained clip overlap");
        auto target = initial;
        MainWindowSettingsTestAccess::read(window, target);
        expect(target.replay.overlapSeconds == 17, "absent clip overlap lost the fallback");
        MainWindowSettingsTestAccess::buildCaptureSettings(window, host.handle);
        MainWindowSettingsTestAccess::clearControls(window);
        expect(MainWindowSettingsTestAccess::overlap(window) == nullptr, "page teardown retained clip overlap");
        MainWindowSettingsTestAccess::buildCaptureTab(window);
        expect(!GetDlgItem(host.handle, kReplayOverlapSecondsEditId) &&
               MainWindowSettingsTestAccess::overlap(window) == nullptr,
               "clip overlap appeared on the Capture tab");
        MainWindowSettingsTestAccess::clearControls(window);
    }
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

void testAutoRecordOwnershipReconciliation() {
    const AutoRecordOwnership idle{};
    const AutoRecordOwnership owned{true, false};

    // An automatic start takes ownership; a failed one claims nothing so the
    // next poll retries.
    AutoRecordOwnership state = reconcileAutoRecordOwnership(idle, true, true, false, true);
    expect(state.owns && !state.suppressStart, "an automatic start did not take ownership");
    const auto failed = reconcileAutoRecordOwnership(idle, true, false, false, true);
    expect(!failed.owns && !failed.suppressStart, "a failed automatic start claimed ownership");

    // (a) a restart-class settings change stops the auto-owned recording and
    // ApplySettings completes: no suppression, so the next poll restarts it.
    state = reconcileAutoRecordAfterSettings(state, false);
    expect(!state.owns && !state.suppressStart,
           "a settings restart suppressed the next auto start");
    expect(reconcileAutoRecordAfterSettings(owned, true).owns,
           "settings that kept the recording running dropped ownership");

    // (c) an automatic stop is not a user override.
    state = reconcileAutoRecordOwnership(owned, true, false, true, true);
    expect(!state.owns && !state.suppressStart,
           "an automatic stop was treated as a user override");

    // (d) a manual stop blocks the restart only while the app stays open.
    state = reconcileAutoRecordOwnership(owned, false, false, true, true);
    expect(!state.owns && state.suppressStart,
           "a manual stop did not suppress the auto restart");
    const auto manualStopAfterExit = reconcileAutoRecordOwnership(owned, false, false, true, false);
    expect(!manualStopAfterExit.owns && !manualStopAfterExit.suppressStart,
           "a manual stop after the app exited suppressed the next launch");

    // (b) a manual start between polls never claims ownership and lifts the
    // suppression, so a later target exit leaves the user's recording alone.
    state = reconcileAutoRecordOwnership(state, false, true, false, true);
    expect(!state.owns && !state.suppressStart, "a manual start claimed auto ownership");
}

} // namespace

int main() {
    try {
        std::filesystem::remove_all(testRoot());
        testStartupSnapshotAndFailedCommitRetry();
        testFailedStartupReadNeverOverwritesFile();
        testImmediatePreferencesPreserveDraftsAndSavedValues();
        testAutoRecordUsesAppliedSettings();
        testAutoRecordOwnershipReconciliation();
        testMissingCheckboxesPreserveSettings();
        testEnumCombosPreserveFallbacks();
        testUnavailableAudioDeviceSurvivesUnrelatedSave();
        testAutoRecordLivenessCheckIgnoresRetainedHandle();
        testFollowMouseGateAndPageReset();
        testClipOverlapSetting();
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
