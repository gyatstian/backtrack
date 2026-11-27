#include "settings/SettingsStore.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace backtrack {

std::filesystem::path defaultClipDirectory() {
    return L"C:\\backtrack_test_default_clip_directory";
}

} // namespace backtrack

namespace {

using backtrack::AppSettings;
using backtrack::SettingsStore;

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path testRoot() {
    return std::filesystem::temp_directory_path() /
        (L"backtrack_settings_store_test_" + std::to_wstring(GetCurrentProcessId()));
}

std::vector<char> readBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    expect(file.is_open(), "could not read settings fixture");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void writeBytes(const std::filesystem::path& path, const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    expect(static_cast<bool>(file), "could not write settings fixture");
}

std::wstring stagingName(unsigned long long offset) {
    return L"BT" + std::to_wstring((GetCurrentProcessId() + offset) % 1000000) + L".TMP";
}

void expectNoStaging(const std::filesystem::path& directory, const std::filesystem::path& allowed = {}) {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path() == allowed) continue;
        const auto name = entry.path().filename().wstring();
        expect(!(name.starts_with(L"BT") && name.ends_with(L".TMP")), "settings staging file was left behind");
    }
}

bool createSymlink(const std::filesystem::path& link, const std::filesystem::path& target) {
    if (CreateSymbolicLinkW(link.c_str(), target.c_str(), 0x2)) return true;
    DWORD error = GetLastError();
    if (error == ERROR_INVALID_PARAMETER) {
        if (CreateSymbolicLinkW(link.c_str(), target.c_str(), 0)) return true;
        error = GetLastError();
    }
    if (error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION) {
        std::cout << "settings symbolic link tests skipped: unavailable (" << error << ")\n";
        return false;
    }
    throw std::runtime_error("could not create settings symbolic link fixture");
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

class ReadRangeLock {
public:
    explicit ReadRangeLock(const std::filesystem::path& path)
        : handle_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr)) {
        expect(handle_ != INVALID_HANDLE_VALUE, "could not open range-lock fixture");
        range_.Offset = 4096;
        if (!LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                        0, 4096, 0, &range_)) {
            CloseHandle(handle_);
            throw std::runtime_error("could not lock settings read range");
        }
    }
    ~ReadRangeLock() {
        UnlockFileEx(handle_, 0, 4096, 0, &range_);
        CloseHandle(handle_);
    }
    ReadRangeLock(const ReadRangeLock&) = delete;
    ReadRangeLock& operator=(const ReadRangeLock&) = delete;

private:
    HANDLE handle_;
    OVERLAPPED range_{};
};

void testHardLinkedSettingsAndStaging() {
    const auto directory = testRoot() / L"hardlinks";
    const auto path = directory / L"settings.ini";
    const auto legacyTemp = directory / L"settings.ini.tmp";
    const auto collision = directory / stagingName(0);
    const auto sentinel = testRoot() / L"hardlink-sentinel.txt";
    const std::string original("outside\r\n\0bytes", 15);
    writeBytes(sentinel, original);
    std::filesystem::create_directories(directory);
    for (const auto& link : {path, legacyTemp, collision}) {
        expect(CreateHardLinkW(link.c_str(), sentinel.c_str(), nullptr) != FALSE,
               "could not hard-link settings fixture");
    }

    AppSettings settings;
    settings.video.fps = 0;
    const SettingsStore store(path);
    expect(store.save(settings), "could not save over hard-linked settings");
    const std::vector<char> expected(original.begin(), original.end());
    expect(readBytes(sentinel) == expected && readBytes(legacyTemp) == expected && readBytes(collision) == expected,
           "settings or staging write changed the external hard-link target");
    expect(!std::filesystem::equivalent(path, sentinel), "settings replacement retained the hard link");
    expect(store.load().video.fps == 1, "hard-link save lost settings normalization");
    expectNoStaging(directory, collision);
}

