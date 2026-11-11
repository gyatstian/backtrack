#pragma once

#include "core/Types.h"

#include <iterator>

namespace backtrack {

// Display/serialization names for the shared codec/preset/mode enums. Used by
// the settings store (persist), the encoders (init telemetry), and the
// diagnostics page, so they live in core instead of coupling those domains to
// settings. Header-only like the other core helpers. The matching FromName
// parsers stay in settings/SettingsStore.cpp (they are persistence concerns).

inline const wchar_t* codecName(VideoCodec codec) {
    return codec == VideoCodec::Hevc ? L"hevc" : L"h264";
}

inline const wchar_t* encoderPresetName(EncoderPreset preset) {
    static constexpr const wchar_t* names[] = {L"p1", L"p2", L"p3", L"p4", L"p5", L"p6", L"p7"};
    const auto index = static_cast<unsigned>(preset);
    return index < std::size(names) ? names[index] : L"p4";
}

inline const wchar_t* encoderModeName(EncoderMode mode) {
    static constexpr const wchar_t* names[] = {
        L"high-quality", L"low-latency", L"ultra-low-latency", L"lossless", L"ultra-high-quality"};
    const auto index = static_cast<unsigned>(mode);
    return index < std::size(names) ? names[index] : L"high-quality";
}

} // namespace backtrack
