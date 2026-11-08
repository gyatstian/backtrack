#pragma once

#include "core/Types.h"

namespace backtrack {

// Display/serialization names for the shared codec/preset/mode enums. Used by
// the settings store (persist), the encoders (init telemetry), and the
// diagnostics page, so they live in core instead of coupling those domains to
// settings. Header-only like the other core helpers. The matching FromName
// parsers stay in settings/SettingsStore.cpp (they are persistence concerns).

inline const wchar_t* codecName(VideoCodec codec) {
    switch (codec) {
    case VideoCodec::H264:
        return L"h264";
    case VideoCodec::Hevc:
        return L"hevc";
    }
    return L"h264";
}

inline const wchar_t* encoderPresetName(EncoderPreset preset) {
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

inline const wchar_t* encoderModeName(EncoderMode mode) {
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

} // namespace backtrack