void testStagingShapedDestination() {
    const auto directory = testRoot() / L"staging-name";
    // The hard-link collision and its successful retry consumed names 0 and 1.
    const auto path = directory / stagingName(2);
    AppSettings settings;
    settings.video.fps = 61;
    const SettingsStore store(path);
    expect(store.save(settings), "could not save to a staging-shaped destination");
    expect(store.load().video.fps == 61, "staging-shaped destination did not round trip");
    expectNoStaging(directory, path);
}

void testSymbolicLinkedSettingsAndStaging() {
    const auto directory = testRoot() / L"symlinks";
    const auto path = directory / L"settings.ini";
    const auto legacyTemp = directory / L"settings.ini.tmp";
    // The staging-shaped destination consumed names 2 and 3.
    const auto collision = directory / stagingName(4);
    const auto sentinel = testRoot() / L"symlink-sentinel.txt";
    const std::string original("symbolic\r\n\0bytes", 16);
    writeBytes(sentinel, original);
    std::filesystem::create_directories(directory);
    if (!createSymlink(collision, sentinel)) return;
    expect(createSymlink(legacyTemp, sentinel), "could not link legacy settings staging");
    expect(createSymlink(path, sentinel), "could not link settings destination");

    AppSettings settings;
    settings.video.fps = 177;
    const SettingsStore store(path);
    expect(store.save(settings), "could not save over symbolic-linked settings");
    const std::vector<char> expected(original.begin(), original.end());
    expect(readBytes(sentinel) == expected && readBytes(legacyTemp) == expected && readBytes(collision) == expected,
           "settings or staging write changed the external symbolic-link target");
    expect(!std::filesystem::equivalent(path, sentinel), "settings replacement retained the symbolic link");
    SettingsStore::LoadStatus status;
    const auto loaded = store.load(&status);
    expect(status == SettingsStore::LoadStatus::Loaded && loaded.video.fps == 177,
           "symbolic-link replacement did not round trip");
    expect(store.normalizeLoaded(loaded, status) && store.load() == loaded,
           "normalizing replaced settings changed the snapshot");
    expect(readBytes(sentinel) == expected && readBytes(legacyTemp) == expected && readBytes(collision) == expected,
           "normalization changed a symbolic-link target");
    expectNoStaging(directory, collision);
}

void testLoadStatusesAndLockedStartup() {
    const auto path = testRoot() / L"startup" / L"settings.ini";
    const SettingsStore store(path);
    SettingsStore::LoadStatus status = SettingsStore::LoadStatus::Failed;
    const auto missing = store.load(&status);
    expect(status == SettingsStore::LoadStatus::Missing, "missing file was not distinguished");
    expect(store.normalizeLoaded(missing, status), "missing settings normalization failed");
    expect(!std::filesystem::exists(path), "missing settings were created during normalization");

    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream empty(path, std::ios::binary);
        expect(empty.is_open(), "could not create empty settings fixture");
    }
    store.load(&status);
    expect(status == SettingsStore::LoadStatus::Loaded, "empty file was not loaded");

    const auto directory = testRoot() / L"directory.ini";
    std::filesystem::create_directories(directory);
    SettingsStore(directory).load(&status);
    expect(status == SettingsStore::LoadStatus::Failed, "directory was not a failed read");

    AppSettings original;
    original.video.fps = 123;
    original.replay.seconds = 777;
    expect(store.save(original), "could not write startup settings fixture");
    const auto before = readBytes(path);
    AppSettings failedSnapshot;
    {
        ExclusiveFile lock(path);
        failedSnapshot = store.load(&status);
        expect(status == SettingsStore::LoadStatus::Failed, "locked read was not a failure");
    }

    expect(store.normalizeLoaded(failedSnapshot, status), "failed read was not skipped");
    expect(readBytes(path) == before, "failed read overwrote the original settings");
    const auto recovered = store.load(&status);
    expect(status == SettingsStore::LoadStatus::Loaded && recovered.video.fps == 123 &&
           recovered.replay.seconds == 777, "settings did not recover after unlock");
    expect(store.normalizeLoaded(recovered, status), "could not normalize recovered settings");
    expect(store.load() == recovered, "normalization changed recovered settings");
}

