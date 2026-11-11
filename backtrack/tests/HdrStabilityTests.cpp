#include "capture/HdrStability.h"
#include "platform/Win32Util.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using backtrack::DesktopDuplicationResult;
using backtrack::duplicateDesktopOutput;
using backtrack::hdrRejectionCooldown;
using backtrack::isDesktopDuplicationFormat;
using backtrack::isHdrCaptureFormat;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct FakeOutput {
    std::vector<HRESULT> modernResults{S_OK};
    std::vector<HRESULT> legacyResults{S_OK};
    size_t modernCalls = 0;
    size_t legacyCalls = 0;
    int waits = 0;

    DesktopDuplicationResult duplicate(HRESULT output5Result = S_OK) {
        return duplicateDesktopOutput(output5Result,
            [&](UINT flags, UINT count, const DXGI_FORMAT* formats) {
                expect(flags == 0, "DuplicateOutput1 flags changed");
                expect(count == 2, "unexpected number of duplication formats");
                expect(formats != nullptr, "missing duplication formats");
                expect(formats[0] == DXGI_FORMAT_R16G16B16A16_FLOAT &&
                    formats[1] == DXGI_FORMAT_B8G8R8A8_UNORM,
                    "DuplicateOutput1 did not request scRGB and BGRA only");
                const size_t index = modernCalls++;
                return modernResults[index < modernResults.size() ? index : modernResults.size() - 1];
            },
            [&]() {
                const size_t index = legacyCalls++;
                return legacyResults[index < legacyResults.size() ? index : legacyResults.size() - 1];
            },
            [&]() { ++waits; });
    }
};

void testModernNegotiation() {
    FakeOutput output;
    const auto result = output.duplicate();
    expect(result.result == S_OK && !result.usedLegacy, "modern duplication did not succeed");
    expect(output.modernCalls == 1 && output.legacyCalls == 0 && output.waits == 0,
        "modern success called the legacy API or retried");
}

void testLegacyFallback() {
    FakeOutput missingInterface;
    const auto missing = missingInterface.duplicate(E_NOINTERFACE);
    expect(missing.result == S_OK && missing.usedLegacy && missing.modernResult == E_NOINTERFACE,
        "missing Output5 did not use legacy duplication");
    expect(missingInterface.modernCalls == 0 && missingInterface.legacyCalls == 1,
        "missing Output5 called the modern API");

    for (const HRESULT capabilityFailure : {E_NOINTERFACE, E_NOTIMPL, DXGI_ERROR_UNSUPPORTED, E_INVALIDARG}) {
        FakeOutput output;
        output.modernResults = {capabilityFailure};
        output.legacyResults = {DXGI_ERROR_ACCESS_LOST, DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, S_OK};
        const auto result = output.duplicate();
        expect(result.result == S_OK && result.usedLegacy && result.modernResult == capabilityFailure,
            "capability failure did not use legacy duplication");
        expect(output.modernCalls == 1 && output.legacyCalls == 3 && output.waits == 2,
            "legacy retry did not stay on the fallback API");
    }
}

