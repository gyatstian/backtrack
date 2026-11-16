#include "settings/SettingsStore.h"

#include "core/LogContext.h"
#include "core/LogTiming.h"
#include "core/Logger.h"
#include "platform/AtomicWrite.h"
#include "platform/Win32Util.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <unordered_set>

namespace backtrack {

namespace {

// One case fold per parse: the persisted spellings are lowercase, so a
// hand-edited "LOW-LATENCY" must not land on the fallback value.
std::wstring foldName(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return value;
}

// Hard cap for a persisted app list, mirroring sanitizeSettings.
constexpr uint32_t kMaxAppListEntries = 128;

// The settings file is trusted no further than this: one corrupt line must not
// buffer the whole file while it waits for a newline that never comes.
constexpr size_t kMaxLineBytes = 64u * 1024;

uint32_t readUInt(const std::map<std::wstring, std::wstring>& values, const std::wstring& key, uint32_t fallback) {
    const auto it = values.find(key);
    if (it == values.end()) {
        return fallback;
    }
    const auto& text = it->second;
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](wchar_t ch) { return iswdigit(ch) != 0; })) {
        return fallback;
    }
    try {
        const auto parsed = std::stoull(text);
        if (parsed > (std::numeric_limits<uint32_t>::max)()) {
            return fallback;
        }
        return static_cast<uint32_t>(parsed);
    } catch (...) {
        return fallback;
    }
}

bool readBool(const std::map<std::wstring, std::wstring>& values, const std::wstring& key, bool fallback) {
    const auto it = values.find(key);
    if (it == values.end()) {
        return fallback;
    }
    auto value = foldName(it->second);
    if (value == L"1" || value == L"true" || value == L"yes" || value == L"on") {
        return true;
    }
    if (value == L"0" || value == L"false" || value == L"no" || value == L"off") {
        return false;
    }
    return fallback;
}

std::wstring readString(const std::map<std::wstring, std::wstring>& values, const std::wstring& key, const std::wstring& fallback) {
    const auto it = values.find(key);
    return it == values.end() ? fallback : it->second;
}

bool readLibraryGalleryView(const std::map<std::wstring, std::wstring>& values, bool fallback) {
    const auto modeIt = values.find(L"library.viewMode");
    if (modeIt != values.end()) {
        // Case-insensitive like readBool, because the user's capitalization is
        // not a reason to drop a gallery view back to the default.
        const std::wstring mode = foldName(modeIt->second);
        if (mode == L"gallery") {
            return true;
        }
        if (mode == L"list") {
            return false;
        }
    }
    return readBool(values, L"library.galleryView", fallback);
}

uint32_t clampEven(uint32_t value, uint32_t minimum, uint32_t maximum) {
    value = std::clamp(value, minimum, maximum);
    return (value % 2) == 0 ? value : value - 1;
}

// Shared cleanup for the per-executable app lists (sound separation, auto
// record): trim/default names, drop empty and duplicate paths, cap the size.
template <typename App>
void dedupeNamedExecutableApps(std::vector<App>& apps) {
    std::vector<App> kept;
    kept.reserve(apps.size());
    std::unordered_set<std::wstring> seen;
    for (auto app : apps) {
        if (app.executablePath.empty()) {
            continue;
        }
        app.name = trimText(std::move(app.name));
        if (app.name.empty()) {
            app.name = app.executablePath.stem().wstring();
        }
        std::replace_if(app.name.begin(), app.name.end(), [](wchar_t ch) {
            return ch == L'\r' || ch == L'\n';
        }, L' ');
        app.name = trimText(std::move(app.name));
        const std::wstring key = normalizedPathKey(app.executablePath);
        if (key.empty() || seen.contains(key)) {
            continue;
        }
        seen.insert(key);
        kept.push_back(std::move(app));
        if (kept.size() >= kMaxAppListEntries) {
            break;
        }
    }
    apps = std::move(kept);
}

struct ResolutionPreset {
    ResolutionMode mode;
    const wchar_t* name;
    uint32_t width;
    uint32_t height;
};

constexpr ResolutionPreset kResolutionPresets[] = {
    {ResolutionMode::P240, L"240p", 426, 240},
    {ResolutionMode::P480, L"480p", 854, 480},
    {ResolutionMode::P720, L"720p", 1280, 720},
    {ResolutionMode::P1080, L"1080p", 1920, 1080},
    {ResolutionMode::P2K, L"2k", 2560, 1440},
    {ResolutionMode::P4K, L"4k", 3840, 2160},
    {ResolutionMode::Custom, L"custom", 0, 0},
};

bool resolutionPresetSize(ResolutionMode mode, uint32_t& width, uint32_t& height) {
    for (const auto& preset : kResolutionPresets) {
        if (preset.mode == mode && mode != ResolutionMode::Custom) {
            width = preset.width;
            height = preset.height;
            return true;
        }
    }
    return false;
}

} // namespace

// Hotkey safety is defined here because sanitizeSettings is the authority; the
// UI calls these instead of keeping its own copy that could drift from it.
uint32_t supportedHotkeyModifiers(uint32_t modifiers) {
    return modifiers & (MOD_ALT | MOD_CONTROL | MOD_SHIFT);
}