void testReadErrorDiscardsPartialSettings() {
    const auto path = testRoot() / L"partial" / L"settings.ini";
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream file(path, std::ios::binary);
        expect(file.is_open(), "could not create partial-read fixture");
        file << "video.fps=123\n#" << std::string(8192, 'x') << "\nreplay.seconds=777\n";
        expect(static_cast<bool>(file), "could not write partial-read fixture");
    }
    const auto before = readBytes(path);
    const SettingsStore store(path);
    SettingsStore::LoadStatus status;
    AppSettings failedSnapshot;
    {
        ReadRangeLock lock(path);
        failedSnapshot = store.load(&status);
    }
    expect(status == SettingsStore::LoadStatus::Failed, "partial read was reported as loaded");
    expect(failedSnapshot.video.fps == AppSettings{}.video.fps,
           "partial read returned values from an incomplete file");
    expect(store.normalizeLoaded(failedSnapshot, status), "partial-read normalization was not skipped");
    expect(readBytes(path) == before, "partial read damaged the original settings");
    const auto recovered = store.load(&status);
    expect(status == SettingsStore::LoadStatus::Loaded && recovered.video.fps == 123 &&
           recovered.replay.seconds == 777, "partial-read fixture did not recover after unlock");
}

void testChunkedParsing() {
    const auto path = testRoot() / L"chunked" / L"settings.ini";
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream file(path, std::ios::binary);
        expect(file.is_open(), "could not create chunked settings fixture");
        file << '#' << std::string(4094, 'x') << "\r\nvideo.fps=143\r\nreplay.seconds=333";
        expect(static_cast<bool>(file), "could not write chunked settings fixture");
    }
    SettingsStore::LoadStatus status;
    const auto settings = SettingsStore(path).load(&status);
    expect(status == SettingsStore::LoadStatus::Loaded && settings.video.fps == 143 &&
           settings.replay.seconds == 333, "chunk boundary or final line broke parsing");
}

void testFailedSaveCanRetry() {
    const auto path = testRoot() / L"save" / L"settings.ini";
    const SettingsStore store(path);
    AppSettings original;
    original.video.fps = 120;
    expect(store.save(original), "could not write save fixture");
    const auto before = readBytes(path);
    SettingsStore::LoadStatus status;
    const auto loaded = store.load(&status);
    expect(status == SettingsStore::LoadStatus::Loaded, "could not load save fixture");

    auto edited = original;
    edited.video.fps = 45;
    {
        ExclusiveFile lock(path);
        expect(!store.normalizeLoaded(loaded, status), "locked normalization reported success");
        expect(!store.save(edited), "locked save reported success");
    }
    expect(readBytes(path) == before, "failed save changed the previous settings");
    expect(!std::filesystem::exists(path.wstring() + L".tmp"), "failed save left a temp file");
    expectNoStaging(path.parent_path());
    expect(store.save(edited), "unlocked save could not be retried");
    expect(store.load().video.fps == 45, "retried save did not persist the edit");
}

