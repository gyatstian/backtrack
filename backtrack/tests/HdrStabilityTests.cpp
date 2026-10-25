#include "capture/HdrStability.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using backtrack::HdrSessionPin;
using backtrack::hdrRejectionCooldown;
using backtrack::isHdrCaptureFormat;
using backtrack::srgbChannelToLinear;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testPinStartsSdr() {
    HdrSessionPin pin;
    expect(pin.begin(DXGI_FORMAT_B8G8R8A8_UNORM) == DXGI_FORMAT_B8G8R8A8_UNORM,
        "SDR session did not pool BGRA");
    expect(!pin.pinnedHdr(), "SDR session pinned HDR");
    expect(pin.poolFormat() == DXGI_FORMAT_B8G8R8A8_UNORM, "SDR pool format not BGRA");
}

void testPinStartsHdrFromSurface() {
    HdrSessionPin pin;
    expect(pin.begin(DXGI_FORMAT_R16G16B16A16_FLOAT) == DXGI_FORMAT_R16G16B16A16_FLOAT,
        "HDR surface did not pool scRGB");
    expect(pin.pinnedHdr(), "HDR surface did not pin");
    HdrSessionPin pq;
    expect(pq.begin(DXGI_FORMAT_R10G10B10A2_UNORM) == DXGI_FORMAT_R10G10B10A2_UNORM,
        "PQ surface did not pool R10");
    expect(pq.pinnedHdr(), "PQ surface did not pin");
}

void testPinSurfacesDecide() {
    // The OS poll never pre-pins: an HDR report with all-BGRA surfaces is a
    // whole SDR session, and an scRGB pool would force every one of its
    // frames through the HDR tone-map. The first surface alone decides; a
    // later HDR surface still upgrades once (testPinUpgradesOnce).
    HdrSessionPin pin;
    expect(pin.begin(DXGI_FORMAT_B8G8R8A8_UNORM) == DXGI_FORMAT_B8G8R8A8_UNORM,
        "first SDR surface did not pool BGRA");
    expect(!pin.pinnedHdr(), "SDR first surface pinned HDR");
}

void testPinUpgradesOnce() {
    HdrSessionPin pin;
    pin.begin(DXGI_FORMAT_B8G8R8A8_UNORM);
    expect(pin.noteSurfaceFormat(DXGI_FORMAT_R16G16B16A16_FLOAT), "first HDR surface did not upgrade");
    expect(pin.pinnedHdr(), "upgrade did not pin");
    expect(pin.poolFormat() == DXGI_FORMAT_R16G16B16A16_FLOAT, "upgrade did not move the pool");
    expect(!pin.noteSurfaceFormat(DXGI_FORMAT_R16G16B16A16_FLOAT), "steady HDR re-upgraded");
    expect(!pin.noteSurfaceFormat(DXGI_FORMAT_B8G8R8A8_UNORM), "SDR surface upgraded");
}

void testPinNeverDowngrades() {
    HdrSessionPin pin;
    pin.begin(DXGI_FORMAT_R16G16B16A16_FLOAT);
    expect(!pin.noteSurfaceFormat(DXGI_FORMAT_B8G8R8A8_UNORM), "SDR surface moved a pinned session");
    expect(pin.pinnedHdr(), "pinned session downgraded to SDR");
    expect(pin.poolFormat() == DXGI_FORMAT_R16G16B16A16_FLOAT, "pinned pool left scRGB");
    HdrSessionPin upgraded;
    upgraded.begin(DXGI_FORMAT_B8G8R8A8_UNORM);
    upgraded.noteSurfaceFormat(DXGI_FORMAT_R16G16B16A16_FLOAT);
    expect(!upgraded.noteSurfaceFormat(DXGI_FORMAT_B8G8R8A8_UNORM), "upgraded session accepted SDR");
    expect(upgraded.pinnedHdr(), "upgraded session downgraded to SDR");
}

void testSrgbBridgeEndpoints() {
    // The bridge must carry SDR content exactly: black and paper white pass
    // through bit-identical, or every bridged frame flickers against SDR.
    expect(srgbChannelToLinear(0.0f) == 0.0f, "bridge did not preserve black");
    expect(srgbChannelToLinear(1.0f) == 1.0f, "bridge did not preserve paper white");
}