bool isSafeBareHotkey(uint32_t virtualKey) {
    return (virtualKey >= VK_F1 && virtualKey <= VK_F24) ||
           virtualKey == VK_PAUSE ||
           virtualKey == VK_SNAPSHOT ||
           virtualKey == VK_MEDIA_PLAY_PAUSE ||
           virtualKey == VK_MEDIA_STOP ||
           virtualKey == VK_MEDIA_NEXT_TRACK ||
           virtualKey == VK_MEDIA_PREV_TRACK;
}

bool isSafeGlobalHotkey(uint32_t modifiers, uint32_t virtualKey) {
    if (virtualKey == 0) {
        return true;
    }
    if (virtualKey > 0xfe) {
        return false;
    }
    modifiers = supportedHotkeyModifiers(modifiers);
    if (modifiers == 0) {
        return isSafeBareHotkey(virtualKey);
    }
    if (isSafeBareHotkey(virtualKey)) {
        return true;
    }
    return (modifiers & (MOD_CONTROL | MOD_ALT)) != 0;
}

SettingsStore::SettingsStore(std::filesystem::path path)
    : path_(std::move(path)) {
}

bool SettingsStore::normalizeLoaded(const AppSettings& settings, LoadStatus status) const {
    return status != LoadStatus::Loaded || save(settings);
}

AppSettings SettingsStore::load(LoadStatus* status) const {
    AppSettings settings;
    settings.clipDirectory = defaultClipDirectory();
    if (status) {
        *status = LoadStatus::Failed;
    }

    // MSVC filebuf can treat read errors as EOF; exclude writers while reading.
    HANDLE handle = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (status && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) {
            *status = LoadStatus::Missing;
            LogContext::breadcrumb(L"settings file missing; defaults used");
        } else {
            LogContext::breadcrumb(L"settings open failed (Win32 error " + std::to_wstring(error) + L")");
        }
        return sanitizeSettings(settings);
    }
    const std::unique_ptr<void, decltype(&CloseHandle)> input(handle, CloseHandle);

    std::map<std::wstring, std::wstring> values;
    std::string rawLine;
    bool firstLine = true;
    bool discardingLine = false;
    const auto parseLine = [&] {
        if (!rawLine.empty() && rawLine.back() == '\r') {
            rawLine.pop_back();
        }
        if (firstLine) {
            firstLine = false;
            // A UTF-8 BOM would otherwise join the first key and make that
            // setting unreachable.
            if (rawLine.starts_with("\xEF\xBB\xBF")) {
                rawLine.erase(0, 3);
            }
        }
        const std::wstring line = trimText(utf8ToWide(rawLine));
        if (line.empty() || line.front() == L'#') {
            return;
        }
        const auto pos = line.find(L'=');
        if (pos != std::wstring::npos) {
            values[trimText(line.substr(0, pos))] = trimText(line.substr(pos + 1));
        }
    };
    std::array<char, 4096> buffer;
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(input.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
            LogContext::breadcrumb(L"settings read failed (Win32 error " + std::to_wstring(GetLastError()) + L")");
            return sanitizeSettings(settings);
        }
        if (count == 0) {
            break;
        }
        for (DWORD index = 0; index < count; ++index) {
            const char ch = buffer[index];
            if (ch == '\n') {
                if (!discardingLine) {
                    parseLine();
                }
                discardingLine = false;
                rawLine.clear();
            } else if (!discardingLine) {
                if (rawLine.size() >= kMaxLineBytes) {
                    discardingLine = true;
                    rawLine.clear();
                    Logger::instance().warning(L"settings",
                        L"settings line longer than 65536 bytes dropped");
                } else {
                    rawLine.push_back(ch);
                }
            }
        }
    }
    if (!rawLine.empty()) {
        parseLine();
    }
    if (status) {
        *status = LoadStatus::Loaded;
    }

    settings.video.width = readUInt(values, L"video.width", settings.video.width);
    settings.video.height = readUInt(values, L"video.height", settings.video.height);
    settings.video.fps = readUInt(values, L"video.fps", settings.video.fps);
    settings.video.bitrateKbps = readUInt(values, L"video.bitrateKbps", settings.video.bitrateKbps);
    settings.video.gopSeconds = readUInt(values, L"video.gopSeconds", settings.video.gopSeconds);
    settings.video.codec = codecFromName(readString(values, L"video.codec", codecName(settings.video.codec)));
    settings.video.resolutionMode = resolutionModeFromName(readString(values, L"video.resolutionMode", resolutionModeName(settings.video.resolutionMode)));
    settings.video.encoderPreset = encoderPresetFromName(readString(values, L"video.encoderPreset", encoderPresetName(settings.video.encoderPreset)));
    settings.video.encoderMode = encoderModeFromName(readString(values, L"video.encoderMode", encoderModeName(settings.video.encoderMode)));
    settings.video.encoderProfile = encoderProfileFromName(readString(values, L"video.encoderProfile", encoderProfileName(settings.video.encoderProfile)));
    settings.video.encoderLookahead = readBool(values, L"video.encoderLookahead", settings.video.encoderLookahead);
    settings.video.encoderLookaheadDepth = readUInt(values, L"video.encoderLookaheadDepth", settings.video.encoderLookaheadDepth);
    settings.video.encoderSpatialAQ = readBool(values, L"video.encoderSpatialAQ", settings.video.encoderSpatialAQ);
    settings.video.encoderAQStrength = readUInt(values, L"video.encoderAQStrength", settings.video.encoderAQStrength);
    settings.video.encoderTemporalAQ = readBool(values, L"video.encoderTemporalAQ", settings.video.encoderTemporalAQ);
    settings.video.encoderMultipass = encoderMultipassFromName(readString(values, L"video.encoderMultipass", encoderMultipassName(settings.video.encoderMultipass)));
    settings.video.encoderBFrames = readBool(values, L"video.encoderBFrames", settings.video.encoderBFrames);
    settings.video.encoderAdaptiveBFrames = readBool(values, L"video.encoderAdaptiveBFrames", settings.video.encoderAdaptiveBFrames);
    settings.video.encoderAdaptiveIFrames = readBool(values, L"video.encoderAdaptiveIFrames", settings.video.encoderAdaptiveIFrames);
    settings.video.encoderZeroReorderDelay = readBool(values, L"video.encoderZeroReorderDelay", settings.video.encoderZeroReorderDelay);
    settings.video.encoderReferenceFrames = readUInt(values, L"video.encoderReferenceFrames", settings.video.encoderReferenceFrames);
    settings.replay.enabled = readBool(values, L"replay.enabled", settings.replay.enabled);
    settings.replay.seconds = readUInt(values, L"replay.seconds", settings.replay.seconds);
    settings.replay.overlapSeconds = readUInt(values, L"replay.overlapSeconds", settings.replay.overlapSeconds);
    settings.replay.diskBackedVideo = readBool(values, L"replay.diskBackedVideo", settings.replay.diskBackedVideo);
    settings.replay.diskBackedAudio = readBool(values, L"replay.diskBackedAudio", settings.replay.diskBackedAudio);
    settings.replayRingDirectory = readString(values, L"replay.ringDirectory", settings.replayRingDirectory.wstring());
    const bool legacyLeagueKillClips =
        readBool(values, L"integrations.leagueOfLegends.autoKillClips", settings.gameIntegrations.leagueOfLegendsKillReminder);
    settings.gameIntegrations.leagueOfLegendsKillReminder =
        readBool(values, L"integrations.leagueOfLegends.killReminder", legacyLeagueKillClips);
    settings.gpu.adaptiveMode = gpuAdaptiveModeFromName(readString(values, L"gpu.adaptiveMode", gpuAdaptiveModeName(settings.gpu.adaptiveMode)));
    settings.gpu.wgcZeroCopy = readBool(values, L"gpu.wgcZeroCopy", settings.gpu.wgcZeroCopy);
    settings.gpu.frameQueueLimit = readUInt(values, L"gpu.frameQueueLimit", settings.gpu.frameQueueLimit);
    settings.gpu.allowIdleFrameSkipping =
        readBool(values, L"gpu.losslessIdleCoalescing", settings.gpu.allowIdleFrameSkipping);
    settings.gpu.stableMultimonitorFrames =
        readBool(values, L"gpu.stableMultimonitorFrames", settings.gpu.stableMultimonitorFrames);
    settings.captureMicrophone = readBool(values, L"audio.microphone", settings.captureMicrophone);
    settings.captureSystemAudio = readBool(values, L"audio.system", settings.captureSystemAudio);
    settings.audioInputDeviceId = readString(values, L"audio.inputDeviceId", settings.audioInputDeviceId);
    settings.audioOutputDeviceId = readString(values, L"audio.outputDeviceId", settings.audioOutputDeviceId);
    settings.audioInputVolumePercent = readUInt(values, L"audio.inputVolumePercent", settings.audioInputVolumePercent);
    settings.audioOutputVolumePercent = readUInt(values, L"audio.outputVolumePercent", settings.audioOutputVolumePercent);
    settings.audioInputAutoGainEnabled = readBool(values, L"audio.inputAutoGain", settings.audioInputAutoGainEnabled);
