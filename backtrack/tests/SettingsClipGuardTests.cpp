#include "clips/ClipManager.h"
#include "core/Logger.h"
#include "platform/Win32Util.h"
#include "settings/SettingsStore.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Link-time stub for the one platform helper SettingsStore.cpp needs.
// platform/Win32Util.cpp stays out of the build (it would drag DXGI, avrt and
// winmm along for a value this suite asserts on), and a fixed literal beats the
// real %USERPROFILE%\Videos\Backtrack for a deterministic expectation. Same
// pattern as ReplayBufferTests' Mp4Muxer::muxToMp4 stub.
namespace backtrack {

std::filesystem::path defaultClipDirectory() {
    return L"C:\\backtrack_test_default_clip_directory";
}

} // namespace backtrack

namespace {

using backtrack::AppSettings;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path testRoot() {
    return std::filesystem::temp_directory_path() / L"backtrack_settings_clip_guard_test";
}

void writeFile(const std::filesystem::path& path, const char* contents = "not a real mp4") {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    expect(stream.is_open(), "could not create the test file");
    stream << contents;
}

std::vector<char> readFileBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    expect(stream.is_open(), "could not read the test file back");
    return std::vector<char>((std::istreambuf_iterator<char>(stream)),
                             std::istreambuf_iterator<char>());
}

// --- SettingsStore::load coverage ---
// The parse path is the other half of the trust boundary: it is what stands
// between a hand-edited file and a broken encoder config, and nothing ran it
// before. Every case writes a settings.ini to its own folder and loads it
// through the public store.

std::filesystem::path settingsPath(const wchar_t* name) {
    return testRoot() / L"settings" / name / L"settings.ini";
}

backtrack::AppSettings loadFrom(const std::filesystem::path& path, const std::string& contents) {
    writeFile(path, contents.c_str());
    return backtrack::SettingsStore(path).load();
}

// A key that is missing, or present but unparseable, must leave the default in
// place rather than zeroing the field. A zeroed field is what an encoder then
// rejects at initialize().
void testEveryKeyFallsBackToItsDefault() {
    const AppSettings defaults;
    const AppSettings loaded = loadFrom(settingsPath(L"empty"), "# only a comment\n\n   \n");
    expect(loaded.video.fps == defaults.video.fps,
        "a missing fps key did not fall back to the default");
    expect(loaded.video.bitrateKbps == defaults.video.bitrateKbps,
        "a missing bitrate key did not fall back to the default");
    expect(loaded.video.codec == defaults.video.codec,
        "a missing codec key did not fall back to the default");
    expect(loaded.video.resolutionMode == defaults.video.resolutionMode,
        "a missing resolution key did not fall back to the default");
    expect(loaded.replay.seconds == defaults.replay.seconds,
        "a missing replay window did not fall back to the default");
    // A settings file from before the Sound checkbox has no audio key and
    // must fall back to the default rather than failing the load.
    expect(loaded.replay.diskBackedAudio == defaults.replay.diskBackedAudio,
        "a missing sound disk-backing key did not fall back to the default");
    expect(loaded.captureMicrophone == defaults.captureMicrophone,
        "a missing microphone key did not fall back to the default");
    expect(loaded.logLevel == defaults.logLevel,
        "a missing log level did not fall back to the default");
    expect(loaded.libraryGalleryView == defaults.libraryGalleryView,
        "a missing library view key did not fall back to the default");
    expect(loaded.organizeClipsByGame == defaults.organizeClipsByGame,
        "a missing game-folder key did not fall back to the default");

    // Malformed values are as bad as missing ones. readUInt takes digits only,
    // and readBool / the xxxFromName pair fall back rather than failing the load.
    const AppSettings malformed = loadFrom(settingsPath(L"malformed"),
        "video.fps=abc\n"
        "video.bitrateKbps=12x34\n"
        "video.codec=jpeg\n"
        "video.resolutionMode=8k\n"
        "replay.seconds=-5\n"
        "capture.microphone=maybe\n"
        "diagnostics.logLevel=shout\n"
        "library.viewMode=mosaic\n"
        "capture.backend=vulkan\n"
        "integrations.discordRichPresence=shout\n");
    expect(malformed.video.fps == defaults.video.fps,
        "a non-numeric fps did not fall back to the default");
    expect(malformed.video.bitrateKbps == defaults.video.bitrateKbps,
        "a partially numeric bitrate did not fall back to the default");
    expect(malformed.video.codec == defaults.video.codec,
        "an unknown codec name did not fall back through load()");
    expect(malformed.video.resolutionMode == defaults.video.resolutionMode,
        "an unknown resolution name did not fall back through load()");
    expect(malformed.replay.seconds == defaults.replay.seconds,
        "a negative replay window did not fall back to the default");
    expect(malformed.captureMicrophone == defaults.captureMicrophone,
        "a non-boolean microphone key did not fall back to the default");
    expect(malformed.logLevel == defaults.logLevel,
        "an unknown log level did not fall back through load()");
    expect(malformed.preferredCaptureBackend == defaults.preferredCaptureBackend,
        "an unknown capture backend did not fall back through load()");
}