void testReplayFallbackIsDistinctAndIdempotent() {
    const backtrack::HotkeySettings defaults;
    for (const uint32_t replayKey : {defaults.saveReplayVirtualKey, 0x100u}) {
        AppSettings settings;
        settings.hotkeys.startStopModifiers = defaults.saveReplayModifiers;
        settings.hotkeys.startStopVirtualKey = defaults.saveReplayVirtualKey;
        settings.hotkeys.saveReplayModifiers = defaults.saveReplayModifiers;
        settings.hotkeys.saveReplayVirtualKey = replayKey;

        const auto normalized = backtrack::sanitizeSettings(settings);
        expect(normalized.hotkeys.startStopModifiers == defaults.saveReplayModifiers &&
               normalized.hotkeys.startStopVirtualKey == defaults.saveReplayVirtualKey,
               "replay fallback changed the start/stop chord");
        expect(normalized.hotkeys.saveReplayVirtualKey == 0 ||
               normalized.hotkeys.saveReplayModifiers != normalized.hotkeys.startStopModifiers ||
               normalized.hotkeys.saveReplayVirtualKey != normalized.hotkeys.startStopVirtualKey,
               "replay fallback duplicates the start/stop chord");
        expect(backtrack::isSafeGlobalHotkey(normalized.hotkeys.saveReplayModifiers,
                                            normalized.hotkeys.saveReplayVirtualKey),
               "replay fallback is unsafe");
        expect(backtrack::sanitizeSettings(normalized) == normalized,
               "replay fallback is not idempotent");
    }

    AppSettings duplicate;
    duplicate.hotkeys.startStopModifiers = MOD_CONTROL;
    duplicate.hotkeys.startStopVirtualKey = VK_F1;
    duplicate.hotkeys.saveReplayModifiers = MOD_CONTROL;
    duplicate.hotkeys.saveReplayVirtualKey = VK_F1;
    const auto normalized = backtrack::sanitizeSettings(duplicate);
    expect(normalized.hotkeys.saveReplayModifiers == defaults.saveReplayModifiers &&
           normalized.hotkeys.saveReplayVirtualKey == defaults.saveReplayVirtualKey,
           "available replay default was not retained as fallback");
    expect(backtrack::sanitizeSettings(normalized) == normalized,
           "distinct replay fallback is not idempotent");
}

void testAppNamesCannotInjectSettings() {
    const SettingsStore store(testRoot() / L"names" / L"settings.ini");
    AppSettings settings;
    settings.video.fps = 141;
    settings.replay.seconds = 777;
    settings.captureSystemAudio = false;
    settings.soundSeparationApps = {
        {L"Alpha\r\nvideo.fps=17", L"C:\\Apps\\alpha.exe", true},
        {L"\r\n", L"C:\\Apps\\blank.exe", false},
    };
    settings.autoRecordApps = {
        {L"Beta\rCompany\nreplay.seconds=12\naudio.system=1", L"C:\\Apps\\beta.exe"},
    };

    const auto normalized = backtrack::sanitizeSettings(settings);
    expect(normalized.soundSeparationApps[0].name == L"Alpha  video.fps=17" &&
           normalized.soundSeparationApps[1].name == L"blank" &&
           normalized.autoRecordApps[0].name == L"Beta Company replay.seconds=12 audio.system=1",
           "app names were not normalized to one line");
    expect(backtrack::sanitizeSettings(normalized) == normalized,
           "app-name normalization is not idempotent");
    expect(store.save(settings), "could not save multiline app names");
    const auto loaded = store.load();
    expect(loaded == normalized, "app names injected or changed unrelated settings");
}

void testAppListsSurviveMissingCount() {
    const auto path = testRoot() / L"applist-count" / L"settings.ini";
    writeBytes(path,
        "integrations.autoRecord.enabled=1\n"
        // No count key: the entries still have to survive, or normalizeLoaded
        // publishes an empty list and the user's apps are gone.
        "integrations.autoRecord.0.name=Alpha\n"
        "integrations.autoRecord.0.path=C:\\Apps\\alpha.exe\n"
        "integrations.autoRecord.1.name=Beta\n"
        "integrations.autoRecord.1.path=C:\\Apps\\beta.exe\n"
        // A count shorter than the list must not hide the entries past it.
        "audio.soundSeparation.count=1\n"
        "audio.soundSeparation.0.name=Gamma\n"
        "audio.soundSeparation.0.path=C:\\Apps\\gamma.exe\n"
        "audio.soundSeparation.0.muted=0\n"
        "audio.soundSeparation.1.name=Delta\n"
        "audio.soundSeparation.1.path=C:\\Apps\\delta.exe\n");

    SettingsStore::LoadStatus status;
    const auto settings = SettingsStore(path).load(&status);
    expect(status == SettingsStore::LoadStatus::Loaded, "the app-list fixture did not load");
    expect(settings.autoRecordApps.size() == 2 &&
           settings.autoRecordApps[1].executablePath.wstring() == L"C:\\Apps\\beta.exe",
        "entries without a count key were dropped");
    expect(settings.soundSeparationApps.size() == 2 &&
           settings.soundSeparationApps[1].executablePath.wstring() == L"C:\\Apps\\delta.exe" &&
           !settings.soundSeparationApps[0].muted && settings.soundSeparationApps[1].muted,
        "entries past the count hint or the default muted flag were lost");
    const SettingsStore store(path);
    expect(store.normalizeLoaded(settings, status) && store.load() == settings,
        "normalization lost app-list entries or muted flags");
}