void testSrgbBridgeShape() {
    float prev = srgbChannelToLinear(0.0f);
    for (int i = 1; i <= 255; ++i) {
        const float linear = srgbChannelToLinear(static_cast<float>(i) / 255.0f);
        expect(linear > prev, "bridge transfer is not monotonic");
        prev = linear;
    }
    const float mid = srgbChannelToLinear(0.5f);
    expect(mid > 0.20f && mid < 0.23f, "bridge midtone left the sRGB curve");
    // 8-bit round trip through scRGB-linear stays within 1 LSB, so a bridged
    // frame and a direct SDR frame of the same desktop are indistinguishable.
    for (int i = 0; i <= 255; ++i) {
        const float linear = srgbChannelToLinear(static_cast<float>(i) / 255.0f);
        const float back = linear <= 0.0031308f
            ? linear * 12.92f
            : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
        const int roundTripped = static_cast<int>(back * 255.0f + 0.5f);
        const int drift = roundTripped >= i ? roundTripped - i : i - roundTripped;
        expect(drift <= 1, "bridge round trip drifted more than 1 LSB");
    }
}

void testSrgbBridgeExactMidpoint() {
    // Pins the transfer the bridge shader implements exactly: 0.5 sRGB is
    // 0.214041 linear. A gamma passthrough (0.5) or a pure 2.2 power
    // (0.2176) both miss by >1e-3 and fry mids through the tone-map.
    const double oracle = std::pow((0.5 + 0.055) / 1.055, 2.4);
    const float bridged = srgbChannelToLinear(0.5f);
    expect(std::fabs(static_cast<double>(bridged) - oracle) < 1e-6,
        "bridge midpoint left the sRGB curve");
    expect(bridged > 0.213f && bridged < 0.215f, "bridge midpoint out of band");
}

void testSrgbBridgeToeBoundary() {
    // The piecewise joint: 0.04045 takes the linear segment, anything above
    // the power segment, with no visible step between them.
    const float below = srgbChannelToLinear(0.04045f);
    expect(std::fabs(below - 0.04045f / 12.92f) < 1e-7f, "bridge toe segment moved");
    const double oracle = std::pow((0.04046 + 0.055) / 1.055, 2.4);
    const float above = srgbChannelToLinear(0.04046f);
    expect(std::fabs(static_cast<double>(above) - oracle) < 1e-6,
        "bridge power segment moved at the joint");
    expect(std::fabs(above - below) < 1e-4f, "bridge transfer steps at the joint");
}

void testSrgbBridgeWhiteScale() {
    // Paper-white scale guard: SDR white lands on scRGB 1.0 exactly, not on
    // an 80-nit or 10000-nit rescaling, and mid gray is linearized rather
    // than passed through. Either failure drives the whole SDR range through
    // the wrong tone-map region.
    expect(srgbChannelToLinear(1.0f) == 1.0f, "bridge rescaled paper white");
    expect(srgbChannelToLinear(0.5f) != 0.5f, "bridge passed gamma through");
    // shortcut: if a gray-ramp recording ever clears bridged tenures only
    // with an identity bridge, the duplication BGRA holds linear and this
    // transfer (plus the shader) must become identity.
}

void testBackoffSchedule() {
    expect(hdrRejectionCooldown(0).count() == 5, "first retry did not wait 5 s");
    expect(hdrRejectionCooldown(1).count() == 5, "retry after one rejection did not wait 5 s");
    expect(hdrRejectionCooldown(2).count() == 15, "retry after two rejections did not wait 15 s");
    expect(hdrRejectionCooldown(3).count() == 60, "retry after three rejections did not wait 60 s");
    expect(hdrRejectionCooldown(1000).count() == 60, "backoff did not cap at 60 s");
}

void testHdrFormatPredicate() {
    expect(isHdrCaptureFormat(DXGI_FORMAT_R10G10B10A2_UNORM), "R10G10B10A2 was not HDR");
    expect(isHdrCaptureFormat(DXGI_FORMAT_R16G16B16A16_FLOAT), "R16F was not HDR");
    expect(!isHdrCaptureFormat(DXGI_FORMAT_B8G8R8A8_UNORM), "BGRA took the tone-map path");
    expect(!isHdrCaptureFormat(DXGI_FORMAT_NV12), "NV12 took the tone-map path");
    expect(!isHdrCaptureFormat(DXGI_FORMAT_UNKNOWN), "UNKNOWN took the tone-map path");
}

} // namespace

int main() {
    try {
        testPinStartsSdr();
        testPinStartsHdrFromSurface();
        testPinSurfacesDecide();
        testPinUpgradesOnce();
        testPinNeverDowngrades();
        testSrgbBridgeEndpoints();
        testSrgbBridgeShape();
        testSrgbBridgeExactMidpoint();
        testSrgbBridgeToeBoundary();
        testSrgbBridgeWhiteScale();
        testBackoffSchedule();
        testHdrFormatPredicate();
    } catch (const std::exception& e) {
        std::cerr << "HdrStabilityTests FAILED: " << e.what() << "\n";
        return 1;
    }
    std::cout << "HdrStabilityTests passed\n";
    return 0;
}
