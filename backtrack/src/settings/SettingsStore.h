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

} // namespace backtrack