void testAppListCountBoundsAndGaps() {
    const auto path = testRoot() / L"applist-bounds" / L"settings.ini";
    const SettingsStore store(path);
    for (const std::string hint : {"", "0", "1", "4294967295", "invalid"}) {
        std::string text;
        for (const std::string key : {"audio.soundSeparation", "integrations.autoRecord"}) {
            if (!hint.empty()) text += key + ".count=" + hint + "\n";
            for (unsigned index = 0; index < 130; ++index) {
                text += key + "." + std::to_string(index) + ".path=C:\\Apps\\app" +
                    std::to_string(index) + ".exe\n";
            }
        }
        writeBytes(path, text);
        const auto loaded = store.load();
        expect(loaded.soundSeparationApps.size() == 128 && loaded.autoRecordApps.size() == 128,
            "an app-list count hint changed the 128-entry cap");
        expect(loaded.soundSeparationApps.back().muted &&
               loaded.autoRecordApps.back().executablePath == L"C:\\Apps\\app127.exe",
            "app-list loading changed entry order or the muted default");
    }

    const std::string entries =
        "audio.soundSeparation.1.path=C:\\Apps\\sound.exe\n"
        "audio.soundSeparation.1.muted=invalid\n"
        "integrations.autoRecord.1.path=C:\\Apps\\record.exe\n";
    writeBytes(path, entries);
    auto loaded = store.load();
    expect(loaded.soundSeparationApps.empty() && loaded.autoRecordApps.empty(),
        "a missing path beyond the count hint did not stop either app list");
    writeBytes(path, "audio.soundSeparation.count=2\nintegrations.autoRecord.count=2\n" + entries);
    loaded = store.load();
    expect(loaded.soundSeparationApps.size() == 1 && loaded.soundSeparationApps[0].muted &&
           loaded.autoRecordApps.size() == 1, "a gap inside the count hint hid later entries");
}

void testReplayOverlapPersistence() {
    expect(AppSettings{}.replay.overlapSeconds == 3, "replay overlap default changed");
    const auto path = testRoot() / L"overlap" / L"settings.ini";
    const SettingsStore store(path);
    expect(store.load().replay.overlapSeconds == 3, "missing file lost the overlap default");
    writeBytes(path, "replay.seconds=1\n");
    expect(store.load().replay.overlapSeconds == 3, "missing key lost the overlap default");

    for (const char* malformed : {"", "invalid", "-1", "+5", "1.5", "5seconds", "4294967296"}) {
        writeBytes(path, std::string("replay.overlapSeconds=") + malformed + "\n");
        expect(store.load().replay.overlapSeconds == 3, "malformed overlap did not use the default");
    }
    const struct {
        uint32_t input;
        uint32_t expected;
    } cases[] = {{0, 0}, {3, 3}, {17, 17}, {3600, 3600}, {3601, 3600}, {0xFFFFFFFFu, 3600}};
    for (const auto& entry : cases) {
        writeBytes(path, "replay.seconds=1\nreplay.overlapSeconds=" + std::to_string(entry.input) + "\n");
        expect(store.load().replay.overlapSeconds == entry.expected,
            "loaded overlap was not clamped independently of replay duration");
        AppSettings settings;
        settings.replay.seconds = 1;
        settings.replay.overlapSeconds = entry.input;
        const auto normalized = backtrack::sanitizeSettings(settings);
        expect(normalized.replay.overlapSeconds == entry.expected && normalized.replay.seconds == 1,
            "overlap sanitization changed the independent replay duration");
        expect(store.save(settings), "could not save replay overlap");
        const auto bytes = readBytes(path);
        expect(std::string(bytes.begin(), bytes.end()).find(
            "replay.overlapSeconds=" + std::to_string(entry.expected) + "\n") != std::string::npos,
            "overlap was not persisted with the normalized value");
        expect(store.load().replay.overlapSeconds == entry.expected, "replay overlap did not round trip");
    }
}

