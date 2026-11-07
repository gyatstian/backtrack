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
