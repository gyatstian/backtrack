#include "settings/SettingsStore.h"

#include "core/Logger.h"
#include "platform/Win32Util.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <unordered_set>

namespace backtrack {

namespace {

std::wstring trim(std::wstring value) {
    while (!value.empty() && iswspace(value.front())) {
        value.erase(value.begin());
    }
    while (!value.empty() && iswspace(value.back())) {
        value.pop_back();
    }
    return value;
}

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

std::wstring normalizedPathKey(const std::filesystem::path& path) {
    std::wstring value = path.wstring();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return value;
}

bool readBool(const std::map<std::wstring, std::wstring>& values, const std::wstring& key, bool fallback) {
    const auto it = values.find(key);
    if (it == values.end()) {
        return fallback;
    }
    auto value = it->second;
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
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
        const auto& mode = modeIt->second;
        if (mode == L"gallery" || mode == L"Gallery" || mode == L"GALLERY") {
            return true;
        }
        if (mode == L"list" || mode == L"List" || mode == L"LIST") {
            return false;
        }
    }
    return readBool(values, L"library.galleryView", fallback);
}

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

uint32_t clampEven(uint32_t value, uint32_t minimum, uint32_t maximum) {
    value = std::clamp(value, minimum, maximum);
    return (value % 2) == 0 ? value : value - 1;
}

bool resolutionPresetSize(ResolutionMode mode, uint32_t& width, uint32_t& height) {
    switch (mode) {
    case ResolutionMode::P240:
        width = 426;
        height = 240;
        return true;
    case ResolutionMode::P480:
        width = 854;
        height = 480;
        return true;
    case ResolutionMode::P720:
        width = 1280;
        height = 720;
        return true;
    case ResolutionMode::P1080:
        width = 1920;
        height = 1080;
        return true;
    case ResolutionMode::P2K:
        width = 2560;
        height = 1440;
        return true;
    case ResolutionMode::P4K:
        width = 3840;
        height = 2160;
        return true;
    case ResolutionMode::Native:
    case ResolutionMode::Custom:
        break;
    }
    return false;
}

} // namespace

SettingsStore::SettingsStore(std::filesystem::path path)
    : path_(std::move(path)) {
}