#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
    settings.audioInputNoiseSuppressionEnabled = readBool(values, L"audio.inputNoiseSuppression", settings.audioInputNoiseSuppressionEnabled);
#endif
    settings.soundSeparationEnabled = readBool(values, L"audio.soundSeparation.enabled", settings.soundSeparationEnabled);
    const auto readAppList = [&values](const std::wstring& listKey, auto& apps, auto loadEntry) {
        // count is only a hint: normalizeLoaded must not erase entries without it.
        const uint32_t count = std::min<uint32_t>(
            readUInt(values, listKey + L".count", 0), kMaxAppListEntries);
        apps.clear();
        apps.reserve(count);
        for (uint32_t index = 0; index < kMaxAppListEntries; ++index) {
            const std::wstring prefix = listKey + L"." + std::to_wstring(index) + L".";
            const bool hasPath = values.contains(prefix + L"path");
            if (index >= count && !hasPath) {
                break;
            }
            apps.push_back(loadEntry(prefix));
        }
    };
    readAppList(L"audio.soundSeparation", settings.soundSeparationApps,
        [&values](const std::wstring& prefix) {
            return AppSettings::SoundSeparationApp{
                readString(values, prefix + L"name", {}),
                readString(values, prefix + L"path", {}),
                readBool(values, prefix + L"muted", true)};
        });
    settings.autoRecordEnabled = readBool(values, L"integrations.autoRecord.enabled", settings.autoRecordEnabled);
    readAppList(L"integrations.autoRecord", settings.autoRecordApps,
        [&values](const std::wstring& prefix) {
            return AppSettings::AutoRecordApp{
                readString(values, prefix + L"name", {}),
                readString(values, prefix + L"path", {})};
        });
    settings.startWithWindowsMinimized = readBool(values, L"general.startWithWindowsMinimized", settings.startWithWindowsMinimized);
    settings.pruneStaleMicrophoneConsentEntries =
        readBool(values, L"general.pruneStaleMicrophoneConsentEntries", settings.pruneStaleMicrophoneConsentEntries);
    settings.exitToTray = readBool(values, L"general.exitToTray", settings.exitToTray);
    settings.checkForUpdates = readBool(values, L"general.checkForUpdates", settings.checkForUpdates);
    settings.loggingEnabled = readBool(values, L"diagnostics.loggingEnabled", settings.loggingEnabled);
    settings.notificationSoundVolumePercent =
        readUInt(values, L"general.notificationSoundVolumePercent", settings.notificationSoundVolumePercent);
    settings.customNotificationSoundPath =
        readString(values, L"general.customNotificationSoundPath", settings.customNotificationSoundPath.wstring());
    settings.monitorIndex = readUInt(values, L"capture.monitorIndex", settings.monitorIndex);
    settings.multiMonitorSupport = readBool(values, L"capture.multiMonitorSupport", settings.multiMonitorSupport);
    settings.followFocusedMonitor = readBool(values, L"capture.followFocusedMonitor", settings.followFocusedMonitor);
    settings.followMouseMonitor = readBool(values, L"capture.followMouseMonitor", settings.followMouseMonitor);
    settings.captureCursor = readBool(values, L"capture.cursor", settings.captureCursor);
    settings.preferredCaptureBackend = captureBackendFromName(
        readString(values, L"capture.backend", captureBackendName(settings.preferredCaptureBackend)));
    settings.clipDirectory = readString(values, L"clips.directory", settings.clipDirectory.wstring());
    settings.organizeClipsByGame = readBool(values, L"clips.organizeByGame", settings.organizeClipsByGame);
    settings.libraryGalleryView = readLibraryGalleryView(values, settings.libraryGalleryView);
    settings.hotkeys.startStopModifiers = readUInt(values, L"hotkeys.startStop.modifiers", settings.hotkeys.startStopModifiers);
    settings.hotkeys.startStopVirtualKey = readUInt(values, L"hotkeys.startStop.vk", settings.hotkeys.startStopVirtualKey);
    settings.hotkeys.saveReplayModifiers = readUInt(values, L"hotkeys.saveReplay.modifiers", settings.hotkeys.saveReplayModifiers);
    settings.hotkeys.saveReplayVirtualKey = readUInt(values, L"hotkeys.saveReplay.vk", settings.hotkeys.saveReplayVirtualKey);
    settings.gameIntegrations.voiceCommand = voiceCommandModeFromName(
        readString(values, L"integrations.voiceCommand", voiceCommandModeName(settings.gameIntegrations.voiceCommand)));
    {
        const std::wstring raw = foldName(readString(
            values,
            L"integrations.discordRichPresence",
            discordRichPresenceModeName(settings.gameIntegrations.discordRichPresence)));
        // Legacy bool values from older settings files: 1/true -> Always, 0/false -> Off.
        if (raw == L"1" || raw == L"true") {
            settings.gameIntegrations.discordRichPresence =
                GameIntegrationSettings::DiscordRichPresenceMode::Always;
        } else if (raw == L"0" || raw == L"false") {
            settings.gameIntegrations.discordRichPresence =
                GameIntegrationSettings::DiscordRichPresenceMode::Off;
        } else {
            settings.gameIntegrations.discordRichPresence = discordRichPresenceModeFromName(raw);
        }
    }
    settings.gameIntegrations.discordRichPresenceResetTimerOnSwitch = readBool(
        values,
        L"integrations.discordRichPresenceResetTimerOnSwitch",
        settings.gameIntegrations.discordRichPresenceResetTimerOnSwitch);

    return sanitizeSettings(settings);
}