void testResolutionPersistence() {
    using backtrack::ResolutionMode;
    const struct {
        ResolutionMode mode;
        const char* name;
        uint32_t width;
        uint32_t height;
    } cases[] = {
        {ResolutionMode::P240, "240p", 426, 240},
        {ResolutionMode::P480, "480p", 854, 480},
        {ResolutionMode::P720, "720p", 1280, 720},
        {ResolutionMode::P1080, "1080p", 1920, 1080},
        {ResolutionMode::P2K, "2k", 2560, 1440},
        {ResolutionMode::P4K, "4k", 3840, 2160},
        {ResolutionMode::Custom, "custom", 1000, 800},
    };
    const auto path = testRoot() / L"resolution" / L"settings.ini";
    const SettingsStore store(path);
    for (const auto& entry : cases) {
        AppSettings settings;
        settings.video.resolutionMode = entry.mode;
        settings.video.width = 1000;
        settings.video.height = 800;
        expect(store.save(settings), "could not save a resolution preset");
        const auto bytes = readBytes(path);
        expect(std::string(bytes.begin(), bytes.end()).find(
            std::string("video.resolutionMode=") + entry.name + "\n") != std::string::npos,
            "persisted resolution spelling changed");
        const auto loaded = store.load();
        expect(loaded.video.resolutionMode == entry.mode && loaded.video.width == entry.width &&
               loaded.video.height == entry.height, "resolution preset did not round trip");
    }
    for (const auto& entry : cases) {
        writeBytes(path, std::string("video.resolutionMode=") + entry.name +
            "\nvideo.width=1000\nvideo.height=800\n");
        const auto loaded = store.load();
        expect(loaded.video.resolutionMode == entry.mode && loaded.video.width == entry.width &&
               loaded.video.height == entry.height, "persisted resolution name changed its size");
    }
    const struct {
        const char* name;
        ResolutionMode mode;
        uint32_t width;
        uint32_t height;
    } aliases[] = {
        {"1440P", ResolutionMode::P2K, 2560, 1440},
        {"2160p", ResolutionMode::P4K, 3840, 2160},
        {"NaTiVe", ResolutionMode::Custom, 1000, 800},
        {"CuStOm", ResolutionMode::Custom, 1000, 800},
        {"unknown", ResolutionMode::P1080, 1920, 1080},
    };
    for (const auto& entry : aliases) {
        writeBytes(path, std::string("video.resolutionMode=") + entry.name +
            "\nvideo.width=1000\nvideo.height=800\n");
        const auto loaded = store.load();
        expect(loaded.video.resolutionMode == entry.mode && loaded.video.width == entry.width &&
               loaded.video.height == entry.height, "legacy or unknown resolution fallback changed");
    }
}