AppSettings SettingsStore::load() const {
    AppSettings settings;
    settings.clipDirectory = defaultClipDirectory();

    std::ifstream input(path_, std::ios::binary);
    if (!input.is_open()) {
        return sanitizeSettings(settings);
    }

    std::map<std::wstring, std::wstring> values;
    std::string rawLine;
    std::wstring line;
    while (std::getline(input, rawLine)) {
        if (!rawLine.empty() && rawLine.back() == '\r') {
            rawLine.pop_back();
        }
        line = utf8ToWide(rawLine);
        line = trim(line);
        if (line.empty() || line.front() == L'#') {
            continue;
        }
        const auto pos = line.find(L'=');
        if (pos == std::wstring::npos) {
            continue;
        }
        values[trim(line.substr(0, pos))] = trim(line.substr(pos + 1));
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
    settings.soundSeparationEnabled = readBool(values, L"audio.soundSeparation.enabled", settings.soundSeparationEnabled);
    const uint32_t soundSeparationCount = std::min<uint32_t>(
        readUInt(values, L"audio.soundSeparation.count", 0),
        128);
    settings.soundSeparationApps.clear();
    settings.soundSeparationApps.reserve(soundSeparationCount);
    for (uint32_t index = 0; index < soundSeparationCount; ++index) {
        const std::wstring prefix = L"audio.soundSeparation." + std::to_wstring(index) + L".";
        AppSettings::SoundSeparationApp app;
        app.name = readString(values, prefix + L"name", {});
        app.executablePath = readString(values, prefix + L"path", {});
        app.muted = readBool(values, prefix + L"muted", true);
        settings.soundSeparationApps.push_back(std::move(app));
    }
    settings.startWithWindowsMinimized = readBool(values, L"general.startWithWindowsMinimized", settings.startWithWindowsMinimized);
    settings.exitToTray = readBool(values, L"general.exitToTray", settings.exitToTray);
    settings.notificationSoundVolumePercent =
        readUInt(values, L"general.notificationSoundVolumePercent", settings.notificationSoundVolumePercent);
    settings.monitorIndex = readUInt(values, L"capture.monitorIndex", settings.monitorIndex);
    settings.followFocusedMonitor = readBool(values, L"capture.followFocusedMonitor", settings.followFocusedMonitor);
    settings.followMouseMonitor = readBool(values, L"capture.followMouseMonitor", settings.followMouseMonitor);
    settings.captureCursor = readBool(values, L"capture.cursor", settings.captureCursor);
    settings.preferredCaptureBackend = captureBackendFromName(
        readString(values, L"capture.backend", captureBackendName(settings.preferredCaptureBackend)));
    settings.clipDirectory = readString(values, L"clips.directory", settings.clipDirectory.wstring());
    settings.libraryGalleryView = readLibraryGalleryView(values, settings.libraryGalleryView);
    settings.hotkeys.startStopModifiers = readUInt(values, L"hotkeys.startStop.modifiers", settings.hotkeys.startStopModifiers);
    settings.hotkeys.startStopVirtualKey = readUInt(values, L"hotkeys.startStop.vk", settings.hotkeys.startStopVirtualKey);
    settings.hotkeys.saveReplayModifiers = readUInt(values, L"hotkeys.saveReplay.modifiers", settings.hotkeys.saveReplayModifiers);
    settings.hotkeys.saveReplayVirtualKey = readUInt(values, L"hotkeys.saveReplay.vk", settings.hotkeys.saveReplayVirtualKey);

    return sanitizeSettings(settings);
}

void SettingsStore::save(const AppSettings& settings) const {
    const AppSettings normalized = sanitizeSettings(settings);
    if (!path_.parent_path().empty()) {
        std::error_code error;
        std::filesystem::create_directories(path_.parent_path(), error);
        if (error) {
            Logger::instance().error(L"Could not create settings directory: " + path_.parent_path().wstring());
            return;
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
    output << L"integrations.leagueOfLegends.killReminder=" << (normalized.gameIntegrations.leagueOfLegendsKillReminder ? 1 : 0) << L"\n";
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
    output << L"audio.soundSeparation.enabled=" << (normalized.soundSeparationEnabled ? 1 : 0) << L"\n";
    output << L"audio.soundSeparation.count=" << normalized.soundSeparationApps.size() << L"\n";
    for (size_t index = 0; index < normalized.soundSeparationApps.size(); ++index) {
        const auto& app = normalized.soundSeparationApps[index];
        const std::wstring prefix = L"audio.soundSeparation." + std::to_wstring(index) + L".";
        output << prefix << L"name=" << app.name << L"\n";
        output << prefix << L"path=" << app.executablePath.wstring() << L"\n";
        output << prefix << L"muted=" << (app.muted ? 1 : 0) << L"\n";
    }
    output << L"general.startWithWindowsMinimized=" << (normalized.startWithWindowsMinimized ? 1 : 0) << L"\n";
    output << L"general.exitToTray=" << (normalized.exitToTray ? 1 : 0) << L"\n";
    output << L"general.notificationSoundVolumePercent=" << normalized.notificationSoundVolumePercent << L"\n";
    output << L"capture.monitorIndex=" << normalized.monitorIndex << L"\n";
    output << L"capture.followFocusedMonitor=" << (normalized.followFocusedMonitor ? 1 : 0) << L"\n";
    output << L"capture.followMouseMonitor=" << (normalized.followMouseMonitor ? 1 : 0) << L"\n";
    output << L"capture.cursor=" << (normalized.captureCursor ? 1 : 0) << L"\n";
    output << L"capture.backend=" << captureBackendName(normalized.preferredCaptureBackend) << L"\n";
    output << L"clips.directory=" << normalized.clipDirectory.wstring() << L"\n";
    output << L"library.viewMode=" << (normalized.libraryGalleryView ? L"gallery" : L"list") << L"\n";
    output << L"hotkeys.startStop.modifiers=" << normalized.hotkeys.startStopModifiers << L"\n";
    output << L"hotkeys.startStop.vk=" << normalized.hotkeys.startStopVirtualKey << L"\n";
    output << L"hotkeys.saveReplay.modifiers=" << normalized.hotkeys.saveReplayModifiers << L"\n";
    output << L"hotkeys.saveReplay.vk=" << normalized.hotkeys.saveReplayVirtualKey << L"\n";

    std::ofstream file(path_, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        Logger::instance().error(L"Could not open settings file for writing: " + path_.wstring());
        return;
    }
    file << wideToUtf8(output.str());
    if (!file) {
        Logger::instance().error(L"Could not write settings file: " + path_.wstring());
    }
}

AppSettings sanitizeSettings(AppSettings settings) {
    uint32_t presetWidth = 0;
    uint32_t presetHeight = 0;
    if (resolutionPresetSize(settings.video.resolutionMode, presetWidth, presetHeight)) {
        settings.video.width = presetWidth;
        settings.video.height = presetHeight;
    }
    settings.video.width = clampEven(settings.video.width, 16, 7680);
    settings.video.height = clampEven(settings.video.height, 16, 4320);
    settings.video.fps = std::clamp<uint32_t>(settings.video.fps, 1, 240);
    settings.video.bitrateKbps = std::clamp<uint32_t>(settings.video.bitrateKbps, 1000, 200000);
    settings.video.gopSeconds = std::clamp<uint32_t>(settings.video.gopSeconds, 1, 30);
    settings.video.encoderLookaheadDepth = std::min<uint32_t>(settings.video.encoderLookaheadDepth, 31);
    settings.video.encoderAQStrength = std::clamp<uint32_t>(settings.video.encoderAQStrength, 1, 15);
    settings.video.encoderReferenceFrames = std::clamp<uint32_t>(settings.video.encoderReferenceFrames, 1, 4);
    settings.replay.seconds = std::clamp<uint32_t>(settings.replay.seconds, 1, 3600);
    settings.gpu.frameQueueLimit = std::clamp<uint32_t>(settings.gpu.frameQueueLimit, 1, 8);
    settings.audioInputVolumePercent = std::clamp<uint32_t>(settings.audioInputVolumePercent, 0, 200);
    settings.audioOutputVolumePercent = std::clamp<uint32_t>(settings.audioOutputVolumePercent, 0, 200);
    settings.notificationSoundVolumePercent = std::clamp<uint32_t>(settings.notificationSoundVolumePercent, 0, 100);
    std::vector<AppSettings::SoundSeparationApp> soundSeparationApps;
    soundSeparationApps.reserve(settings.soundSeparationApps.size());
    std::unordered_set<std::wstring> seenSoundSeparationPaths;
    for (auto app : settings.soundSeparationApps) {
        if (app.executablePath.empty()) {
            continue;
        }
        app.name = trim(std::move(app.name));
        if (app.name.empty()) {
            app.name = app.executablePath.stem().wstring();
        }
        const std::wstring key = normalizedPathKey(app.executablePath);
        if (key.empty() || seenSoundSeparationPaths.contains(key)) {
            continue;
        }
        seenSoundSeparationPaths.insert(key);
        soundSeparationApps.push_back(std::move(app));
        if (soundSeparationApps.size() >= 128) {
            break;
        }
    }
    settings.soundSeparationApps = std::move(soundSeparationApps);
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
    }
    if (!settings.video.encoderLookahead) {
        settings.video.encoderAdaptiveBFrames = false;
        settings.video.encoderAdaptiveIFrames = false;
    }
    if (!settings.video.encoderBFrames) {
        settings.video.encoderAdaptiveBFrames = false;
    }
    if (!settings.followFocusedMonitor) {
        settings.followMouseMonitor = false;
    }
    if (settings.clipDirectory.empty()) {
        settings.clipDirectory = defaultClipDirectory();
    }
    return settings;
}

const wchar_t* codecName(VideoCodec codec) {
    switch (codec) {
    case VideoCodec::H264:
        return L"h264";
    case VideoCodec::Hevc:
        return L"hevc";
    }
    return L"h264";
}

VideoCodec codecFromName(const std::wstring& value) {
    if (value == L"hevc" || value == L"HEVC" || value == L"h265" || value == L"H265") {
        return VideoCodec::Hevc;
    }
    return VideoCodec::H264;
}

const wchar_t* resolutionModeName(ResolutionMode mode) {
    switch (mode) {
    case ResolutionMode::Native:
        return L"native";
    case ResolutionMode::P240:
        return L"240p";
    case ResolutionMode::P480:
        return L"480p";
    case ResolutionMode::P720:
        return L"720p";
    case ResolutionMode::P1080:
        return L"1080p";
    case ResolutionMode::P2K:
        return L"2k";
    case ResolutionMode::P4K:
        return L"4k";
    case ResolutionMode::Custom:
        return L"custom";
    }
    return L"native";
}

ResolutionMode resolutionModeFromName(const std::wstring& value) {
    if (value == L"240p" || value == L"240P") {
        return ResolutionMode::P240;
    }
    if (value == L"480p" || value == L"480P") {
        return ResolutionMode::P480;
    }
    if (value == L"720p" || value == L"720P") {
        return ResolutionMode::P720;
    }
    if (value == L"1080p" || value == L"1080P") {
        return ResolutionMode::P1080;
    }
    if (value == L"2k" || value == L"2K" || value == L"1440p" || value == L"1440P") {
        return ResolutionMode::P2K;
    }
    if (value == L"4k" || value == L"4K" || value == L"2160p" || value == L"2160P") {
        return ResolutionMode::P4K;
    }
    if (value == L"custom" || value == L"CUSTOM") {
        return ResolutionMode::Custom;
    }
    return ResolutionMode::Native;
}

const wchar_t* encoderPresetName(EncoderPreset preset) {
    switch (preset) {
    case EncoderPreset::P1:
        return L"p1";
    case EncoderPreset::P2:
        return L"p2";
    case EncoderPreset::P3:
        return L"p3";
    case EncoderPreset::P4:
        return L"p4";
    case EncoderPreset::P5:
        return L"p5";
    case EncoderPreset::P6:
        return L"p6";
    case EncoderPreset::P7:
        return L"p7";
    }
    return L"p4";
}

EncoderPreset encoderPresetFromName(const std::wstring& value) {
    if (value == L"p1" || value == L"P1") {
        return EncoderPreset::P1;
    }
    if (value == L"p2" || value == L"P2") {
        return EncoderPreset::P2;
    }
    if (value == L"p3" || value == L"P3") {
        return EncoderPreset::P3;
    }
    if (value == L"p5" || value == L"P5") {
        return EncoderPreset::P5;
    }
    if (value == L"p6" || value == L"P6") {
        return EncoderPreset::P6;
    }
    if (value == L"p7" || value == L"P7") {
        return EncoderPreset::P7;
    }
    return EncoderPreset::P4;
}

const wchar_t* encoderModeName(EncoderMode mode) {
    switch (mode) {
    case EncoderMode::HighQuality:
        return L"high-quality";
    case EncoderMode::LowLatency:
        return L"low-latency";
    case EncoderMode::UltraLowLatency:
        return L"ultra-low-latency";
    case EncoderMode::Lossless:
        return L"lossless";
    case EncoderMode::UltraHighQuality:
        return L"ultra-high-quality";
    }
    return L"high-quality";
}

EncoderMode encoderModeFromName(const std::wstring& value) {
    if (value == L"low-latency" || value == L"LOW_LATENCY") {
        return EncoderMode::LowLatency;
    }
    if (value == L"ultra-low-latency" || value == L"ULTRA_LOW_LATENCY") {
        return EncoderMode::UltraLowLatency;
    }
    if (value == L"lossless" || value == L"LOSSLESS") {
        return EncoderMode::Lossless;
    }
    if (value == L"ultra-high-quality" || value == L"ULTRA_HIGH_QUALITY") {
        return EncoderMode::UltraHighQuality;
    }
    return EncoderMode::HighQuality;
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
    if (value == L"balanced" || value == L"BALANCED") {
        return EncoderProfile::Balanced;
    }
    if (value == L"custom" || value == L"CUSTOM") {
        return EncoderProfile::Custom;
    }
    return EncoderProfile::LowestGpu;
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
    if (value == L"quarter-resolution" || value == L"quarter" || value == L"QUARTER") {
        return EncoderMultipass::QuarterResolution;
    }
    if (value == L"full-resolution" || value == L"full" || value == L"FULL") {
        return EncoderMultipass::FullResolution;
    }
    return EncoderMultipass::Disabled;
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
    if (value == L"disabled" || value == L"DISABLED" || value == L"off") {
        return GpuAdaptiveMode::Disabled;
    }
    if (value == L"aggressive" || value == L"AGGRESSIVE") {
        return GpuAdaptiveMode::Aggressive;
    }
    return GpuAdaptiveMode::Conservative;
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
    if (value == L"desktop-duplication" || value == L"dda" || value == L"dxgi") {
        return CaptureBackend::DesktopDuplication;
    }
    return CaptureBackend::WindowsGraphicsCapture;
}

} // namespace backtrack