bool SettingsStore::save(const AppSettings& settings) const {
    const AppSettings normalized = sanitizeSettings(settings);
    if (!path_.parent_path().empty()) {
        std::error_code error;
        std::filesystem::create_directories(path_.parent_path(), error);
        if (error) {
            Logger::instance().error(L"settings", L"Could not create settings directory: " + path_.parent_path().wstring());
            return false;
        }
    }

    std::wstringstream output;
    output << L"# Backtrack settings\n";
    output << L"video.width=" << normalized.video.width << L"\n";
    output << L"video.height=" << normalized.video.height << L"\n";
    output << L"video.fps=" << normalized.video.fps << L"\n";
    output << L"video.bitrateKbps=" << normalized.video.bitrateKbps << L"\n";
    output << L"video.gopSeconds=" << normalized.video.gopSeconds << L"\n";
    output << L"video.codec=" << codecName(normalized.video.codec) << L"\n";
    output << L"video.resolutionMode=" << resolutionModeName(normalized.video.resolutionMode) << L"\n";
    output << L"video.encoderPreset=" << encoderPresetName(normalized.video.encoderPreset) << L"\n";
    output << L"video.encoderMode=" << encoderModeName(normalized.video.encoderMode) << L"\n";
    output << L"video.encoderProfile=" << encoderProfileName(normalized.video.encoderProfile) << L"\n";
    output << L"video.encoderLookahead=" << (normalized.video.encoderLookahead ? 1 : 0) << L"\n";
    output << L"video.encoderLookaheadDepth=" << normalized.video.encoderLookaheadDepth << L"\n";
    output << L"video.encoderSpatialAQ=" << (normalized.video.encoderSpatialAQ ? 1 : 0) << L"\n";
    output << L"video.encoderAQStrength=" << normalized.video.encoderAQStrength << L"\n";
    output << L"video.encoderTemporalAQ=" << (normalized.video.encoderTemporalAQ ? 1 : 0) << L"\n";
    output << L"video.encoderMultipass=" << encoderMultipassName(normalized.video.encoderMultipass) << L"\n";
    output << L"video.encoderBFrames=" << (normalized.video.encoderBFrames ? 1 : 0) << L"\n";
    output << L"video.encoderAdaptiveBFrames=" << (normalized.video.encoderAdaptiveBFrames ? 1 : 0) << L"\n";
    output << L"video.encoderAdaptiveIFrames=" << (normalized.video.encoderAdaptiveIFrames ? 1 : 0) << L"\n";
    output << L"video.encoderZeroReorderDelay=" << (normalized.video.encoderZeroReorderDelay ? 1 : 0) << L"\n";
    output << L"video.encoderReferenceFrames=" << normalized.video.encoderReferenceFrames << L"\n";
    output << L"replay.enabled=" << (normalized.replay.enabled ? 1 : 0) << L"\n";
    output << L"replay.seconds=" << normalized.replay.seconds << L"\n";
    output << L"replay.overlapSeconds=" << normalized.replay.overlapSeconds << L"\n";
    output << L"replay.diskBackedVideo=" << (normalized.replay.diskBackedVideo ? 1 : 0) << L"\n";
    output << L"replay.diskBackedAudio=" << (normalized.replay.diskBackedAudio ? 1 : 0) << L"\n";
    output << L"replay.ringDirectory=" << normalized.replayRingDirectory.wstring() << L"\n";
    output << L"integrations.leagueOfLegends.killReminder=" << (normalized.gameIntegrations.leagueOfLegendsKillReminder ? 1 : 0) << L"\n";
    output << L"integrations.voiceCommand=" << voiceCommandModeName(normalized.gameIntegrations.voiceCommand) << L"\n";
    output << L"integrations.discordRichPresence="
           << discordRichPresenceModeName(normalized.gameIntegrations.discordRichPresence) << L"\n";
    output << L"integrations.discordRichPresenceResetTimerOnSwitch="
           << (normalized.gameIntegrations.discordRichPresenceResetTimerOnSwitch ? 1 : 0) << L"\n";
    output << L"gpu.adaptiveMode=" << gpuAdaptiveModeName(normalized.gpu.adaptiveMode) << L"\n";
    output << L"gpu.wgcZeroCopy=" << (normalized.gpu.wgcZeroCopy ? 1 : 0) << L"\n";
    output << L"gpu.frameQueueLimit=" << normalized.gpu.frameQueueLimit << L"\n";
    output << L"gpu.losslessIdleCoalescing=" << (normalized.gpu.allowIdleFrameSkipping ? 1 : 0) << L"\n";
    output << L"gpu.stableMultimonitorFrames=" << (normalized.gpu.stableMultimonitorFrames ? 1 : 0) << L"\n";
    output << L"audio.microphone=" << (normalized.captureMicrophone ? 1 : 0) << L"\n";
    output << L"audio.system=" << (normalized.captureSystemAudio ? 1 : 0) << L"\n";
    output << L"audio.inputDeviceId=" << normalized.audioInputDeviceId << L"\n";
    output << L"audio.outputDeviceId=" << normalized.audioOutputDeviceId << L"\n";
    output << L"audio.inputVolumePercent=" << normalized.audioInputVolumePercent << L"\n";
    output << L"audio.outputVolumePercent=" << normalized.audioOutputVolumePercent << L"\n";
    output << L"audio.inputAutoGain=" << (normalized.audioInputAutoGainEnabled ? 1 : 0) << L"\n";
#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
    output << L"audio.inputNoiseSuppression=" << (normalized.audioInputNoiseSuppressionEnabled ? 1 : 0) << L"\n";
#endif
    output << L"audio.soundSeparation.enabled=" << (normalized.soundSeparationEnabled ? 1 : 0) << L"\n";
    output << L"audio.soundSeparation.count=" << normalized.soundSeparationApps.size() << L"\n";
    for (size_t index = 0; index < normalized.soundSeparationApps.size(); ++index) {
        const auto& app = normalized.soundSeparationApps[index];
        const std::wstring prefix = L"audio.soundSeparation." + std::to_wstring(index) + L".";
        output << prefix << L"name=" << app.name << L"\n";
        output << prefix << L"path=" << app.executablePath.wstring() << L"\n";
        output << prefix << L"muted=" << (app.muted ? 1 : 0) << L"\n";
    }
    output << L"integrations.autoRecord.enabled=" << (normalized.autoRecordEnabled ? 1 : 0) << L"\n";
    output << L"integrations.autoRecord.count=" << normalized.autoRecordApps.size() << L"\n";
    for (size_t index = 0; index < normalized.autoRecordApps.size(); ++index) {
        const auto& app = normalized.autoRecordApps[index];
        const std::wstring prefix = L"integrations.autoRecord." + std::to_wstring(index) + L".";
        output << prefix << L"name=" << app.name << L"\n";
        output << prefix << L"path=" << app.executablePath.wstring() << L"\n";
    }
    output << L"general.startWithWindowsMinimized=" << (normalized.startWithWindowsMinimized ? 1 : 0) << L"\n";
    output << L"general.pruneStaleMicrophoneConsentEntries=" << (normalized.pruneStaleMicrophoneConsentEntries ? 1 : 0) << L"\n";
    output << L"general.exitToTray=" << (normalized.exitToTray ? 1 : 0) << L"\n";
    output << L"general.checkForUpdates=" << (normalized.checkForUpdates ? 1 : 0) << L"\n";
    output << L"diagnostics.loggingEnabled=" << (normalized.loggingEnabled ? 1 : 0) << L"\n";
    output << L"general.notificationSoundVolumePercent=" << normalized.notificationSoundVolumePercent << L"\n";
    output << L"general.customNotificationSoundPath=" << normalized.customNotificationSoundPath.wstring() << L"\n";
    output << L"capture.monitorIndex=" << normalized.monitorIndex << L"\n";
    output << L"capture.multiMonitorSupport=" << (normalized.multiMonitorSupport ? 1 : 0) << L"\n";
    output << L"capture.followFocusedMonitor=" << (normalized.followFocusedMonitor ? 1 : 0) << L"\n";
    output << L"capture.followMouseMonitor=" << (normalized.followMouseMonitor ? 1 : 0) << L"\n";
    output << L"capture.cursor=" << (normalized.captureCursor ? 1 : 0) << L"\n";
    output << L"capture.backend=" << captureBackendName(normalized.preferredCaptureBackend) << L"\n";
    output << L"clips.directory=" << normalized.clipDirectory.wstring() << L"\n";
    output << L"clips.organizeByGame=" << (normalized.organizeClipsByGame ? 1 : 0) << L"\n";
    output << L"library.viewMode=" << (normalized.libraryGalleryView ? L"gallery" : L"list") << L"\n";
    output << L"hotkeys.startStop.modifiers=" << normalized.hotkeys.startStopModifiers << L"\n";
    output << L"hotkeys.startStop.vk=" << normalized.hotkeys.startStopVirtualKey << L"\n";
    output << L"hotkeys.saveReplay.modifiers=" << normalized.hotkeys.saveReplayModifiers << L"\n";
    output << L"hotkeys.saveReplay.vk=" << normalized.hotkeys.saveReplayVirtualKey << L"\n";
    if (!output) {
        Logger::instance().error(L"settings", L"Could not serialize settings");
        return false;
    }

    // Wrap the publish in a slow-path timer; save runs on the UI thread and a
    // cold filesystem should not make a setting change feel hung.
    const ScopedLatencyWarn publishSlow(L"settings", L"Settings publish slow", std::chrono::milliseconds(500));
    const auto error = atomicWriteSibling(path_, wideToUtf8(output.str()));
    if (error) {
        Logger::instance().error(L"settings", L"Could not publish settings file, previous settings kept: " +
            path_.wstring() + L" (Win32 error " + std::to_wstring(error.value()) + L")");
        return false;
    }
    Logger::instance().info(L"settings", L"settings saved");
    return true;
}