void testBomAndOverlongLine() {
    const auto bomPath = testRoot() / L"bom" / L"settings.ini";
    writeBytes(bomPath, std::string("\xEF\xBB\xBF") + "video.fps=143\nreplay.seconds=333\n");
    SettingsStore::LoadStatus status;
    auto settings = SettingsStore(bomPath).load(&status);
    expect(status == SettingsStore::LoadStatus::Loaded, "the BOM fixture did not load");
    expect(settings.video.fps == 143,
        "a UTF-8 BOM made the first key unreachable");

    const auto longPath = testRoot() / L"overlong" / L"settings.ini";
    // The oversized line is a complete assignment once trimmed, so parsing it
    // would set replay.seconds; dropping it must not swallow the next key.
    writeBytes(longPath,
        "replay.seconds=333" + std::string(70 * 1024, ' ') + "\n" +
        "video.fps=143\n");
    settings = SettingsStore(longPath).load(&status);
    expect(status == SettingsStore::LoadStatus::Loaded, "the over-long-line fixture did not load");
    expect(settings.replay.seconds == AppSettings{}.replay.seconds,
        "an over-long settings line was not dropped");
    expect(settings.video.fps == 143,
        "an over-long settings line swallowed the key after it");
}

void testHotkeySafety() {
    using backtrack::isSafeGlobalHotkey;
    expect(!isSafeGlobalHotkey(MOD_ALT, VK_F4), "Alt+F4 must be rejected as a global hotkey");
    expect(!isSafeGlobalHotkey(MOD_ALT | MOD_CONTROL | MOD_SHIFT, VK_F4),
        "Alt+F4 with extra modifiers must be rejected");
    expect(isSafeGlobalHotkey(MOD_CONTROL, VK_F4), "Ctrl+F4 is a valid hotkey");
    expect(isSafeGlobalHotkey(0, VK_F4), "a bare function key is a valid hotkey");
    expect(isSafeGlobalHotkey(0, 0), "an unbound hotkey is valid");

    AppSettings settings;
    settings.hotkeys.startStopModifiers = MOD_ALT;
    settings.hotkeys.startStopVirtualKey = VK_F4;
    const AppSettings defaults;
    const auto normalized = backtrack::sanitizeSettings(settings);
    expect(normalized.hotkeys.startStopVirtualKey == defaults.hotkeys.startStopVirtualKey &&
           normalized.hotkeys.startStopModifiers == defaults.hotkeys.startStopModifiers,
        "an Alt+F4 start/stop binding was not reset to the default");
}

void testRelativeFolderFallbacks() {
    const auto fallback = backtrack::defaultClipDirectory();
    AppSettings settings;
    settings.clipDirectory = L"relative\\clips";
    settings.replayRingDirectory = L"relative\\ring";
    auto normalized = backtrack::sanitizeSettings(settings);
    expect(normalized.clipDirectory == fallback, "a relative clip folder was kept");
    expect(normalized.replayRingDirectory.empty(), "a relative buffer folder was kept");

    settings.clipDirectory = fallback;
    settings.replayRingDirectory = L"ring";
    normalized = backtrack::sanitizeSettings(settings);
    expect(normalized.clipDirectory == fallback && normalized.replayRingDirectory.empty(),
        "a relative buffer folder was not replaced by the clip folder default");

    settings.replayRingDirectory = L"D:\\Ring";
    normalized = backtrack::sanitizeSettings(settings);
    expect(normalized.replayRingDirectory == L"D:\\Ring", "an absolute buffer folder was replaced");
}