// A hand-edited file is not obliged to use lowercase, and the user's
// capitalization is not a reason to lose a setting. Values above uint32_t are
// rejected by readUInt; the negative sign here proves it is not parsed as a
// digit run.
void testNumericAndBooleanSpellingsAndOverflow() {
    const AppSettings defaults;
    const AppSettings loaded = loadFrom(settingsPath(L"spellings"),
        "video.fps=144\n"
        "replay.seconds=4294967295\n"   // valid uint32_t, then clamped by sanitize
        "gpu.frameQueueLimit=4294967296\n"  // one above uint32_t: rejected outright
        "audio.microphone=YES\n"
        "audio.system=Off\n"
        "capture.cursor=TRUE\n"
        "capture.multiMonitorSupport=yes\n"
        "video.encoderLookahead=On\n");
    expect(loaded.video.fps == 144, "a valid fps was not read");
    expect(loaded.replay.seconds == 3600,
        "an in-range-but-huge replay window was not clamped to 3600");
    expect(loaded.gpu.frameQueueLimit == defaults.gpu.frameQueueLimit,
        "a value above uint32_t was not rejected and fell back to the default");
    expect(loaded.captureMicrophone, "readBool did not accept \"YES\"");
    expect(!loaded.captureSystemAudio, "readBool did not accept \"Off\"");
    expect(loaded.captureCursor, "readBool did not accept \"TRUE\"");
    expect(loaded.multiMonitorSupport, "readBool did not accept \"yes\"");
    expect(loaded.video.encoderLookahead, "readBool did not accept \"On\"");
}

// A repeated key keeps the last value, which is what makes a merge conflict
// resolvable by appending rather than by editing the file.
void testRepeatedKeysKeepTheLastValue() {
    const AppSettings loaded = loadFrom(settingsPath(L"repeated"),
        "video.fps=60\n"
        "video.fps=30\n"
        "video.fps=144\n"
        "capture.cursor=0\n"
        "capture.cursor=1\n");
    expect(loaded.video.fps == 144, "a repeated key did not keep the last value");
    expect(loaded.captureCursor, "a repeated boolean key did not keep the last value");
}

// Comments, blank lines and lines without '=' are ignored rather than being
// parsed into a key named after the whole line.
void testCommentsAndLinesWithoutEqualsAreIgnored() {
    const AppSettings loaded = loadFrom(settingsPath(L"comments"),
        "# Backtrack settings\n"
        "\n"
        "   \n"
        "this line has no equals sign\n"
        "video.fps=90\n"
        "  # indented comment\n"
        "   video.bitrateKbps   =   12000   \n");
    expect(loaded.video.fps == 90, "a valid key after comment lines was not read");
    expect(loaded.video.bitrateKbps == 12000,
        "surrounding whitespace around a key and value was not trimmed");
}

// sanitizeSettings is the trust boundary and runs on every load, so a
// hand-edited out-of-range value is normalized before it can reach an encoder.
void testOutOfRangeValuesAreClampedOnLoad() {
    const AppSettings defaults;
    const AppSettings loaded = loadFrom(settingsPath(L"clamped"),
        "video.resolutionMode=custom\n"
        "video.width=99999\n"
        "video.height=1\n"
        "video.bitrateKbps=1\n"
        "video.gopSeconds=9999\n"
        "video.encoderAQStrength=0\n"
        "video.encoderReferenceFrames=99\n"
        "video.encoderLookaheadDepth=999\n"
        "replay.seconds=0\n"
        "gpu.frameQueueLimit=0\n"
        "audio.inputVolumePercent=9999\n"
        "audio.outputVolumePercent=9999\n"
        "general.notificationSoundVolumePercent=9999\n");
    expect(loaded.video.width == 7680, "an oversized width was not clamped on load");
    expect(loaded.video.height == 16, "a zero height was not clamped up on load");
    expect(loaded.video.bitrateKbps == 1000, "a tiny bitrate was not clamped up on load");
    expect(loaded.video.gopSeconds == 30, "a huge GOP was not clamped down on load");
    expect(loaded.video.encoderAQStrength == 1, "AQ strength was not clamped up on load");
    expect(loaded.video.encoderReferenceFrames == 4,
        "reference frames were not clamped down on load");
    expect(loaded.video.encoderLookaheadDepth == 31,
        "lookahead depth was not capped on load");
    expect(loaded.replay.seconds == 1, "a zero replay window was not clamped up on load");
    expect(loaded.gpu.frameQueueLimit == 1,
        "a zero frame queue limit was not clamped up on load");
    expect(loaded.audioInputVolumePercent == 200,
        "an oversized input volume was not clamped on load");
    expect(loaded.audioOutputVolumePercent == 200,
        "an oversized output volume was not clamped on load");
    expect(loaded.notificationSoundVolumePercent == 100,
        "an oversized notification volume was not clamped on load");

    // readUInt takes digits only, so a negative is not parsed as a digit run: the
    // key falls back to its default rather than wrapping to a huge unsigned.
    const AppSettings negative = loadFrom(settingsPath(L"clamped_negative"),
        "audio.outputVolumePercent=-4\n"
        "replay.seconds=-1\n");
    expect(negative.audioOutputVolumePercent == defaults.audioOutputVolumePercent,
        "a negative volume did not fall back to the default");
    expect(negative.replay.seconds == defaults.replay.seconds,
        "a negative replay window did not fall back to the default");
}