VideoSettings validateEncoderConfig(VideoSettings video) {
    video.width = clampEven(video.width, 16, 7680);
    video.height = clampEven(video.height, 16, 4320);
    video.fps = std::clamp<uint32_t>(video.fps, 1, 240);
    video.bitrateKbps = std::clamp<uint32_t>(video.bitrateKbps, 1000, 200000);
    video.gopSeconds = std::clamp<uint32_t>(video.gopSeconds, 1, 30);
    video.encoderLookaheadDepth = std::min<uint32_t>(video.encoderLookaheadDepth, 31);
    video.encoderAQStrength = std::clamp<uint32_t>(video.encoderAQStrength, 1, 15);
    video.encoderReferenceFrames = std::clamp<uint32_t>(video.encoderReferenceFrames, 1, 4);
    if (!video.encoderLookahead) {
        video.encoderAdaptiveBFrames = false;
        video.encoderAdaptiveIFrames = false;
    }
    if (!video.encoderBFrames) {
        video.encoderAdaptiveBFrames = false;
    }
    return video;
}

AppSettings sanitizeSettings(AppSettings settings) {
    uint32_t presetWidth = 0;
    uint32_t presetHeight = 0;
    if (resolutionPresetSize(settings.video.resolutionMode, presetWidth, presetHeight)) {
        settings.video.width = presetWidth;
        settings.video.height = presetHeight;
    }
    settings.video = validateEncoderConfig(settings.video);
    settings.replay.seconds = std::clamp<uint32_t>(settings.replay.seconds, 1, 3600);
    settings.replay.overlapSeconds = std::clamp<uint32_t>(settings.replay.overlapSeconds, 0, 3600);
    settings.gpu.frameQueueLimit = std::clamp<uint32_t>(settings.gpu.frameQueueLimit, 1, 8);
    settings.audioInputVolumePercent = std::clamp<uint32_t>(settings.audioInputVolumePercent, 0, 200);
    settings.audioOutputVolumePercent = std::clamp<uint32_t>(settings.audioOutputVolumePercent, 0, 200);
    settings.notificationSoundVolumePercent = std::clamp<uint32_t>(settings.notificationSoundVolumePercent, 0, 100);
    dedupeNamedExecutableApps(settings.soundSeparationApps);
    dedupeNamedExecutableApps(settings.autoRecordApps);
    settings.hotkeys.startStopModifiers = supportedHotkeyModifiers(settings.hotkeys.startStopModifiers);
    settings.hotkeys.saveReplayModifiers = supportedHotkeyModifiers(settings.hotkeys.saveReplayModifiers);
    const HotkeySettings defaultHotkeys;
    if (!isSafeGlobalHotkey(settings.hotkeys.startStopModifiers, settings.hotkeys.startStopVirtualKey)) {
        settings.hotkeys.startStopModifiers = defaultHotkeys.startStopModifiers;
        settings.hotkeys.startStopVirtualKey = defaultHotkeys.startStopVirtualKey;
    }
    if (!isSafeGlobalHotkey(settings.hotkeys.saveReplayModifiers, settings.hotkeys.saveReplayVirtualKey) ||
        (settings.hotkeys.saveReplayVirtualKey != 0 &&
         settings.hotkeys.saveReplayModifiers == settings.hotkeys.startStopModifiers &&
         settings.hotkeys.saveReplayVirtualKey == settings.hotkeys.startStopVirtualKey)) {
        settings.hotkeys.saveReplayModifiers = defaultHotkeys.saveReplayModifiers;
        settings.hotkeys.saveReplayVirtualKey = defaultHotkeys.saveReplayVirtualKey;
        if (settings.hotkeys.saveReplayModifiers == settings.hotkeys.startStopModifiers &&
            settings.hotkeys.saveReplayVirtualKey == settings.hotkeys.startStopVirtualKey) {
            settings.hotkeys.saveReplayModifiers = 0;
            settings.hotkeys.saveReplayVirtualKey = 0;
        }
    }
    if (!settings.multiMonitorSupport || !settings.followFocusedMonitor) {
        settings.followMouseMonitor = false;
    }
    if (settings.clipDirectory.empty()) {
        settings.clipDirectory = defaultClipDirectory();
    }
    return settings;
}