void testWindowPlacementPersistence() {
    const auto path = testRoot() / L"placement" / L"settings.ini";
    const SettingsStore store(path);
    expect(AppSettings{}.windowWidth == 0, "the default window size no longer means unset");

    AppSettings settings;
    settings.windowLeft = -1200;
    settings.windowTop = 40;
    settings.windowWidth = 1280;
    settings.windowHeight = 900;
    settings.windowMaximized = true;
    expect(store.save(settings), "could not save a window placement");
    const auto loaded = store.load();
    expect(loaded.windowLeft == -1200 && loaded.windowTop == 40 && loaded.windowWidth == 1280 &&
           loaded.windowHeight == 900 && loaded.windowMaximized,
        "the window placement did not round trip");

    AppSettings degenerate;
    degenerate.windowLeft = 5;
    degenerate.windowWidth = 10;
    degenerate.windowHeight = 10;
    auto normalized = backtrack::sanitizeSettings(degenerate);
    expect(normalized.windowWidth == 0 && normalized.windowHeight == 0,
        "a degenerate window size was kept as a placement");

    writeBytes(path, "ui.windowLeft=-8\nui.windowTop=7\nui.windowWidth=99999\nui.windowHeight=400\n"
                     "ui.windowMaximized=1\n");
    const auto capped = store.load();
    expect(capped.windowLeft == -8 && capped.windowTop == 7 && capped.windowWidth == 20000 &&
           capped.windowHeight == 400 && capped.windowMaximized,
        "a stored window size was not capped");

    writeBytes(path, "ui.windowWidth=bad\nui.windowHeight=-400\n");
    const auto malformed = store.load();
    expect(malformed.windowWidth == 0 && malformed.windowHeight == 0,
        "a malformed window size was not treated as unset");

    AppSettings zoomed;
    zoomed.uiZoomPercent = 175;
    zoomed.uiScaleWithWindow = true;
    expect(store.save(zoomed), "could not save the UI zoom");
    const auto reloaded = store.load();
    expect(reloaded.uiZoomPercent == 175 && reloaded.uiScaleWithWindow, "the UI zoom did not round trip");
    writeBytes(path, "ui.zoomPercent=5\n");
    expect(store.load().uiZoomPercent == 60, "an out-of-range UI zoom was not clamped");

    AppSettings columns;
    columns.libraryColumns = 7;
    expect(store.save(columns), "could not save the gallery columns");
    expect(store.load().libraryColumns == 7, "the gallery columns did not round trip");
    columns.libraryFitThumbnails = false;
    expect(store.save(columns), "could not save the thumbnail fit");
    expect(!store.load().libraryFitThumbnails, "the thumbnail fit did not round trip");
    columns.libraryOwnScale = true;
    columns.libraryZoomPercent = 180;
    columns.libraryShowHiddenFolders = true;
    expect(store.save(columns), "could not save the library scale");
    const auto scaled = store.load();
    expect(scaled.libraryOwnScale && scaled.libraryZoomPercent == 180, "the library scale did not round trip");
    expect(scaled.libraryShowHiddenFolders, "library.showHiddenFolders did not round trip");
    expect(!AppSettings{}.libraryShowHiddenFolders, "libraryShowHiddenFolders should default off");
    writeBytes(path, "library.zoomPercent=3\n");
    expect(store.load().libraryZoomPercent == 60, "an out-of-range library zoom was not clamped");
    writeBytes(path, "library.columns=4294967295\n");
    expect(store.load().libraryColumns == 4294967295u, "the gallery columns must stay unbounded");
    writeBytes(path, "library.columns=two\n");
    expect(store.load().libraryColumns == 0, "a malformed gallery column count should mean automatic");
}

} // namespace

int main() {
    try {
        std::filesystem::remove_all(testRoot());
        testHardLinkedSettingsAndStaging();
        testStagingShapedDestination();
        testSymbolicLinkedSettingsAndStaging();
        testLoadStatusesAndLockedStartup();
        testReadErrorDiscardsPartialSettings();
        testChunkedParsing();
        testFailedSaveCanRetry();
        testReplayFallbackIsDistinctAndIdempotent();
        testAppNamesCannotInjectSettings();
        testAppListsSurviveMissingCount();
        testBomAndOverlongLine();
        testAppListCountBoundsAndGaps();
        testResolutionPersistence();
        testReplayOverlapPersistence();
        testHotkeySafety();
        testRelativeFolderFallbacks();
        testWindowPlacementPersistence();
        std::filesystem::remove_all(testRoot());
        std::cout << "SettingsStoreTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove_all(testRoot(), ignored);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
