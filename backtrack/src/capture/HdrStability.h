#pragma once

#include <dxgi.h>

#include <chrono>

namespace backtrack {

// R10 may be sRGB or PQ; its format alone does not establish a color space.
inline bool isHdrCaptureFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

inline bool isDesktopDuplicationFormat(DXGI_FORMAT format, bool legacy) {
    return format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        (!legacy && isHdrCaptureFormat(format));
}

struct DesktopDuplicationResult {
    HRESULT result;
    HRESULT modernResult;
    bool usedLegacy;
};

// keep transient failures on the same API so recovery does not silently lose HDR.
template <typename Modern, typename Legacy, typename Wait>
DesktopDuplicationResult duplicateDesktopOutput(
    HRESULT output5Result, Modern&& modern, Legacy&& legacy, Wait&& wait) {
    constexpr DXGI_FORMAT formats[] = {
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_B8G8R8A8_UNORM,
    };
    const auto permitsLegacy = [](HRESULT hr) {
        return hr == E_NOINTERFACE || hr == E_NOTIMPL ||
            hr == DXGI_ERROR_UNSUPPORTED || hr == E_INVALIDARG;
    };
    bool useLegacy = false;
    HRESULT hr = output5Result;
    constexpr int kMaxAttempts = 4;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (FAILED(output5Result)) {
            if (!permitsLegacy(output5Result)) {
                return {output5Result, output5Result, false};
            }
            useLegacy = true;
        }
        if (!useLegacy) {
            hr = modern(0u, static_cast<UINT>(sizeof(formats) / sizeof(formats[0])), formats);
            if (permitsLegacy(hr)) {
                output5Result = hr;
                useLegacy = true;
                attempt = 0; // retain the legacy API's retry budget
            }
        }
        if (useLegacy) {
            hr = legacy();
        }
        if (SUCCEEDED(hr) || (hr != DXGI_ERROR_NOT_CURRENTLY_AVAILABLE &&
            hr != DXGI_ERROR_ACCESS_LOST && hr != E_ACCESSDENIED)) {
            break;
        }
        if (attempt + 1 < kMaxAttempts) {
            wait();
        }
    }
    return {hr, output5Result, useLegacy};
}

// retry schedule after the video processor refuses HDR tone-mapping.
inline std::chrono::seconds hdrRejectionCooldown(int consecutiveRejections) {
    if (consecutiveRejections <= 1) {
        return std::chrono::seconds(5);
    }
    if (consecutiveRejections == 2) {
        return std::chrono::seconds(15);
    }
    return std::chrono::seconds(60);
}

} // namespace backtrack