VideoCodec codecFromName(const std::wstring& value) {
    const std::wstring name = foldName(value);
    if (name == L"hevc" || name == L"h265") {
        return VideoCodec::Hevc;
    }
    if (name == L"h264") {
        return VideoCodec::H264;
    }
    return VideoCodec::H264; // fallback for unknown names
}

const wchar_t* resolutionModeName(ResolutionMode mode) {
    for (const auto& preset : kResolutionPresets) {
        if (preset.mode == mode) {
            return preset.name;
        }
    }
    return L"1080p";
}

ResolutionMode resolutionModeFromName(const std::wstring& value) {
    const std::wstring name = foldName(value);
    for (const auto& preset : kResolutionPresets) {
        if (name == preset.name) {
            return preset.mode;
        }
    }
    if (name == L"1440p") {
        return ResolutionMode::P2K;
    }
    if (name == L"2160p") {
        return ResolutionMode::P4K;
    }
    // Legacy "native" mode was removed. Preserve the persisted width/height by
    // migrating to Custom (which keeps the explicit size) rather than a preset.
    if (name == L"native") {
        return ResolutionMode::Custom;
    }
    return ResolutionMode::P1080; // fallback for unknown names
}

EncoderPreset encoderPresetFromName(const std::wstring& value) {
    const std::wstring name = foldName(value);
    if (name == L"p1") {
        return EncoderPreset::P1;
    }
    if (name == L"p2") {
        return EncoderPreset::P2;
    }
    if (name == L"p3") {
        return EncoderPreset::P3;
    }
    if (name == L"p4") {
        return EncoderPreset::P4;
    }
    if (name == L"p5") {
        return EncoderPreset::P5;
    }
    if (name == L"p6") {
        return EncoderPreset::P6;
    }
    if (name == L"p7") {
        return EncoderPreset::P7;
    }
    return EncoderPreset::P4; // fallback for unknown names
}