// Renaming a key silently resets it, so the old spellings stay readable. These
// are the four migrations listed in settings/AGENTS.md.
void testLegacyKeySpellingsAreStillRead() {
    const AppSettings gallery = loadFrom(settingsPath(L"legacy_gallery"),
        "library.galleryView=1\n");
    expect(gallery.libraryGalleryView, "the legacy library.galleryView key was ignored");

    const AppSettings list = loadFrom(settingsPath(L"legacy_list"),
        "library.galleryView=0\n");
    expect(!list.libraryGalleryView, "a legacy library.galleryView=false was ignored");

    const AppSettings kills = loadFrom(settingsPath(L"legacy_league"),
        "integrations.leagueOfLegends.autoKillClips=1\n");
    expect(kills.gameIntegrations.leagueOfLegendsKillReminder,
        "the legacy league autoKillClips key was ignored");

    const AppSettings idle = loadFrom(settingsPath(L"legacy_idle"),
        "gpu.losslessIdleCoalescing=1\n");
    expect(idle.gpu.allowIdleFrameSkipping,
        "the legacy gpu.losslessIdleCoalescing key was ignored");

    const AppSettings richPresence = loadFrom(settingsPath(L"legacy_presence"),
        "integrations.discordRichPresence=1\n");
    expect(richPresence.gameIntegrations.discordRichPresence ==
            backtrack::GameIntegrationSettings::DiscordRichPresenceMode::Always,
        "a legacy boolean discordRichPresence=1 did not become Always");
    const AppSettings presenceOff = loadFrom(settingsPath(L"legacy_presence_off"),
        "integrations.discordRichPresence=false\n");
    expect(presenceOff.gameIntegrations.discordRichPresence ==
            backtrack::GameIntegrationSettings::DiscordRichPresenceMode::Off,
        "a legacy boolean discordRichPresence=false did not become Off");

    // The current spelling wins over the legacy one.
    const AppSettings current = loadFrom(settingsPath(L"legacy_precedence"),
        "library.galleryView=1\n"
        "library.viewMode=list\n"
        "integrations.leagueOfLegends.autoKillClips=1\n"
        "integrations.leagueOfLegends.killReminder=0\n"
        "integrations.discordRichPresence=1\n"
        "integrations.discordRichPresence=fullscreen-only\n");
    expect(!current.libraryGalleryView, "the current library.viewMode lost to the legacy key");
    expect(!current.gameIntegrations.leagueOfLegendsKillReminder,
        "the current league killReminder key lost to the legacy one");
    expect(current.gameIntegrations.discordRichPresence ==
            backtrack::GameIntegrationSettings::DiscordRichPresenceMode::FullscreenOnly,
        "the current discordRichPresence spelling did not win over the legacy bool");
}

// Unknown keys are ignored, which is what makes an outright rename silent rather
// than a load failure.
void testUnknownKeysAreIgnored() {
    const AppSettings loaded = loadFrom(settingsPath(L"unknown"),
        "video.fps=75\n"
        "some.future.setting=12345\n"
        "video.encoderLookaheadDepth2=8\n"
        "clips.directory=\n"  // empty: replaced with the platform default
        "audio.soundSeparation.count=1\n"
        "audio.soundSeparation.0.name=Alpha\n"
        "audio.soundSeparation.0.path=C:\\Games\\Alpha\\alpha.exe\n");
    expect(loaded.video.fps == 75, "an unknown key disturbed a known one");
    expect(loaded.clipDirectory == backtrack::defaultClipDirectory(),
        "an empty clip directory was not replaced with the default on load");
    expect(loaded.soundSeparationApps.size() == 1,
        "the indexed app list was not read through load()");
    expect(!loaded.soundSeparationApps.empty() && loaded.soundSeparationApps[0].name == L"Alpha",
        "an indexed app-list name was not read");
}

// The file is UTF-8 without a BOM. A hand-edited non-ASCII path (a user's
// display name, a folder name) must survive load, and save must not smuggle a
// BOM in that the next load would turn into a mangled first key.
void testUtf8RoundTripsThroughSaveAndLoad() {
    const auto path = settingsPath(L"utf8");
    const std::wstring folderName = L"Klipy — \u00e9\u00fc\u00f1\u4e2d\u6587";

    backtrack::AppSettings settings;
    settings.clipDirectory = testRoot() / L"clips" / folderName;
    settings.soundSeparationApps = {
        {L"N\x00e9on", L"C:\\Games\\Caf\x00e9\\caf\x00e9.exe", true},
    };
    backtrack::SettingsStore store(path);
    store.save(settings);

    // No BOM: the parser reads raw bytes and would key the first line on it.
    {
        std::ifstream raw(path, std::ios::binary);
        expect(raw.is_open(), "save produced no settings file");
        char first = 0;
        raw.get(first);
        expect(static_cast<unsigned char>(first) != 0xEF,
            "save wrote a UTF-8 BOM, which would corrupt the first key on load");
    }

    const AppSettings loaded = store.load();
    expect(loaded.clipDirectory == settings.clipDirectory,
        "a non-ASCII clip directory did not round trip");
    expect(loaded.soundSeparationApps.size() == 1,
        "the non-ASCII app list did not survive the round trip");
    expect(!loaded.soundSeparationApps.empty() &&
            loaded.soundSeparationApps[0].name == settings.soundSeparationApps[0].name,
        "a non-ASCII app name did not round trip");

    // A save over an existing file must go through the atomic temp-and-rename
    // publish: the live file is replaced whole and no staging file is left.
    store.save(settings);
    const std::filesystem::path tempPath = path.wstring() + L".tmp";
    expect(!std::filesystem::exists(tempPath),
        "save left its staging file behind after a second save");
    expect(store.load().clipDirectory == settings.clipDirectory,
        "a second save over an existing file broke the round trip");
}

