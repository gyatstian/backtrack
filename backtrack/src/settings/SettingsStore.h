#pragma once

#include "core/Types.h"

#include <filesystem>

namespace backtrack {

class SettingsStore {
public:
    explicit SettingsStore(std::filesystem::path path);

    AppSettings load() const;
    void save(const AppSettings& settings) const;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

AppSettings sanitizeSettings(AppSettings settings);

// Pure VideoSettings validation: every numeric range and the adaptive B/I-frame
// gates live here and nowhere else, so a range change is one edit in one
// expression. sanitizeSettings calls it on the load/save path; live encoder use
// shares it rather than repeating the ranges.
VideoSettings validateEncoderConfig(VideoSettings video);

// Hotkey safety is defined here, not in the UI, because sanitizeSettings is the
// authority: the UI must reject exactly the hotkeys a load would reset.
// supportedHotkeyModifiers is the single source for the mask; the HotkeyService
// hook matcher must call it instead of repeating the expression.
uint32_t supportedHotkeyModifiers(uint32_t modifiers);
bool isSafeBareHotkey(uint32_t virtualKey);
bool isSafeGlobalHotkey(uint32_t modifiers, uint32_t virtualKey);

const wchar_t* codecName(VideoCodec codec);
VideoCodec codecFromName(const std::wstring& value);
const wchar_t* resolutionModeName(ResolutionMode mode);
ResolutionMode resolutionModeFromName(const std::wstring& value);
const wchar_t* encoderPresetName(EncoderPreset preset);
EncoderPreset encoderPresetFromName(const std::wstring& value);
const wchar_t* encoderModeName(EncoderMode mode);
EncoderMode encoderModeFromName(const std::wstring& value);
const wchar_t* encoderProfileName(EncoderProfile profile);
EncoderProfile encoderProfileFromName(const std::wstring& value);
const wchar_t* encoderMultipassName(EncoderMultipass multipass);
EncoderMultipass encoderMultipassFromName(const std::wstring& value);
const wchar_t* gpuAdaptiveModeName(GpuAdaptiveMode mode);
GpuAdaptiveMode gpuAdaptiveModeFromName(const std::wstring& value);
const wchar_t* captureBackendName(CaptureBackend backend);
CaptureBackend captureBackendFromName(const std::wstring& value);
const wchar_t* voiceCommandModeName(GameIntegrationSettings::VoiceCommandMode mode);
GameIntegrationSettings::VoiceCommandMode voiceCommandModeFromName(const std::wstring& value);
const wchar_t* discordRichPresenceModeName(GameIntegrationSettings::DiscordRichPresenceMode mode);
GameIntegrationSettings::DiscordRichPresenceMode discordRichPresenceModeFromName(const std::wstring& value);

} // namespace backtrack