EncoderMode encoderModeFromName(const std::wstring& value) {
    const std::wstring name = foldName(value);
    if (name == L"low-latency" || name == L"low_latency") {
        return EncoderMode::LowLatency;
    }
    if (name == L"ultra-low-latency" || name == L"ultra_low_latency") {
        return EncoderMode::UltraLowLatency;
    }
    if (name == L"lossless") {
        return EncoderMode::Lossless;
    }
    if (name == L"ultra-high-quality" || name == L"ultra_high_quality") {
        return EncoderMode::UltraHighQuality;
    }
    if (name == L"high-quality") {
        return EncoderMode::HighQuality;
    }
    return EncoderMode::HighQuality; // fallback for unknown names
}

const wchar_t* encoderProfileName(EncoderProfile profile) {
    switch (profile) {
    case EncoderProfile::LowestGpu:
        return L"lowest-gpu";
    case EncoderProfile::Balanced:
        return L"balanced";
    case EncoderProfile::Custom:
        return L"custom";
    }
    return L"lowest-gpu";
}

EncoderProfile encoderProfileFromName(const std::wstring& value) {
    const std::wstring name = foldName(value);
    if (name == L"balanced") {
        return EncoderProfile::Balanced;
    }
    if (name == L"custom") {
        return EncoderProfile::Custom;
    }
    if (name == L"lowest-gpu") {
        return EncoderProfile::LowestGpu;
    }
    return EncoderProfile::LowestGpu; // fallback for unknown names
}