// The publish rule is what makes save() safe to run against a file another
// process holds open - a sync client, an antivirus scanner, a second instance.
// Holding settings.ini with no FILE_SHARE_* denies both write and delete, so the
// swap fails with ERROR_SHARING_VIOLATION after the temp file was already
// written. What must survive is the previous file, byte for byte: a save that
// could not complete may not damage what was already there. save() returns void
// and reports through the logger, which this suite does not link, so the
// observable is disk state alone.
void testFailedSavePreservesTheOldFileAndLeavesNoTemp() {
    const auto path = settingsPath(L"sharing_violation");
    const backtrack::SettingsStore store(path);

    // A distinctive value so "unchanged" cannot be confused with "rewritten to
    // the same defaults".
    AppSettings original;
    original.clipDirectory = testRoot() / L"clips";
    original.video.fps = 123;
    original.replay.seconds = 777;
    store.save(original);
    const std::vector<char> before = readFileBytes(path);
    expect(!before.empty(), "the baseline settings file was not written");

    HANDLE held = CreateFileW(path.c_str(), GENERIC_READ, 0 /* no FILE_SHARE_* */,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    expect(held != INVALID_HANDLE_VALUE,
        "could not open settings.ini exclusively; the sharing-violation case would not be reached");

    AppSettings replacement = original;
    replacement.video.fps = 45;
    replacement.replay.seconds = 30;
    store.save(replacement);

    CloseHandle(held);

    expect(readFileBytes(path) == before,
        "a save that could not swap left the previous settings file damaged");
    expect(!std::filesystem::exists(path.wstring() + L".tmp"),
        "a failed save left its staging file behind");

    // The lock is gone, so the same save must now land: the failure above must
    // be the lock's doing and not a broken store or an early return.
    store.save(replacement);
    const AppSettings reloaded = store.load();
    expect(reloaded.video.fps == 45 && reloaded.replay.seconds == 30,
        "save did not take effect once the file was no longer locked");
    expect(readFileBytes(path) != before,
        "the unlocked save did not change the file, so the locked case proves nothing");
}

// The other load cases hand-write one key at a time, which proves the parser
// reads what it expects but not that the writer and the parser agree. One
// configuration with a non-default value in every settings group, written and
// read back, is the only thing that catches a serializer/parser key mismatch.
void testEverySerializedKeyRoundTrips() {
    const auto path = settingsPath(L"roundtrip");

    AppSettings original;
    original.clipDirectory = testRoot() / L"clips";
    original.organizeClipsByGame = true;
    original.replayRingDirectory = testRoot() / L"ring";
    original.video.resolutionMode = backtrack::ResolutionMode::Custom;
    original.video.width = 2560;
    original.video.height = 1440;
    original.video.fps = 144;
    original.video.bitrateKbps = 45000;
    original.video.gopSeconds = 4;
    original.video.codec = backtrack::VideoCodec::Hevc;
    original.video.encoderPreset = backtrack::EncoderPreset::P2;
    original.video.encoderMode = backtrack::EncoderMode::Lossless;
    original.video.encoderProfile = backtrack::EncoderProfile::Balanced;
    original.video.encoderLookahead = true;
    original.video.encoderLookaheadDepth = 8;
    original.video.encoderSpatialAQ = true;
    original.video.encoderAQStrength = 5;
    original.video.encoderTemporalAQ = true;
    original.video.encoderMultipass = backtrack::EncoderMultipass::FullResolution;
    original.video.encoderBFrames = true;
    original.video.encoderAdaptiveBFrames = true;
    original.video.encoderAdaptiveIFrames = true;
    original.video.encoderZeroReorderDelay = false;
    original.video.encoderReferenceFrames = 3;
    original.replay.seconds = 45;
    original.replay.enabled = false;
    original.replay.diskBackedVideo = true;
    original.replay.diskBackedAudio = true;
    original.gpu.adaptiveMode = backtrack::GpuAdaptiveMode::Aggressive;
    original.gpu.wgcZeroCopy = false;
    original.gpu.frameQueueLimit = 2;
    original.gpu.allowIdleFrameSkipping = false;
    original.gpu.stableMultimonitorFrames = false;
    original.preferredCaptureBackend = backtrack::CaptureBackend::DesktopDuplication;
    original.captureMicrophone = false;
    original.captureSystemAudio = false;
    original.audioInputDeviceId = L"{0.0.0.00000000}.{mic}.Input";
    original.audioOutputDeviceId = L"{0.0.0.00000000}.{speaker}.Output";
    original.audioInputVolumePercent = 130;
    original.audioOutputVolumePercent = 45;
    original.audioInputAutoGainEnabled = true;
    original.soundSeparationEnabled = true;
    original.soundSeparationApps = {{L"Alpha", L"C:\\Games\\Alpha\\alpha.exe", false}};
    original.autoRecordEnabled = true;
    original.autoRecordApps = {{L"Beta", L"C:\\Games\\Beta\\beta.exe"}};
    original.startWithWindowsMinimized = true;
    original.pruneStaleMicrophoneConsentEntries = true;
    original.exitToTray = true;
    original.checkForUpdates = false;
    original.notificationSoundVolumePercent = 35;
    original.customNotificationSoundPath = L"C:\\Sounds\\saved.wav";
    original.monitorIndex = 1;
    original.multiMonitorSupport = true;
    original.followFocusedMonitor = true;
    original.followMouseMonitor = true;
    original.captureCursor = false;
    original.libraryGalleryView = true;
    original.logLevel = backtrack::LogLevel::Warning;
    original.hotkeys.startStopModifiers = MOD_CONTROL | MOD_SHIFT;
    original.hotkeys.startStopVirtualKey = VK_F7;
    original.hotkeys.saveReplayModifiers = MOD_ALT;
    original.hotkeys.saveReplayVirtualKey = VK_F8;
    original.gameIntegrations.leagueOfLegendsKillReminder = true;
    original.gameIntegrations.voiceCommand =
        backtrack::GameIntegrationSettings::VoiceCommandMode::RecordVideo;
    original.gameIntegrations.discordRichPresence =
        backtrack::GameIntegrationSettings::DiscordRichPresenceMode::FullscreenOnly;
    original.gameIntegrations.discordRichPresenceResetTimerOnSwitch = false;

    // save() reports through the logger and may return a status; neither is what
    // this case is about, so the result is deliberately ignored.
    const backtrack::SettingsStore store(path);
    store.save(original);

    const AppSettings reloaded = store.load();
    if (reloaded != original) {
        std::ostringstream detail;
        detail << "a value did not survive save() and load(): fps " << original.video.fps
               << " -> " << reloaded.video.fps << ", preset "
               << static_cast<int>(original.video.encoderPreset) << " -> "
               << static_cast<int>(reloaded.video.encoderPreset) << ", logLevel "
               << static_cast<int>(original.logLevel) << " -> " << static_cast<int>(reloaded.logLevel)
               << ", gallery " << original.libraryGalleryView << " -> " << reloaded.libraryGalleryView
               << ", ring '" << backtrack::wideToUtf8(original.replayRingDirectory.wstring()) << "' -> '"
               << backtrack::wideToUtf8(reloaded.replayRingDirectory.wstring()) << "'";
        throw std::runtime_error(detail.str());
    }
}

// The user's capitalization is not a reason to drop the library view back to
// the default. readBool folds case; the view mode used to compare against three
// spellings only, so "gAlLery" silently reset the library to a list view.
void testLibraryViewModeIsCaseInsensitive() {
    for (const char* value : {"gallery", "Gallery", "GALLERY", "gAlLeRy"}) {
        const AppSettings loaded = loadFrom(
            settingsPath(L"viewmode"), std::string("library.viewMode=") + value + "\n");
        expect(loaded.libraryGalleryView,
            std::string("library.viewMode=") + value + " did not select the gallery view");
    }
    for (const char* value : {"list", "List", "LIST", "lIsT"}) {
        const AppSettings loaded = loadFrom(
            settingsPath(L"viewmode"), std::string("library.viewMode=") + value + "\n");
        expect(!loaded.libraryGalleryView,
            std::string("library.viewMode=") + value + " did not select the list view");
    }

    // The legacy boolean spelling goes through readBool and is equally
    // case-insensitive.
    const AppSettings legacy = loadFrom(settingsPath(L"viewmode_legacy"),
        "library.galleryView=YeS\n");
    expect(legacy.libraryGalleryView, "the legacy gallery key is not case-insensitive");
}

// load() runs before the logger is initialized, so it must not throw on a
// missing file, an empty file, or a file it cannot read.
void testLoadOfAMissingOrUnreadableFileReturnsDefaults() {
    const auto path = testRoot() / L"settings" / L"does_not_exist.ini";
    const AppSettings defaults;
    expect(backtrack::SettingsStore(path).load().video.fps == defaults.video.fps,
        "loading a missing file did not return the defaults");

    const auto directoryPath = testRoot() / L"settings" / L"a_directory.ini";
    std::error_code error;
    std::filesystem::create_directories(directoryPath, error);
    expect(!error, "could not create the directory-shaped settings fixture");
    expect(backtrack::SettingsStore(directoryPath).load().video.fps == defaults.video.fps,
        "loading a path that is a directory did not return the defaults");

    std::filesystem::remove_all(testRoot(), error);
}

// sanitizeSettings replaces an empty clipDirectory with the default, so every
// case that is not about that branch sets one.
AppSettings baseSettings() {
    AppSettings settings;
    settings.clipDirectory = testRoot() / L"clips";
    return settings;
}

void testOutOfRangeValuesAreClamped() {
    AppSettings settings = baseSettings();
    // Custom, so the stored dimensions survive to the clamp instead of being
    // overwritten by the resolution preset.
    settings.video.resolutionMode = backtrack::ResolutionMode::Custom;
    settings.video.width = 1919;    // odd and in range -> forced even
    settings.video.height = 99999;  // above the 4320 ceiling
    settings.video.fps = 0;
    settings.video.bitrateKbps = 999999;
    settings.video.gopSeconds = 500;
    settings.video.encoderLookaheadDepth = 99;
    settings.video.encoderAQStrength = 0;
    settings.video.encoderReferenceFrames = 99;
    settings.replay.seconds = 100000;
    settings.gpu.frameQueueLimit = 0;
    settings.audioInputVolumePercent = 500;
    settings.audioOutputVolumePercent = 100000;
    settings.notificationSoundVolumePercent = 300;

    const AppSettings clamped = backtrack::sanitizeSettings(settings);
    expect(clamped.video.width == 1918, "an odd width was not rounded down to an even value");
    expect(clamped.video.height == 4320, "height was not clamped to 4320");
    expect(clamped.video.fps == 1, "fps was not clamped up to 1");
    expect(clamped.video.bitrateKbps == 200000, "bitrate was not clamped to 200000");
    expect(clamped.video.gopSeconds == 30, "GOP length was not clamped to 30");
    expect(clamped.video.encoderLookaheadDepth == 31, "lookahead depth was not capped at 31");
    expect(clamped.video.encoderAQStrength == 1, "AQ strength was not clamped up to 1");
    expect(clamped.video.encoderReferenceFrames == 4, "reference frames were not clamped to 4");
    expect(clamped.replay.seconds == 3600, "replay seconds were not clamped to 3600");
    expect(clamped.gpu.frameQueueLimit == 1, "frame queue limit was not clamped up to 1");
    expect(clamped.audioInputVolumePercent == 200, "input volume was not clamped to 200");
    expect(clamped.audioOutputVolumePercent == 200, "output volume was not clamped to 200");
    expect(clamped.notificationSoundVolumePercent == 100,
        "notification volume was not clamped to 100");

    // The opposite ends of each range, including lookahead depth which is capped
    // but has no lower bound.
    AppSettings low = baseSettings();
    low.video.resolutionMode = backtrack::ResolutionMode::Custom;
    low.video.width = 1;      // below the 16 floor
    low.video.height = 0;
    low.video.fps = 1000;
    low.video.bitrateKbps = 1;
    low.video.gopSeconds = 0;
    low.video.encoderLookaheadDepth = 0;
    low.video.encoderAQStrength = 99;
    low.video.encoderReferenceFrames = 0;
    low.replay.seconds = 0;
    low.gpu.frameQueueLimit = 99;
    low.audioInputVolumePercent = 0;
    low.audioOutputVolumePercent = 0;
    low.notificationSoundVolumePercent = 0;

    const AppSettings clampedLow = backtrack::sanitizeSettings(low);
    expect(clampedLow.video.width == 16, "width was not clamped up to 16");
    expect(clampedLow.video.height == 16, "height was not clamped up to 16");
    expect(clampedLow.video.fps == 240, "fps was not clamped down to 240");
    expect(clampedLow.video.bitrateKbps == 1000, "bitrate was not clamped up to 1000");
    expect(clampedLow.video.gopSeconds == 1, "GOP length was not clamped up to 1");
    expect(clampedLow.video.encoderLookaheadDepth == 0, "lookahead depth gained a lower bound it does not have");
    expect(clampedLow.video.encoderAQStrength == 15, "AQ strength was not clamped down to 15");
    expect(clampedLow.video.encoderReferenceFrames == 1, "reference frames were not clamped up to 1");
    expect(clampedLow.replay.seconds == 1, "replay seconds were not clamped up to 1");
    expect(clampedLow.gpu.frameQueueLimit == 8, "frame queue limit was not clamped down to 8");
    expect(clampedLow.audioInputVolumePercent == 0, "input volume lost its 0 lower bound");
    expect(clampedLow.audioOutputVolumePercent == 0, "output volume lost its 0 lower bound");
    expect(clampedLow.notificationSoundVolumePercent == 0, "notification volume lost its 0 lower bound");
}

void testInRangeValuesAreUntouched() {
    // Every branch of sanitizeSettings that fires on the defaults either makes
    // no change or makes this one, so a valid configuration must survive a
    // round trip byte for byte. multiMonitor/follow flags are enabled so the
    // followMouseMonitor reset does not fire.
    AppSettings settings = baseSettings();
    settings.multiMonitorSupport = true;
    settings.followFocusedMonitor = true;
    settings.followMouseMonitor = true;

    expect(backtrack::sanitizeSettings(settings) == settings,
        "sanitizeSettings modified an in-range configuration");
}

void testResolutionPresetOverridesStoredDimensions() {
    AppSettings settings = baseSettings();
    settings.video.resolutionMode = backtrack::ResolutionMode::P4K;
    settings.video.width = 640;
    settings.video.height = 480;

    AppSettings sanitized = backtrack::sanitizeSettings(settings);
    expect(sanitized.video.width == 3840 && sanitized.video.height == 2160,
        "a 4K preset did not replace the stored dimensions");

    settings.video.resolutionMode = backtrack::ResolutionMode::P240;
    settings.video.width = 3840;
    settings.video.height = 2160;
    sanitized = backtrack::sanitizeSettings(settings);
    expect(sanitized.video.width == 426 && sanitized.video.height == 240,
        "a 240p preset did not replace the stored dimensions");

    // An empty clip directory is replaced with the platform default.
    settings = baseSettings();
    settings.clipDirectory.clear();
    expect(backtrack::sanitizeSettings(settings).clipDirectory == backtrack::defaultClipDirectory(),
        "an empty clip directory was not replaced with the default");
}

void testEnumNamesFallBackInsteadOfAborting() {
    // Enum fields are not clamped by sanitizeSettings; the parse path is what
    // keeps them in range, so a hand-edited value falls back to a known one.
    expect(backtrack::codecFromName(L"nonsense") == backtrack::VideoCodec::H264,
        "an unknown codec name did not fall back to H.264");
    expect(backtrack::resolutionModeFromName(L"nonsense") == backtrack::ResolutionMode::P1080,
        "an unknown resolution name did not fall back to 1080p");
    expect(backtrack::encoderPresetFromName(L"nonsense") == backtrack::EncoderPreset::P4,
        "an unknown preset name did not fall back to P4");
    expect(backtrack::encoderModeFromName(L"nonsense") == backtrack::EncoderMode::HighQuality,
        "an unknown quality name did not fall back to high-quality");
    expect(backtrack::encoderProfileFromName(L"nonsense") == backtrack::EncoderProfile::LowestGpu,
        "an unknown profile name did not fall back to lowest-gpu");
    expect(backtrack::encoderMultipassFromName(L"nonsense") == backtrack::EncoderMultipass::Disabled,
        "an unknown multipass name did not fall back to disabled");
    expect(backtrack::captureBackendFromName(L"nonsense") == backtrack::CaptureBackend::WindowsGraphicsCapture,
        "an unknown capture backend name did not fall back to WGC");

    // Known values must still round trip; a suite that only checks fallbacks
    // passes even if every mapping is broken.
    expect(std::wstring(backtrack::encoderModeName(backtrack::encoderModeFromName(L"lossless"))) == L"lossless",
        "the lossless quality name did not round trip");
    expect(std::wstring(backtrack::encoderProfileName(backtrack::encoderProfileFromName(L"balanced"))) == L"balanced",
        "the balanced profile name did not round trip");
}

void testAppListsAreDeduped() {
    AppSettings settings = baseSettings();
    settings.soundSeparationApps = {
        {L"", L"C:\\Games\\Alpha\\alpha.exe", true},
        {L"   ", L"C:\\Games\\Alpha\\ALPHA.EXE", false},  // same file, different case
        {L"NoPath", L"", true},                          // nothing to run
        {L"  Beta  ", L"C:\\Games\\Beta\\beta.exe", false},
    };
    settings.autoRecordApps = {
        {L"Gamma", L"C:\\Games\\Gamma\\gamma.exe"},
    };

    const AppSettings sanitized = backtrack::sanitizeSettings(settings);
    expect(sanitized.soundSeparationApps.size() == 2,
        "the sound separation list was not deduplicated");
    expect(sanitized.soundSeparationApps[0].name == L"alpha",
        "a blank sound separation name was not filled from the executable stem");
    expect(sanitized.soundSeparationApps[0].muted, "the first duplicate won with the wrong muted flag");
    expect(sanitized.soundSeparationApps[1].name == L"Beta",
        "a sound separation name was not trimmed");
    expect(sanitized.autoRecordApps.size() == 1 && sanitized.autoRecordApps[0].name == L"Gamma",
        "a valid auto record entry was dropped");
}

void testHotkeysAreMaskedAndUnsafeBindingsRejected() {
    // Win and the other Win32 bookkeeping bits are not supported by a global
    // hotkey; they are masked off rather than rejected.
    AppSettings settings = baseSettings();
    settings.hotkeys.startStopModifiers = MOD_CONTROL | MOD_WIN;
    settings.hotkeys.startStopVirtualKey = VK_F9;
    settings.hotkeys.saveReplayModifiers = MOD_ALT | MOD_SHIFT;
    settings.hotkeys.saveReplayVirtualKey = VK_F10;

    AppSettings sanitized = backtrack::sanitizeSettings(settings);
    expect(sanitized.hotkeys.startStopModifiers == MOD_CONTROL,
        "an unsupported hotkey modifier was not masked off");
    expect(sanitized.hotkeys.startStopVirtualKey == VK_F9,
        "a safe hotkey was reset by the modifier mask");
    expect(sanitized.hotkeys.saveReplayModifiers == (MOD_ALT | MOD_SHIFT),
        "supported hotkey modifiers were not left alone");

    // Shift+letter is a bare typing shortcut, not a safe global hotkey.
    settings = baseSettings();
    settings.hotkeys.startStopModifiers = MOD_SHIFT;
    settings.hotkeys.startStopVirtualKey = L'A';
    sanitized = backtrack::sanitizeSettings(settings);
    expect(sanitized.hotkeys.startStopVirtualKey == VK_F9,
        "an unsafe hotkey was not reset to the default");
    expect(sanitized.hotkeys.startStopModifiers == (MOD_CONTROL | MOD_ALT),
        "the modifier of a reset hotkey was not reset with it");

    // A key code outside the byte Windows can register globally.
    settings = baseSettings();
    settings.hotkeys.startStopModifiers = MOD_CONTROL | MOD_ALT;
    settings.hotkeys.startStopVirtualKey = 0x100;
    expect(backtrack::sanitizeSettings(settings).hotkeys.startStopVirtualKey == VK_F9,
        "an out-of-range virtual key was not reset to the default");

    // Two bindings on the same chord would make one unreachable.
    settings = baseSettings();
    settings.hotkeys.startStopModifiers = MOD_CONTROL;
    settings.hotkeys.startStopVirtualKey = VK_F1;
    settings.hotkeys.saveReplayModifiers = MOD_CONTROL;
    settings.hotkeys.saveReplayVirtualKey = VK_F1;
    sanitized = backtrack::sanitizeSettings(settings);
    expect(sanitized.hotkeys.saveReplayVirtualKey == VK_F10,
        "a duplicated hotkey binding was not reset");
    expect(sanitized.hotkeys.startStopVirtualKey == VK_F1,
        "the first half of a duplicated pair was reset as well");

    // Key code 0 means "unbound" and is explicitly allowed to stay that way.
    settings = baseSettings();
    settings.hotkeys.saveReplayModifiers = MOD_CONTROL | MOD_ALT;
    settings.hotkeys.saveReplayVirtualKey = 0;
    expect(backtrack::sanitizeSettings(settings).hotkeys.saveReplayVirtualKey == 0,
        "an unbound hotkey was reset to the default");
}

void testClipPathGateRejectsEverythingOutsideTheClipDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(testRoot(), ignored);

    const std::filesystem::path root = testRoot() / L"clips";
    const std::filesystem::path outside = testRoot() / L"outside";
    const std::filesystem::path elsewhere = testRoot() / L"elsewhere";
    const std::filesystem::path managed = root / L"managed.mp4";
    const std::filesystem::path wrongExtension = root / L"notes.mkv";
    const std::filesystem::path sibling = outside / L"sibling.mp4";
    const std::filesystem::path absolute = elsewhere / L"absolute.mp4";
    const std::filesystem::path missing = root / L"never-created.mp4";

    writeFile(managed);
    writeFile(wrongExtension);
    writeFile(sibling);
    writeFile(absolute);

    backtrack::ClipManager manager(root);

    // The gate is private, so it is exercised through the destructive operation
    // it guards: every refusal must also leave the file on disk.
    expect(!manager.removeClip(root / L".." / L"outside" / L"sibling.mp4"),
        "a .. traversal out of the clip directory was not rejected");
    expect(std::filesystem::exists(sibling),
        "the traversal attempt deleted a file outside the clip directory");

    expect(!manager.removeClip(sibling),
        "a path in a sibling directory was not rejected");
    expect(std::filesystem::exists(sibling),
        "a sibling-directory delete removed a file outside the clip directory");

    expect(!manager.removeClip(absolute),
        "an absolute path outside the clip directory was not rejected");
    expect(std::filesystem::exists(absolute),
        "an absolute-path delete removed a file outside the clip directory");

    expect(!manager.removeClip(wrongExtension),
        "a non-mp4 file inside the clip directory was not rejected");
    expect(std::filesystem::exists(wrongExtension),
        "a non-mp4 file inside the clip directory was deleted");

    expect(!manager.removeClip(missing),
        "an mp4 that does not exist was treated as a managed clip");
    expect(!manager.removeClip(std::filesystem::path{}),
        "an empty path was treated as a managed clip");

    expect(manager.removeClip(managed), "a managed mp4 inside the clip directory was refused");
    expect(!std::filesystem::exists(managed), "a managed mp4 was not deleted");

    std::filesystem::remove_all(testRoot(), ignored);
}