void testTransientAndFatalFailures() {
    FakeOutput transient;
    transient.modernResults = {E_ACCESSDENIED, DXGI_ERROR_ACCESS_LOST, S_OK};
    const auto recovered = transient.duplicate();
    expect(recovered.result == S_OK && !recovered.usedLegacy,
        "transient modern failure discarded HDR negotiation");
    expect(transient.modernCalls == 3 && transient.legacyCalls == 0 && transient.waits == 2,
        "transient modern failure did not retry the modern API");

    FakeOutput exhausted;
    exhausted.modernResults = {DXGI_ERROR_NOT_CURRENTLY_AVAILABLE};
    const auto failed = exhausted.duplicate();
    expect(failed.result == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE && !failed.usedLegacy,
        "exhausted modern retries used legacy or hid the failure");
    expect(exhausted.modernCalls == 4 && exhausted.legacyCalls == 0 && exhausted.waits == 3,
        "modern retry budget changed");

    FakeOutput fatal;
    fatal.modernResults = {DXGI_ERROR_DEVICE_REMOVED};
    const auto removed = fatal.duplicate();
    expect(removed.result == DXGI_ERROR_DEVICE_REMOVED && !removed.usedLegacy,
        "device loss was hidden by legacy fallback");
    expect(fatal.modernCalls == 1 && fatal.legacyCalls == 0 && fatal.waits == 0,
        "device loss was retried or fell back");

    FakeOutput queryFailure;
    const auto query = queryFailure.duplicate(E_OUTOFMEMORY);
    expect(query.result == E_OUTOFMEMORY && !query.usedLegacy &&
        queryFailure.modernCalls == 0 && queryFailure.legacyCalls == 0,
        "unexpected Output5 query failure was hidden");

    FakeOutput mixed;
    mixed.modernResults = {DXGI_ERROR_ACCESS_LOST, E_NOTIMPL};
    mixed.legacyResults = {E_ACCESSDENIED, E_ACCESSDENIED, E_ACCESSDENIED, S_OK};
    const auto fallbackRecovered = mixed.duplicate();
    expect(fallbackRecovered.result == S_OK && fallbackRecovered.usedLegacy &&
        mixed.modernCalls == 2 && mixed.legacyCalls == 4 && mixed.waits == 4,
        "legacy fallback lost its retry budget after modern retries");

    FakeOutput legacyFailure;
    legacyFailure.legacyResults = {E_ACCESSDENIED};
    const auto denied = legacyFailure.duplicate(E_NOINTERFACE);
    expect(denied.result == E_ACCESSDENIED && denied.usedLegacy &&
        legacyFailure.legacyCalls == 4 && legacyFailure.waits == 3,
        "legacy failure or retry budget was lost");
}

void testSurfaceFormats() {
    expect(isDesktopDuplicationFormat(DXGI_FORMAT_B8G8R8A8_UNORM, false), "modern BGRA rejected");
    expect(isDesktopDuplicationFormat(DXGI_FORMAT_B8G8R8A8_UNORM, true), "legacy BGRA rejected");
    expect(isDesktopDuplicationFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, false), "modern scRGB rejected");
    expect(!isDesktopDuplicationFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, true), "legacy accepted scRGB");
    expect(!isDesktopDuplicationFormat(DXGI_FORMAT_R10G10B10A2_UNORM, false), "unverified R10 accepted");
    expect(!isDesktopDuplicationFormat(DXGI_FORMAT_NV12, false), "unnegotiated NV12 accepted");
    expect(!isDesktopDuplicationFormat(DXGI_FORMAT_UNKNOWN, false), "unknown format accepted");
    expect(isHdrCaptureFormat(DXGI_FORMAT_R16G16B16A16_FLOAT), "scRGB lost its linear mapping");
    expect(!isHdrCaptureFormat(DXGI_FORMAT_B8G8R8A8_UNORM), "BGRA took the HDR mapping");
    expect(!isHdrCaptureFormat(DXGI_FORMAT_R10G10B10A2_UNORM), "R10 was assumed to be PQ");
}

void testAdvancedColorSpaces() {
    expect(backtrack::isAdvancedColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020),
        "PQ output was not advanced color");
    expect(backtrack::isAdvancedColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709),
        "scRGB output was not advanced color");
    for (const auto colorSpace : {
            DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
            DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709,
            DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020,
            DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020,
            DXGI_COLOR_SPACE_RESERVED,
            DXGI_COLOR_SPACE_CUSTOM}) {
        expect(!backtrack::isAdvancedColorSpace(colorSpace),
            "SDR or an unrecognized output was assumed to be advanced color");
    }
}

void testBackoffSchedule() {
    expect(hdrRejectionCooldown(0).count() == 5, "first retry did not wait 5 s");
    expect(hdrRejectionCooldown(1).count() == 5, "retry after one rejection did not wait 5 s");
    expect(hdrRejectionCooldown(2).count() == 15, "retry after two rejections did not wait 15 s");
    expect(hdrRejectionCooldown(3).count() == 60, "retry after three rejections did not wait 60 s");
    expect(hdrRejectionCooldown(1000).count() == 60, "backoff did not cap at 60 s");
}

} // namespace

int main() {
    try {
        testModernNegotiation();
        testLegacyFallback();
        testTransientAndFatalFailures();
        testSurfaceFormats();
        testAdvancedColorSpaces();
        testBackoffSchedule();
    } catch (const std::exception& e) {
        std::cerr << "HdrStabilityTests FAILED: " << e.what() << "\n";
        return 1;
    }
    std::cout << "HdrStabilityTests passed\n";
    return 0;
}