const wchar_t* encoderMultipassName(EncoderMultipass multipass) {
    switch (multipass) {
    case EncoderMultipass::Disabled:
        return L"disabled";
    case EncoderMultipass::QuarterResolution:
        return L"quarter-resolution";
    case EncoderMultipass::FullResolution:
        return L"full-resolution";
    }
    return L"disabled";
}

EncoderMultipass encoderMultipassFromName(const std::wstring& value) {
    const std::wstring name = foldName(value);
    if (name == L"quarter-resolution" || name == L"quarter") {
        return EncoderMultipass::QuarterResolution;
    }
    if (name == L"full-resolution" || name == L"full") {
        return EncoderMultipass::FullResolution;
    }
    if (name == L"disabled") {
        return EncoderMultipass::Disabled;
    }
    return EncoderMultipass::Disabled; // fallback for unknown names
}

const wchar_t* gpuAdaptiveModeName(GpuAdaptiveMode mode) {
    switch (mode) {
    case GpuAdaptiveMode::Disabled:
        return L"disabled";
    case GpuAdaptiveMode::Conservative:
        return L"conservative";
    case GpuAdaptiveMode::Aggressive:
        return L"aggressive";
    }
    return L"conservative";
}

GpuAdaptiveMode gpuAdaptiveModeFromName(const std::wstring& value) {
    const std::wstring name = foldName(value);
    if (name == L"disabled" || name == L"off") {
        return GpuAdaptiveMode::Disabled;
    }
    if (name == L"aggressive") {
        return GpuAdaptiveMode::Aggressive;
    }
    if (name == L"conservative") {
        return GpuAdaptiveMode::Conservative;
    }
    return GpuAdaptiveMode::Conservative; // fallback for unknown names
}

const wchar_t* captureBackendName(CaptureBackend backend) {
    switch (backend) {
    case CaptureBackend::WindowsGraphicsCapture:
        return L"wgc";
    case CaptureBackend::DesktopDuplication:
        return L"desktop-duplication";
    }
    return L"wgc";
}

CaptureBackend captureBackendFromName(const std::wstring& value) {
    const std::wstring name = foldName(value);
    if (name == L"desktop-duplication" || name == L"dda" || name == L"dxgi") {
        return CaptureBackend::DesktopDuplication;
    }
    if (name == L"wgc") {
        return CaptureBackend::WindowsGraphicsCapture;
    }
    // Legacy "game-capture" values fall back to WGC; the injected game-capture
    // DLL no longer exists in the codebase.
    return CaptureBackend::WindowsGraphicsCapture; // fallback for unknown names
}

const wchar_t* voiceCommandModeName(GameIntegrationSettings::VoiceCommandMode mode) {
    switch (mode) {
    case GameIntegrationSettings::VoiceCommandMode::Disabled:
        return L"disabled";
    case GameIntegrationSettings::VoiceCommandMode::ClipThat:
        return L"clip-that";
    case GameIntegrationSettings::VoiceCommandMode::RecordVideo:
        return L"record-video";
    }
    return L"disabled";
}

GameIntegrationSettings::VoiceCommandMode voiceCommandModeFromName(const std::wstring& value) {
    const std::wstring name = foldName(value);
    if (name == L"clip-that") {
        return GameIntegrationSettings::VoiceCommandMode::ClipThat;
    }
    if (name == L"record-video") {
        return GameIntegrationSettings::VoiceCommandMode::RecordVideo;
    }
    if (name == L"disabled") {
        return GameIntegrationSettings::VoiceCommandMode::Disabled;
    }
    return GameIntegrationSettings::VoiceCommandMode::Disabled; // fallback for unknown names
}

const wchar_t* discordRichPresenceModeName(GameIntegrationSettings::DiscordRichPresenceMode mode) {
    switch (mode) {
    case GameIntegrationSettings::DiscordRichPresenceMode::Always:
        return L"always";
    case GameIntegrationSettings::DiscordRichPresenceMode::FullscreenOnly:
        return L"fullscreen-only";
    case GameIntegrationSettings::DiscordRichPresenceMode::Off:
        return L"off";
    }
    return L"off";
}

GameIntegrationSettings::DiscordRichPresenceMode discordRichPresenceModeFromName(const std::wstring& value) {
    const std::wstring name = foldName(value);
    if (name == L"always") {
        return GameIntegrationSettings::DiscordRichPresenceMode::Always;
    }
    if (name == L"fullscreen-only") {
        return GameIntegrationSettings::DiscordRichPresenceMode::FullscreenOnly;
    }
    if (name == L"off") {
        return GameIntegrationSettings::DiscordRichPresenceMode::Off;
    }
    return GameIntegrationSettings::DiscordRichPresenceMode::Off; // fallback for unknown names
}

} // namespace backtrack