// Clips saved with per-game folders on must still show up in the library: the
// listing scans one level of subfolders, while dot-directories like
// .backtrack_tmp (staged .publish.mp4 files) are never finished clips.
void testListClipsIncludesGameSubfolders() {
    std::error_code ignored;
    std::filesystem::remove_all(testRoot(), ignored);

    const std::filesystem::path root = testRoot() / L"clips";
    writeFile(root / L"root.mp4");
    writeFile(root / L"League of Legends" / L"game.mp4");
    writeFile(root / L"clips" / L"legacy.mp4");
    writeFile(root / L".backtrack_tmp" / L"staged.mp4");

    backtrack::ClipManager manager(root);
    const auto clips = manager.listClips();

    auto contains = [&](const std::filesystem::path& path) {
        return std::any_of(clips.begin(), clips.end(), [&](const backtrack::ClipInfo& info) {
            return info.path == path;
        });
    };
    expect(clips.size() == 3, "a game subfolder clip did not show up in the library");
    expect(contains(root / L"root.mp4"), "a root clip went missing from the library");
    expect(contains(root / L"League of Legends" / L"game.mp4"),
        "a clip inside a game subfolder did not show up in the library");
    expect(contains(root / L"clips" / L"legacy.mp4"),
        "a clip inside the legacy clips subfolder did not show up in the library");

    std::filesystem::remove_all(testRoot(), ignored);
}

} // namespace

