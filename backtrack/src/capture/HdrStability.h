#pragma once

// Stable HDR interpretation shared by DesktopDuplicationCapture and
// D3D11FrameScaler. Pure logic, no device: drivers that refuse HDR
// tone-mapping flap when retried on a fixed window, and the sRGB bridge needs
// the same transfer function on CPU (tests) and GPU (shader). Both rules live
// here so they stay unit-testable; the capture and scaler files own all GPU
// work.

#include <dxgiformat.h>

#include <chrono>
#include <cmath>

namespace backtrack {

// PQ/scRGB surfaces carry native HDR content. 8-bit BGRA under HDR is already
// SDR-mapped by composition and must never take the tone-map path.
inline bool isHdrCaptureFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R10G10B10A2_UNORM ||
        format == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

// sRGB channel to scRGB-linear. CPU mirror of the capture bridge pixel shader
// (kBridgeShaderHlsl): an SDR surface normalized through this into a pinned
// HDR session carries SDR content exactly - 0 stays 0, paper white stays 1 -
// so bridged and native frames take the identical tone-map path downstream
// and cannot flicker against each other.
inline float srgbChannelToLinear(float channel) {
    return channel <= 0.04045f ? channel / 12.92f : std::pow((channel + 0.055f) / 1.055f, 2.4f);
}

// Session-pinned HDR interpretation. The duplication surface genuinely
// oscillates BGRA<->scRGB with seconds-long dwells in each state on some
// HDR monitors, and the GetDesc1 poll oscillates with it, so no agreement
// filter can converge: both states look stable for seconds, and each
// converged segment would encode through a different mapping (flicker) or
// hold frames until the pipeline starves. The pin ratchets instead: HDR once
// any acquired surface says HDR, and it never downgrades
// mid-session, because an HDR pool carries SDR content exactly (the bridge
// normalizes it) while an SDR pool would record HDR content washed-out.
// Single mapping, so alternating surfaces produce identical frames. The
// init-time poll never pins: it stayed HDR for whole sessions whose every
// surface arrived BGRA, and pooling scRGB on the poll alone forces those
// SDR-range frames through the HDR tone-map.
class HdrSessionPin {
public:
    // First acquired surface decides pool and pin. Surfaces decide: an SDR
    // first surface pools BGRA even under an HDR poll, and a later HDR
    // surface upgrades the session once through noteSurfaceFormat.
    DXGI_FORMAT begin(DXGI_FORMAT firstSurface) {
        pinned_ = isHdrCaptureFormat(firstSurface);
        poolFormat_ = pinned_ ? firstSurface : DXGI_FORMAT_B8G8R8A8_UNORM;
        return poolFormat_;
    }
    // Later surfaces. True once, when an HDR surface upgrades an SDR-pinned
    // session (caller recreates the pool); SDR surfaces never upgrade and a
    // pinned session never downgrades.
    bool noteSurfaceFormat(DXGI_FORMAT format) {
        if (!pinned_ && isHdrCaptureFormat(format)) {
            pinned_ = true;
            poolFormat_ = format;
            return true;
        }
        return false;
    }
    bool pinnedHdr() const {
        return pinned_;
    }
    DXGI_FORMAT poolFormat() const {
        return poolFormat_;
    }
    void reset() {
        pinned_ = false;
        poolFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    }

private:
    bool pinned_ = false;
    DXGI_FORMAT poolFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
};

// Retry schedule after the video processor refuses HDR tone-mapping. A fixed
// window re-alternates tone-mapped and SDR frames on marginal drivers; each
// consecutive rejection backs off further and a recovery resets the count.
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