int main() {
    try {
        testOutOfRangeValuesAreClamped();
        testInRangeValuesAreUntouched();
        testResolutionPresetOverridesStoredDimensions();
        testEnumNamesFallBackInsteadOfAborting();
        testAppListsAreDeduped();
        testHotkeysAreMaskedAndUnsafeBindingsRejected();
        testEveryKeyFallsBackToItsDefault();
        testNumericAndBooleanSpellingsAndOverflow();
        testRepeatedKeysKeepTheLastValue();
        testCommentsAndLinesWithoutEqualsAreIgnored();
        testOutOfRangeValuesAreClampedOnLoad();
        testLegacyKeySpellingsAreStillRead();
        testUnknownKeysAreIgnored();
        testUtf8RoundTripsThroughSaveAndLoad();
        testFailedSavePreservesTheOldFileAndLeavesNoTemp();
        testEverySerializedKeyRoundTrips();
        testLibraryViewModeIsCaseInsensitive();
        testLoadOfAMissingOrUnreadableFileReturnsDefaults();
        testClipPathGateRejectsEverythingOutsideTheClipDirectory();
        testListClipsIncludesGameSubfolders();
        std::cout << "SettingsClipGuard tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SettingsClipGuard test failure: " << error.what() << '\n';
        return 1;
    }
}
