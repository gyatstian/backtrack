#include "encoder/AmfEncoder.h"

#include "capture/D3DDevice.h"
#include "core/EncodingNames.h"
#include "core/Logger.h"
#include "encoder/DecodeOrderTimestamps.h"
#include "encoder/EncoderCommon.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <thread>

#ifndef BACKTRACK_HAS_AMF
#if __has_include("AMF/core/Factory.h")
#define BACKTRACK_HAS_AMF 1
#else
#define BACKTRACK_HAS_AMF 0
#endif
#endif

#if BACKTRACK_HAS_AMF
#include "AMF/components/ColorSpace.h"
#include "AMF/components/VideoEncoderHEVC.h"
#include "AMF/components/VideoEncoderVCE.h"
#include "AMF/core/Factory.h"
#include "AMF/core/Version.h"
#endif

namespace backtrack {

#if BACKTRACK_HAS_AMF

namespace {

std::wstring amfResultName(AMF_RESULT result) {
    switch (result) {
    case AMF_OK:
        return L"AMF_OK";
    case AMF_FAIL:
        return L"AMF_FAIL";
    case AMF_UNEXPECTED:
        return L"AMF_UNEXPECTED";
    case AMF_ACCESS_DENIED:
        return L"AMF_ACCESS_DENIED";
    case AMF_INVALID_ARG:
        return L"AMF_INVALID_ARG";
    case AMF_OUT_OF_RANGE:
        return L"AMF_OUT_OF_RANGE";
    case AMF_OUT_OF_MEMORY:
        return L"AMF_OUT_OF_MEMORY";
    case AMF_INVALID_POINTER:
        return L"AMF_INVALID_POINTER";
    case AMF_NO_INTERFACE:
        return L"AMF_NO_INTERFACE";
    case AMF_NOT_IMPLEMENTED:
        return L"AMF_NOT_IMPLEMENTED";
    case AMF_NOT_SUPPORTED:
        return L"AMF_NOT_SUPPORTED";
    case AMF_NOT_FOUND:
        return L"AMF_NOT_FOUND";
    case AMF_ALREADY_INITIALIZED:
        return L"AMF_ALREADY_INITIALIZED";
    case AMF_NOT_INITIALIZED:
        return L"AMF_NOT_INITIALIZED";
    case AMF_INVALID_FORMAT:
        return L"AMF_INVALID_FORMAT";
    case AMF_WRONG_STATE:
        return L"AMF_WRONG_STATE";
    case AMF_NO_DEVICE:
        return L"AMF_NO_DEVICE";
    case AMF_DIRECTX_FAILED:
        return L"AMF_DIRECTX_FAILED";
    case AMF_EOF:
        return L"AMF_EOF";
    case AMF_REPEAT:
        return L"AMF_REPEAT";
    case AMF_INPUT_FULL:
        return L"AMF_INPUT_FULL";
    case AMF_RESOLUTION_CHANGED:
        return L"AMF_RESOLUTION_CHANGED";
    case AMF_SURFACE_FORMAT_NOT_SUPPORTED:
        return L"AMF_SURFACE_FORMAT_NOT_SUPPORTED";
    case AMF_SURFACE_MUST_BE_SHARED:
        return L"AMF_SURFACE_MUST_BE_SHARED";
    case AMF_ENCODER_NOT_PRESENT:
        return L"AMF_ENCODER_NOT_PRESENT";
    case AMF_NEED_MORE_INPUT:
        return L"AMF_NEED_MORE_INPUT";
    default:
        return L"AMF error " + std::to_wstring(static_cast<int>(result));
    }
}

// Formats the loaded amfrt64.dll runtime version for diagnostics.
std::wstring runtimeVersionString(uint64_t version) {
    return std::to_wstring(AMF_GET_MAJOR_VERSION(version)) + L"." +
           std::to_wstring(AMF_GET_MINOR_VERSION(version)) + L"." +
           std::to_wstring(AMF_GET_SUBMINOR_VERSION(version));
}

// Maps the requested encoder preset (P1 fastest .. P7 slowest) to the nearest
// AMF quality preset. AMF exposes three effective steps: SPEED, BALANCED,
// QUALITY.
int avcQualityPreset(EncoderPreset preset) {
    return preset == EncoderPreset::P1 || preset == EncoderPreset::P2 ? AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED :
           preset == EncoderPreset::P6 || preset == EncoderPreset::P7 ? AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY :
           AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED;
}

int hevcQualityPreset(EncoderPreset preset) {
    return preset == EncoderPreset::P1 || preset == EncoderPreset::P2 ? AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED :
           preset == EncoderPreset::P6 || preset == EncoderPreset::P7 ? AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_QUALITY :
           AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED;
}

int avcUsage(EncoderMode mode) {
    static constexpr int usages[] = {
        AMF_VIDEO_ENCODER_USAGE_TRANSCODING, AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY,
        AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY, AMF_VIDEO_ENCODER_USAGE_TRANSCODING,
        AMF_VIDEO_ENCODER_USAGE_TRANSCODING};
    const auto index = static_cast<unsigned>(mode);
    return index < std::size(usages) ? usages[index] : AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY;
}

int hevcUsage(EncoderMode mode) {
    static constexpr int usages[] = {
        AMF_VIDEO_ENCODER_HEVC_USAGE_TRANSCODING, AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY,
        AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY, AMF_VIDEO_ENCODER_HEVC_USAGE_TRANSCODING,
        AMF_VIDEO_ENCODER_HEVC_USAGE_TRANSCODING};
    const auto index = static_cast<unsigned>(mode);
    return index < std::size(usages) ? usages[index] : AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY;
}

} // namespace

struct AmfEncoder::Impl {
    HMODULE module = nullptr;
    amf::AMFFactory* factory = nullptr;
    amf::AMFContextPtr context;
    amf::AMFComponentPtr encoder;
    uint64_t runtimeVersion = 0;

    VideoSettings settings;
    EncoderCapabilities caps;
    EncoderStats encoderStats;
    mutable std::mutex mutex;
    // Last stats/capabilities copied while mutex was free. stats() and
    // capabilities() serve these when try_lock fails so a diagnostics poll
    // never queues behind a driver call (encode/drain hold mutex across
    // QueryOutput sweeps and the Drain helper join). Slightly stale under
    // load; the lock-free availableAtomic stays the health signal.
    mutable std::mutex snapshotMutex;
    EncoderStats statsSnapshot;
    EncoderCapabilities capsSnapshot;
    // Lock-free mirror of encoderStats.encoderAvailable for the capture loop's
    // per-iteration health probe (IEncoder::encoderAvailable). Written only on
    // paths that already hold mutex, so the pair never disagrees observably.
    std::atomic<bool> availableAtomic{false};
    // Lock-free mirror of the drop count for IEncoder::droppedInputs, which the
    // recording-stop drain reads without taking mutex.
    std::atomic<uint64_t> droppedInputAtomic{0};
    // Lock-free mirror of inFlightCap() for IEncoder::inputRetentionBudget, which
    // the capture loop queries while encode() holds mutex across QueryOutput.
    std::atomic<uint32_t> retentionBudget{0};

    void publishAvailability(bool available) {
        encoderStats.encoderAvailable = available;
        availableAtomic.store(available, std::memory_order_release);
    }

    // The session is fully configured and usable. Availability is published
    // only here: a failure while configuring must never leave the encoder
    // looking healthy to the capture loop.
    void publishSessionActive() {
        caps.available = true;
        publishAvailability(true);
    }

    // An input this session left without an output: the drop counter and its
    // lock-free mirror must move together.
    void noteDroppedInput() {
        ++encoderStats.droppedFrames;
        droppedInputAtomic.fetch_add(1, std::memory_order_relaxed);
    }

    // The capture scaler converts frames to NV12 (BT.709 limited) through the
    // fixed-function video processor, which is what AMD VCN hardware natively
    // consumes. Feeding BGRA would instead force the AMF runtime to run its own
    // compute conversion pass on every submitted frame.
    amf::AMF_SURFACE_FORMAT inputSurfaceFormat = amf::AMF_SURFACE_NV12;

    std::atomic<bool> forceKeyFrame{true};

    struct PendingInput {
        amf::AMFSurfacePtr surface;
        std::shared_ptr<void> lease;
        int64_t fallbackPts100ns = 0;
        int64_t fallbackDuration100ns = 0;
        std::chrono::steady_clock::time_point submittedAt;
    };

    std::deque<PendingInput> inFlight;
    std::deque<EncodedPacket> readyPackets;
    // Bound pinned capture leases, with headroom for the actual B-frame pattern.
    static constexpr size_t kMinInFlightCap = 16;
    static constexpr auto kInFlightMaxAge = kEncoderInFlightMaxAge;
    uint64_t inFlightDropCount = 0;
    uint32_t bFrameDelayFrames = 0;
    bool bFramesUnavailable = false;
    bool preAnalysisUnavailable = false;

    size_t inFlightCap() const {
        return std::max<size_t>(kMinInFlightCap, static_cast<size_t>(bFrameDelayFrames) + 8);
    }

    uint32_t reorderDelayFrames() const {
        return bFrameDelayFrames;
    }
    DecodeOrderTimestamps decodeTimestamps;

    // Cached codec parameter sets (VPS/SPS/PPS or SPS/PPS) captured from the
    // first packet that carried them. Prepended to any keyframe packet that
    // lacks them so a replay cut at an IDR is always self-contained even if
    // the driver ignores header-insertion settings.
    std::vector<uint8_t> cachedParameterSets;
    bool encoderFaulted = false;
    uint32_t consecutiveSubmitFailures = 0;

    // Set when a Drain() exceeded its budget and was abandoned on a helper
    // thread. shutdownEncoder() must then skip Terminate/FreeLibrary: the
    // helper still executes inside the driver-owned component, and destroying
    // it concurrently would race the stuck call.
    bool drainAbandoned = false;

    // AMF Drain() has no timeout parameter and blocks until the component is
    // fully flushed; a dead driver would otherwise hang clip saving while
    // pipelineGpuMutex_ is held. The flush therefore runs on a helper thread
    // and this bound decides when to abandon it (escalating to an encoder
    // fault so the capture pipeline recreates).
    static constexpr auto kDrainTimeout = kEncoderOutputTimeout;

    bool isHevc() const { return settings.codec == VideoCodec::Hevc; }

    // Applies one encoder property. Returns false when the driver rejected
    // it; optional-property failures are logged and tolerated, critical ones
    // abort initialization.
    bool setProp(amf::AMFPropertyStorage* store, const wchar_t* name, const amf::AMFVariant& value) {
        if (!store) {
            return false;
        }
        const AMF_RESULT result = store->SetProperty(name, value);
        if (result != AMF_OK) {
            Logger::instance().warning(L"encoder",
                std::wstring(L"AMF SetProperty(") + name + L") failed: " + amfResultName(result));
            return false;
        }
        return true;
    }

    // Reads a component property back after Init to detect drivers that accept
    // properties yet silently drop or reset them (seen on old Adrenalin
    // builds, which then encode with preset defaults).
    bool verifyInt64Property(const wchar_t* name, int64_t expected) const {
        if (!encoder) {
            return false;
        }
        amf::AMFVariant value;
        if (encoder->GetProperty(name, &value) != AMF_OK || value.Empty()) {
            return false;
        }
        return value.ToInt64() == expected;
    }

    bool confirmPreAnalysisDisabled(const wchar_t* name, bool disabled) {
        amf::AMFVariant value;
        if (encoder->GetProperty(name, &value) == AMF_OK &&
            (value.type == amf::AMF_VARIANT_BOOL || value.type == amf::AMF_VARIANT_INT64)) {
            if (!value.ToBool()) {
                return true;
            }
        } else if (disabled || preAnalysisUnavailable) {
            // Unsupported hardware and the SDK default both leave pre-analysis off.
            return true;
        }
        caps.detail = L"AMF pre-analysis could not be disabled or verified";
        return false;
    }

    bool establishAvcBFrameDelay(bool requested, bool applied) {
        amf::AMFVariant value;
        if (encoder->GetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, &value) == AMF_OK &&
            value.type == amf::AMF_VARIANT_INT64) {
            const int64_t pattern = value.ToInt64();
            if (pattern < 0 || static_cast<uint64_t>(pattern) > (std::numeric_limits<uint32_t>::max)()) {
                caps.detail = L"AMF returned an invalid B-frame pattern";
                return false;
            }
            bFrameDelayFrames = static_cast<uint32_t>(pattern);
            return true;
        }
        // If readback is unavailable, do not retain an unverified B-frame request.
        bFrameDelayFrames = 0;
        if ((!requested && applied) || bFramesUnavailable ||
            setProp(encoder, AMF_VIDEO_ENCODER_B_PIC_PATTERN, amf::AMFVariant(static_cast<amf_int64>(0)))) {
            return true;
        }
        caps.detail = L"AMF B-frame pattern could not be disabled or verified";
        return false;
    }

    bool verifyRateProperty(const wchar_t* name, const AMFRate& expected) const {
        if (!encoder) {
            return false;
        }
        amf::AMFVariant value;
        if (encoder->GetProperty(name, &value) != AMF_OK || value.Empty()) {
            return false;
        }
        const AMFRate rate = value.ToRate();
        return rate.num == expected.num && rate.den == expected.den;
    }

    std::wstring driverVersionHint() const {
        const std::wstring version =
            runtimeVersion != 0 ? runtimeVersionString(runtimeVersion) : L"unknown";
        return L"AMD driver ignored required encoder settings (bitrate/GOP/framerate). "
               L"AMD runtime " + version + L" is too old or faulty; update AMD Adrenalin.";
    }

    // Critical user-facing settings must survive initialization: the stored
    // bitrate/GOP/framerate are read back right after Init so a session never
    // runs half configured.
    bool verifyCriticalSettings() const {
        const int64_t bitrate = static_cast<int64_t>(settings.bitrateKbps) * 1000;
        const amf_int64 gopFrames =
            static_cast<amf_int64>(std::max<uint32_t>(1, settings.fps * settings.gopSeconds));
        const AMFRate framerate = AMFConstructRate(settings.fps, 1);

        if (isHevc()) {
            return verifyInt64Property(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, bitrate) &&
                   verifyInt64Property(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, gopFrames) &&
                   verifyRateProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, framerate);
        }
        return verifyInt64Property(AMF_VIDEO_ENCODER_TARGET_BITRATE, bitrate) &&
               verifyInt64Property(AMF_VIDEO_ENCODER_IDR_PERIOD, gopFrames) &&
               verifyRateProperty(AMF_VIDEO_ENCODER_FRAMERATE, framerate);
    }

    // Read-back check for the header-insertion settings that decide whether
    // IDRs carry SPS/PPS/VPS. A driver that silently dropped them would
    // otherwise produce replays that cannot start decoding. Warning-only:
    // the encoder also caches parameter sets from the bitstream and prepends
    // them to header-less keyframes, so a dropped setting degrades gracefully.
    void verifyHeaderInsertionSettings() const {
        bool applied = false;
        if (isHevc()) {
            amf::AMFVariant value;
            if (encoder && encoder->GetProperty(AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE, &value) == AMF_OK &&
                !value.Empty()) {
                applied = value.ToInt64() ==
                          static_cast<int64_t>(AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE_IDR_ALIGNED);
            }
        } else {
            amf::AMFVariant value;
            if (encoder && encoder->GetProperty(AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING, &value) == AMF_OK &&
                !value.Empty()) {
                applied = value.ToInt64() > 0;
            }
        }
        if (!applied) {
            Logger::instance().warning(L"encoder",
                L"AMD driver did not retain the SPS/PPS header-insertion setting; "
                L"keyframes will be patched with cached headers instead");
        }
    }

    // Output VUI is pinned BT.709 matrix/primaries/transfer with limited
    // (studio) range so clips decode with identical colors regardless of
    // player defaults. Input conversion properties are deliberately left at
    // driver AUTO: explicitly pinning InMatrixCoeff/InputFullRange uniformly
    // deep-fried SDR output on AMD Adrenalin (RX 9070 XT observed) while the
    // defaults encode it exactly, so the driver owns the conversion and we
    // only describe the stream.
    void applyColorProperties(bool beforeInit) {
        if (beforeInit) {
            if (isHevc()) {
                setProp(encoder, AMF_VIDEO_ENCODER_HEVC_INPUT_TRANSFER_CHARACTERISTIC,
                        amf::AMFVariant(static_cast<amf_int64>(AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709)));
                setProp(encoder, AMF_VIDEO_ENCODER_HEVC_INPUT_COLOR_PRIMARIES,
                        amf::AMFVariant(static_cast<amf_int64>(AMF_COLOR_PRIMARIES_BT709)));
            } else {
                setProp(encoder, AMF_VIDEO_ENCODER_INPUT_TRANSFER_CHARACTERISTIC,
                        amf::AMFVariant(static_cast<amf_int64>(AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709)));
                setProp(encoder, AMF_VIDEO_ENCODER_INPUT_COLOR_PRIMARIES,
                        amf::AMFVariant(static_cast<amf_int64>(AMF_COLOR_PRIMARIES_BT709)));
            }
            return;
        }
        if (isHevc()) {
            setProp(encoder, AMF_VIDEO_ENCODER_HEVC_OUTPUT_TRANSFER_CHARACTERISTIC,
                    amf::AMFVariant(static_cast<amf_int64>(AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709)));
            setProp(encoder, AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PRIMARIES,
                    amf::AMFVariant(static_cast<amf_int64>(AMF_COLOR_PRIMARIES_BT709)));
            setProp(encoder, AMF_VIDEO_ENCODER_HEVC_OUTPUT_MATRIX_COEFF,
                    amf::AMFVariant(static_cast<amf_int64>(AMF_COLOR_MATRIX_COEFF_BT_709)));
            setProp(encoder, AMF_VIDEO_ENCODER_HEVC_OUTPUT_FULL_RANGE_COLOR,
                    amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_NOMINAL_RANGE_STUDIO)));
        } else {
            setProp(encoder, AMF_VIDEO_ENCODER_OUTPUT_TRANSFER_CHARACTERISTIC,
                    amf::AMFVariant(static_cast<amf_int64>(AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709)));
            setProp(encoder, AMF_VIDEO_ENCODER_OUTPUT_COLOR_PRIMARIES,
                    amf::AMFVariant(static_cast<amf_int64>(AMF_COLOR_PRIMARIES_BT709)));
            setProp(encoder, AMF_VIDEO_ENCODER_OUTPUT_MATRIX_COEFF,
                    amf::AMFVariant(static_cast<amf_int64>(AMF_COLOR_MATRIX_COEFF_BT_709)));
            setProp(encoder, AMF_VIDEO_ENCODER_OUTPUT_FULL_RANGE_COLOR, amf::AMFVariant(false));
        }
    }

    bool loadFactory() {
        // System32 only, matching the NVENC loader: no app-directory search.
        module = LoadLibraryExW(AMF_DLL_NAME, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) {
            caps.detail = L"amfrt64.dll was not found. Install a current AMD Adrenalin driver.";
            return false;
        }

        auto queryVersion =
            reinterpret_cast<AMFQueryVersion_Fn>(GetProcAddress(module, AMF_QUERY_VERSION_FUNCTION_NAME));
        auto init = reinterpret_cast<AMFInit_Fn>(GetProcAddress(module, AMF_INIT_FUNCTION_NAME));
        if (!queryVersion || !init) {
            caps.detail = L"amfrt64.dll did not export the AMF entry points.";
            return false;
        }

        queryVersion(&runtimeVersion);

        AMF_RESULT result = init(AMF_FULL_VERSION, &factory);
        if (result != AMF_OK || !factory) {
            caps.detail = L"AMFInit failed: " + amfResultName(result);
            return false;
        }
        return true;
    }

    bool openContext(ID3D11Device* device) {
        AMF_RESULT result = factory->CreateContext(&context);
        if (result != AMF_OK || !context) {
            caps.detail = L"AMFFactory::CreateContext failed: " + amfResultName(result);
            return false;
        }

        result = context->InitDX11(device);
        if (result != AMF_OK) {
            caps.detail = L"AMFContext::InitDX11 failed: " + amfResultName(result);
            return false;
        }
        return true;
    }

    void updateCapabilities(const std::wstring& adapterName) {
        // Capability values only: availability is published once the whole
        // component is configured (publishSessionActive).
        caps.adapterName = adapterName;
        caps.backendName = L"AMF";
        caps.h264 = true;
        caps.hevc = true;
        caps.bFrames = false;
        caps.multipleReferenceFrames = false;
        caps.lookahead = false;
        bFramesUnavailable = false;
        preAnalysisUnavailable = false;

        amf::AMFCapsPtr encoderCaps;
        if (encoder && encoder->GetCaps(&encoderCaps) == AMF_OK && encoderCaps) {
            const wchar_t* bframeCap = isHevc() ? nullptr : AMF_VIDEO_ENCODER_CAP_BFRAMES;
            if (bframeCap) {
                amf::AMFVariant value;
                if (encoderCaps->GetProperty(bframeCap, &value) == AMF_OK &&
                    (value.type == amf::AMF_VARIANT_BOOL || value.type == amf::AMF_VARIANT_INT64)) {
                    caps.bFrames = value.ToBool();
                    bFramesUnavailable = !caps.bFrames;
                }
            }
            const wchar_t* maxRefCap = isHevc()
                ? AMF_VIDEO_ENCODER_HEVC_CAP_MAX_REFERENCE_FRAMES
                : AMF_VIDEO_ENCODER_CAP_MAX_REFERENCE_FRAMES;
            amf::AMFVariant refValue;
            if (encoderCaps->GetProperty(maxRefCap, &refValue) == AMF_OK) {
                caps.multipleReferenceFrames = refValue.ToInt64() > 1;
            }
            const wchar_t* paCap = isHevc()
                ? AMF_VIDEO_ENCODER_HEVC_CAP_PRE_ANALYSIS
                : AMF_VIDEO_ENCODER_CAP_PRE_ANALYSIS;
            amf::AMFVariant paValue;
            if (encoderCaps->GetProperty(paCap, &paValue) == AMF_OK &&
                (paValue.type == amf::AMF_VARIANT_BOOL || paValue.type == amf::AMF_VARIANT_INT64)) {
                preAnalysisUnavailable = !paValue.ToBool();
            }

            amf::AMFIOCapsPtr inputCaps;
            if (encoderCaps->GetInputCaps(&inputCaps) == AMF_OK && inputCaps) {
                amf_int32 minWidth = 0;
                amf_int32 maxWidth = 0;
                amf_int32 minHeight = 0;
                amf_int32 maxHeight = 0;
                inputCaps->GetWidthRange(&minWidth, &maxWidth);
                inputCaps->GetHeightRange(&minHeight, &maxHeight);
                caps.maxWidth = static_cast<uint32_t>(maxWidth);
                caps.maxHeight = static_cast<uint32_t>(maxHeight);
            }
        }

        // Main 8-bit only; Main10 is not configured.
        caps.tenBitHevc = false;
    }

    bool encodeSizeSupported() const {
        if (caps.maxWidth > 0 && settings.width > caps.maxWidth) {
            return false;
        }
        if (caps.maxHeight > 0 && settings.height > caps.maxHeight) {
            return false;
        }
        return settings.width >= 16 && settings.height >= 16 &&
               (settings.width % 2) == 0 && (settings.height % 2) == 0;
    }

    void publishEffective(EncoderEffectiveSettings effective) {
        // report unconfigured features off rather than echoing the request.
        effective.temporalAQ = false;
        effective.multipass = EncoderMultipass::Disabled;
        effective.adaptiveBFrames = false;
        effective.adaptiveIFrames = false;
        // AMF has no lossless mode and no separate ultra high quality usage:
        // both map onto the transcoding usage (avcUsage/hevcUsage), which is
        // the high quality configuration. Report that instead of the request.
        if (effective.mode == EncoderMode::Lossless ||
            effective.mode == EncoderMode::UltraHighQuality) {
            effective.mode = EncoderMode::HighQuality;
        }
        if (!caps.multipleReferenceFrames) {
            effective.referenceFrames = 1;
        }
        effective.lookahead = false;
        effective.lookaheadDepth = 0;
        caps.effective = effective;
        // Each accepted input keeps its scaler-pool lease until its output is
        // harvested, so the retention budget is the negotiated input cap the
        // backlog check in encode() enforces.
        retentionBudget.store(
            encoder && !encoderFaulted ? static_cast<uint32_t>(inFlightCap()) : 0,
            std::memory_order_release);
    }

    bool configureAvc(const EncoderEffectiveSettings& effective) {
        const int64_t bitrate = static_cast<int64_t>(settings.bitrateKbps) * 1000;
        const uint32_t idrPeriod = std::max<uint32_t>(1, settings.fps * settings.gopSeconds);

        setProp(encoder, AMF_VIDEO_ENCODER_USAGE, amf::AMFVariant(static_cast<amf_int64>(avcUsage(effective.mode))));
        setProp(encoder, AMF_VIDEO_ENCODER_QUALITY_PRESET,
                amf::AMFVariant(static_cast<amf_int64>(avcQualityPreset(effective.preset))));
        setProp(encoder, AMF_VIDEO_ENCODER_PROFILE,
                amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_PROFILE_HIGH)));
        setProp(encoder, AMF_VIDEO_ENCODER_FRAMESIZE,
                amf::AMFVariant(AMFConstructSize(settings.width, settings.height)));
        const bool gopApplied =
            setProp(encoder, AMF_VIDEO_ENCODER_IDR_PERIOD, amf::AMFVariant(static_cast<amf_int64>(idrPeriod)));
        // Insert SPS/PPS periodically (AMF range 0-1000). Forced IDRs also set InsertSPS/PPS.
        setProp(encoder, AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING,
                amf::AMFVariant(static_cast<amf_int64>(std::min<uint32_t>(idrPeriod, 1000))));

        // B-frames reorder output; keep them off when zero-latency/idle coalescing is desired.
        const bool useBFrames = effective.bFrames && caps.bFrames && !effective.zeroReorderDelay;
        const bool bPatternApplied = setProp(encoder, AMF_VIDEO_ENCODER_B_PIC_PATTERN,
                amf::AMFVariant(static_cast<amf_int64>(useBFrames ? 2 : 0)));
        const bool preAnalysisDisabled = setProp(encoder, AMF_VIDEO_ENCODER_PRE_ANALYSIS_ENABLE,
                amf::AMFVariant(false));

        applyColorProperties(true);

        // Init with the actual input surface format. The capture pipeline feeds
        // NV12 (BT.709 limited) textures converted by the D3D11 video processor;
        // VCN hardware consumes NV12 natively, so the AMF runtime's internal
        // BGRA->NV12 conversion pass is bypassed entirely. A scaler that had to
        // fall back to BGRA at runtime switches the component format through
        // switchInputFormat() below instead of paying the per-frame conversion.
        AMF_RESULT result = encoder->Init(inputSurfaceFormat,
                                          static_cast<amf_int32>(settings.width),
                                          static_cast<amf_int32>(settings.height));
        if (result != AMF_OK) {
            caps.detail = L"AMF AVC encoder Init failed: " + amfResultName(result);
            return false;
        }
        applyColorProperties(false);

        // Dynamic rate-control properties applied after Init so USAGE presets do
        // not overwrite them.
        setProp(encoder, AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
                amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR)));
        const bool bitrateApplied =
            setProp(encoder, AMF_VIDEO_ENCODER_TARGET_BITRATE, amf::AMFVariant(bitrate));
        setProp(encoder, AMF_VIDEO_ENCODER_PEAK_BITRATE, amf::AMFVariant(bitrate));
        setProp(encoder, AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, amf::AMFVariant(bitrate));
        setProp(encoder, AMF_VIDEO_ENCODER_FILLER_DATA_ENABLE, amf::AMFVariant(false));
        setProp(encoder, AMF_VIDEO_ENCODER_ENFORCE_HRD, amf::AMFVariant(true));
        const bool framerateApplied = setProp(encoder, AMF_VIDEO_ENCODER_FRAMERATE,
                amf::AMFVariant(AMFConstructRate(settings.fps, 1)));
        if (!gopApplied || !bitrateApplied || !framerateApplied) {
            caps.detail = driverVersionHint();
            return false;
        }
        setProp(encoder, AMF_VIDEO_ENCODER_ENABLE_VBAQ, amf::AMFVariant(effective.spatialAQ));

        const uint32_t refFrames = caps.multipleReferenceFrames
            ? std::clamp<uint32_t>(effective.referenceFrames, 1, 4)
            : 1;
        setProp(encoder, AMF_VIDEO_ENCODER_MAX_NUM_REFRAMES, amf::AMFVariant(static_cast<amf_int64>(refFrames)));
        if (!establishAvcBFrameDelay(useBFrames, bPatternApplied) ||
            !confirmPreAnalysisDisabled(AMF_VIDEO_ENCODER_PRE_ANALYSIS_ENABLE, preAnalysisDisabled)) {
            return false;
        }

        auto applied = effective;
        applied.bFrames = bFrameDelayFrames != 0;
        applied.zeroReorderDelay = bFrameDelayFrames == 0;
        publishEffective(applied);
        return true;
    }

    bool configureHevc(const EncoderEffectiveSettings& effective) {
        const int64_t bitrate = static_cast<int64_t>(settings.bitrateKbps) * 1000;
        const uint32_t gopSize = std::max<uint32_t>(1, settings.fps * settings.gopSeconds);

        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_USAGE,
                amf::AMFVariant(static_cast<amf_int64>(hevcUsage(effective.mode))));
        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET,
                amf::AMFVariant(static_cast<amf_int64>(hevcQualityPreset(effective.preset))));
        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_PROFILE,
                amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN)));
        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_TIER,
                amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_TIER_MAIN)));
        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_FRAMESIZE,
                amf::AMFVariant(AMFConstructSize(settings.width, settings.height)));
        const bool gopApplied =
            setProp(encoder, AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, amf::AMFVariant(static_cast<amf_int64>(gopSize)));
        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_NUM_GOPS_PER_IDR, amf::AMFVariant(static_cast<amf_int64>(1)));
        // VPS/SPS/PPS on every IDR for clean mid-session mux starts.
        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE,
                amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE_IDR_ALIGNED)));
        const bool preAnalysisDisabled = setProp(encoder, AMF_VIDEO_ENCODER_HEVC_PRE_ANALYSIS_ENABLE,
                amf::AMFVariant(false));

        applyColorProperties(true);

        AMF_RESULT result = encoder->Init(inputSurfaceFormat,
                                          static_cast<amf_int32>(settings.width),
                                          static_cast<amf_int32>(settings.height));
        if (result != AMF_OK) {
            caps.detail = L"AMF HEVC encoder Init failed: " + amfResultName(result);
            return false;
        }
        applyColorProperties(false);

        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD,
                amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR)));
        const bool bitrateApplied =
            setProp(encoder, AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, amf::AMFVariant(bitrate));
        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, amf::AMFVariant(bitrate));
        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, amf::AMFVariant(bitrate));
        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_FILLER_DATA_ENABLE, amf::AMFVariant(false));
        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_ENFORCE_HRD, amf::AMFVariant(true));
        const bool framerateApplied = setProp(encoder, AMF_VIDEO_ENCODER_HEVC_FRAMERATE,
                amf::AMFVariant(AMFConstructRate(settings.fps, 1)));
        if (!gopApplied || !bitrateApplied || !framerateApplied) {
            caps.detail = driverVersionHint();
            return false;
        }
        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_ENABLE_VBAQ, amf::AMFVariant(effective.spatialAQ));
        const uint32_t refFrames = caps.multipleReferenceFrames
            ? std::clamp<uint32_t>(effective.referenceFrames, 1, 4)
            : 1;
        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_MAX_NUM_REFRAMES,
                amf::AMFVariant(static_cast<amf_int64>(refFrames)));
        if (!confirmPreAnalysisDisabled(AMF_VIDEO_ENCODER_HEVC_PRE_ANALYSIS_ENABLE, preAnalysisDisabled)) {
            return false;
        }
        bFrameDelayFrames = 0;

        auto applied = effective;
        applied.bFrames = false; // AMF HEVC has no B-frame pattern control here.
        applied.zeroReorderDelay = true;
        publishEffective(applied);
        return true;
    }

    void updateActiveDetail(const EncoderEffectiveSettings& requested) {
        caps.detail = std::wstring(L"AMF ") + (isHevc() ? L"HEVC" : L"AVC") +
                      L" Direct3D 11 session active";
        if (requested.lookahead) {
            caps.detail += L"; lookahead is unsupported by the AMF backend";
        }
        if (requested.mode == EncoderMode::Lossless) {
            caps.detail += L"; AMF has no lossless mode, the high quality configuration is used";
        }
        if (requested.mode == EncoderMode::UltraHighQuality) {
            caps.detail += L"; AMF has no ultra high quality mode, the high quality configuration is used";
        }
        const bool bFramesDropped = requested.bFrames && !requested.zeroReorderDelay && !caps.effective.bFrames;
        if (bFramesDropped) {
            caps.detail += L"; B-frames are unavailable";
        }
        if (requested.lookahead || bFramesDropped) {
            Logger::instance().warning(L"encoder", caps.detail);
        }
    }

    bool createComponent() {
        const wchar_t* componentId = isHevc() ? AMFVideoEncoder_HEVC : AMFVideoEncoderVCE_AVC;
        AMF_RESULT result = factory->CreateComponent(context, componentId, &encoder);
        if (result != AMF_OK || !encoder) {
            caps.detail = std::wstring(L"AMFFactory::CreateComponent(") +
                          (isHevc() ? L"HEVC" : L"AVC") + L") failed: " + amfResultName(result);
            return false;
        }

        return true;
    }

    bool initializeEncoder(ID3D11Device* device, const std::wstring& adapterName) {
        if (!loadFactory() || !openContext(device) || !createComponent()) {
            return false;
        }

        // Query caps before Init so B-frame / reference-frame support is known.
        updateCapabilities(adapterName);

        if (!encodeSizeSupported()) {
            caps.detail =
                L"Encode size " + std::to_wstring(settings.width) + L"x" +
                std::to_wstring(settings.height) +
                L" is outside AMF limits (max " +
                std::to_wstring(caps.maxWidth) + L"x" + std::to_wstring(caps.maxHeight) +
                L", even dimensions required)";
            return false;
        }

        const EncoderEffectiveSettings effective = effectiveSettingsFor(settings);
        const bool configured = isHevc() ? configureHevc(effective) : configureAvc(effective);
        if (!configured) {
            return false;
        }

        // Detect drivers that accept properties yet drop them at Init: the
        // user's bitrate/GOP/framerate must read back unchanged, otherwise the
        // session would silently encode with preset defaults.
        if (!verifyCriticalSettings()) {
            caps.detail = driverVersionHint();
            return false;
        }
        // Same class of check for the SPS/PPS header insertion; warning-only
        // because header-less keyframes are patched from the cached parameter
        // sets at harvest time.
        verifyHeaderInsertionSettings();

        // Use the pattern established on the initialized component.
        decodeTimestamps.reset(reorderDelayFrames());
        cachedParameterSets.clear();

        caps.available = true;
        updateActiveDetail(effective);
        publishSessionActive();

        Logger::instance().info(L"encoder",
            std::wstring(L"AMF ") + (isHevc() ? L"HEVC" : L"AVC") +
            L" encoder initialized in zero-copy D3D11 mode at " +
            std::to_wstring(settings.width) + L"x" + std::to_wstring(settings.height) +
            L", codec=" + codecName(settings.codec) +
            L", bitrateKbps=" + std::to_wstring(settings.bitrateKbps) +
            L", gopSeconds=" + std::to_wstring(settings.gopSeconds) +
            L", preset=" + encoderPresetName(caps.effective.preset) +
            L", mode=" + encoderModeName(caps.effective.mode) +
            L", bFrames=" + (caps.effective.bFrames ? L"yes" : L"no") +
            L", lookahead=" + (caps.effective.lookahead ? L"yes" : L"no"));
        return true;
    }

    void setForceKeyFrameOn(amf::AMFSurface* surface) {
        if (isHevc()) {
            surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE,
                                 amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR)));
            surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_INSERT_HEADER, amf::AMFVariant(true));
        } else {
            surface->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE,
                                 amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR)));
            surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, amf::AMFVariant(true));
            surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, amf::AMFVariant(true));
        }
    }

    bool outputIsKeyFrame(amf::AMFBuffer* buffer) const {
        amf::AMFVariant value;
        if (isHevc()) {
            if (buffer->GetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE, &value) == AMF_OK) {
                return value.ToInt64() == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR;
            }
        } else {
            if (buffer->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &value) == AMF_OK) {
                return value.ToInt64() == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR;
            }
        }
        return false;
    }

    bool fail(const std::wstring& detail) {
        encoderFaulted = true;
        caps.available = false;
        caps.detail = detail;
        publishAvailability(false);
        Logger::instance().error(L"encoder", detail);
        shutdownEncoder();
        return false;
    }

    // Re-creates the AMF component for a different input surface format (NV12
    // <-> BGRA). Needed when the capture scaler's NV12 output path fails at
    // runtime on some driver/adapter combinations and it falls back to BGRA
    // for the rest of the session: the frames keep flowing in the new format,
    // so the encoder must follow instead of dropping every frame until an
    // external pipeline recreate. Ready outputs are harvested from the old
    // component first (mirrors the NVENC EOS flush); only inputs with no
    // output yet are dropped, and a forced IDR afterwards restarts the stream
    // from a clean reference state.
    bool switchInputFormat(amf::AMF_SURFACE_FORMAT format) {
        if (recreateComponentForFormat(format)) {
            return true;
        }
        // A half re-created component must not keep reporting a live session;
        // the caller escalates to the terminal fault.
        caps.available = false;
        publishAvailability(false);
        return false;
    }

    bool recreateComponentForFormat(amf::AMF_SURFACE_FORMAT format) {
        if (!factory || !context) {
            return false;
        }
        const bool previouslyZeroReorder = caps.effective.zeroReorderDelay;
        Logger::instance().warning(L"encoder",
            std::wstring(L"AMF encoder input format changed to ") +
            (format == amf::AMF_SURFACE_NV12 ? L"NV12" : L"BGRA") +
            L"; re-initializing the encoder component");
        // Drain what the old component already finished so the backlog is not
        // dropped with it; readyPackets stay queued across the re-creation.
        // Only inputs the old component never produced output for are released
        // (their surfaces and capture leases) below.
        if (!harvestOutputs(false)) {
            return false;
        }
        inFlight.clear();
        encoder->Terminate();
        encoder = nullptr;

        inputSurfaceFormat = format;
        if (!createComponent()) {
            Logger::instance().error(L"encoder", caps.detail);
            return false;
        }
        updateCapabilities(caps.adapterName);

        const EncoderEffectiveSettings effective = effectiveSettingsFor(settings);
        const bool configured = isHevc() ? configureHevc(effective) : configureAvc(effective);
        if (!configured || !verifyCriticalSettings()) {
            Logger::instance().error(L"encoder",
                std::wstring(L"AMF re-initialization after input-format change failed: ") + caps.detail);
            return false;
        }
        // The capture loop caches this flag until the pipeline is recreated.
        if (previouslyZeroReorder && !caps.effective.zeroReorderDelay) {
            return fail(L"AMF input-format change enabled B-frames; encoder faulted so the capture pipeline can recreate it");
        }

        decodeTimestamps.reset(reorderDelayFrames());
        cachedParameterSets.clear();
        updateActiveDetail(effective);
        publishSessionActive();
        consecutiveSubmitFailures = 0;
        forceKeyFrame = true;
        return true;
    }

    bool popReadyPacket(EncodedPacket& packet) {
        if (readyPackets.empty()) {
            return false;
        }
        packet = std::move(readyPackets.front());
        readyPackets.pop_front();
        return true;
    }

    // Vector form of encode: encode() harvests into readyPackets and pops at
    // most one, so move the remainder too. A submission that completes several
    // outputs must not strand its tail behind a single-packet return.
    void encodeDrain(const GpuFrame& frame, std::vector<EncodedPacket>& out) {
        EncodedPacket packet;
        if (encode(frame, packet)) {
            out.push_back(std::move(packet));
        }
        EncodedPacket rest;
        while (popReadyPacket(rest)) {
            out.push_back(std::move(rest));
        }
    }

    // Retrieves any available encoded output. Non-blocking unless waitForAll
    // (final drain). While draining, some drivers answer NEED_MORE_INPUT or
    // REPEAT until the last packets are flushed; polling continues as long as
    // output keeps arriving so trailing frames are not lost, bounded by an
    // idle deadline so a wedged driver cannot hang clip saving. Unexpected
    // QueryOutput errors during a drain are logged without faulting the
    // session - Drain already reported its own status at that point.
    bool harvestOutputs(bool waitForAll) {
        constexpr auto kIdleDrainLimit = kEncoderOutputTimeout;
        // Non-drain sweeps run on the encode path while impl_->mutex is held;
        // a driver answering AMF_OK with buffers that never yield a packet
        // (non-buffer Data, zero-size payload) must not be able to spin here
        // unboundedly or stop/save/pipeline-recreate hang behind that lock.
        constexpr auto kIdleHarvestLimit = std::chrono::milliseconds(250);
        SteadyClock::time_point lastProgress = SteadyClock::now();

        while (true) {
            // Unified idle gate: progress means a packet was actually produced.
            // Drain waits up to kIdleDrainLimit for trailing frames; a
            // steady-state sweep must return promptly (kIdleHarvestLimit)
            // because impl_->mutex is held throughout.
            const auto idleLimit = waitForAll ? kIdleDrainLimit : kIdleHarvestLimit;
            if (SteadyClock::now() - lastProgress >= idleLimit) {
                Logger::instance().warning(L"encoder",
                    std::wstring(waitForAll
                        ? L"AMF encoder stopped producing output during drain"
                        : L"AMF encoder produced no usable output within the harvest window") +
                    L"; keeping " + std::to_wstring(readyPackets.size()) + L" harvested packet(s)");
                break;
            }

            amf::AMFDataPtr data;
            AMF_RESULT result = encoder->QueryOutput(&data);

            if (result == AMF_EOF) {
                break;
            }
            // NEED_MORE_INPUT means no packet is ready yet (some drivers emit
            // it instead of REPEAT); treat both as "no output this round".
            if (result == AMF_REPEAT || result == AMF_NEED_MORE_INPUT ||
                (result == AMF_OK && !data)) {
                if (!waitForAll) {
                    break;
                }
                // The loop-top idle gate bounds this polling; sleep so waiting
                // out a slow flush does not spin the core.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (result != AMF_OK) {
                const std::wstring detail = L"AMF QueryOutput failed: " + amfResultName(result);
                if (waitForAll) {
                    Logger::instance().error(L"encoder", detail);
                    break;
                }
                return fail(detail);
            }

            amf::AMFBufferPtr buffer(data);
            if (!buffer) {
                continue;
            }

            const int64_t outputPts = static_cast<int64_t>(buffer->GetPts());
            const int64_t outputDuration = static_cast<int64_t>(buffer->GetDuration());
            // Gate PTS matching on nonzero output: a zero echo must not match
            // a legitimately-zero submission PTS below. This gates matching
            // only; timestamp validity is the explicit hasDriverPts passed to
            // finalizePacket (GetPts echoes the SetPts every submit carries).
            const bool canMatchByPts = outputPts != 0;

            // Match by PTS when B-frames reorder output; the oldest matching
            // entry wins so duplicate timestamps keep decode order. When no
            // entry matches, the FIFO front is popped anyway (its surface and
            // capture lease must be released), but driver timestamps are kept
            // whenever present instead of splicing unrelated metadata in.
            PendingInput pending;
            bool matched = false;
            if (canMatchByPts) {
                for (auto it = inFlight.begin(); it != inFlight.end(); ++it) {
                    if (it->fallbackPts100ns == outputPts) {
                        pending = std::move(*it);
                        inFlight.erase(it);
                        matched = true;
                        break;
                    }
                }
            }
            if (!matched && !inFlight.empty()) {
                pending = std::move(inFlight.front());
                inFlight.pop_front();
            }

            EncodedPacket packet;
            const auto* begin = static_cast<const uint8_t*>(buffer->GetNative());
            const size_t size = buffer->GetSize();
            // Only the OUTPUT_DATA_TYPE read stays backend-specific;
            // timestamps, keyframe rule, headers, and stats live in
            // finalizePacket. hasDriverPts is explicit: every submit carries
            // SetPts, so the echo is always present (zero stays a legal
            // timestamp, never a sentinel).
            const bool metadataIdr = outputIsKeyFrame(buffer);
            const bool finalized = finalizePacket(
                settings.codec, begin, size,
                outputPts, true, outputDuration,
                pending.fallbackPts100ns, pending.fallbackDuration100ns,
                metadataIdr, decodeTimestamps, cachedParameterSets,
                encoderStats, pending.submittedAt, packet);

            if (!finalized) {
                // The input produced no usable payload: it is gone without an
                // output, exactly like a dropped frame.
                noteDroppedInput();
                continue;
            }
            readyPackets.push_back(std::move(packet));
            // Progress means a packet was actually produced: refresh here, not
            // on any AMF_OK response, so a driver looping on unusable buffers
            // trips the idle bound above instead of resetting it forever.
            lastProgress = SteadyClock::now();
        }
        return true;
    }

    bool encode(const GpuFrame& frame, EncodedPacket& packet) {
        if (encoderFaulted || !encoder || !frame.texture) {
            return popReadyPacket(packet);
        }
        if (frame.width != settings.width || frame.height != settings.height) {
            Logger::instance().warning(L"encoder", L"Frame size changed; dropping frame until capture pipeline is recreated");
            noteDroppedInput();
            return popReadyPacket(packet);
        }

        // Surface format must match what the component was initialized with.
        // A scaler fallback to BGRA mid-session lands here once and re-creates
        // the component for the new format instead of dropping every frame
        // until an external recreate (see switchInputFormat). Flapping cannot
        // occur: the sticky nv12Unavailable_ flag keeps one format per scaler
        // session, so this fires at most once per source rebind.
        const amf::AMF_SURFACE_FORMAT wantedFormat =
            frame.format == DXGI_FORMAT_NV12 ? amf::AMF_SURFACE_NV12 : amf::AMF_SURFACE_BGRA;
        if (wantedFormat != inputSurfaceFormat) {
            if (!switchInputFormat(wantedFormat)) {
                if (encoderFaulted) {
                    return false;
                }
                return fail(std::wstring(L"AMF encoder could not follow the capture input-format change (") +
                            caps.detail + L"); encoder faulted so the capture pipeline can recreate it");
            }
        }
        // Bounded input backlog: before accepting another surface, give the
        // driver one chance to produce output. If the backlog is still full,
        // shed the incoming frame (drop-newest, matching the frame-queue
        // policy). This happens before CreateSurfaceFromDX11Native so nothing
        // is pinned by a frame that will be dropped. Returning early also
        // leaves any pending force-keyframe request armed for the next frame.
        const size_t maxInFlight = inFlightCap();
        if (inFlight.size() >= maxInFlight && !harvestOutputs(false)) {
            return false;
        }

        // Hard lifetime for accepted-but-unharvested frames: a healthy driver
        // drains within a few frame intervals, so an input this old means the
        // encoder stopped producing output. Fault the session (the capture
        // loop recreates it) instead of querying a dead component forever.
        // Sits after the harvest above and before the drop below so a full
        // backlog cannot shed new frames forever while the oldest one ages out.
        if (!inFlight.empty() &&
            SteadyClock::now() - inFlight.front().submittedAt >= kInFlightMaxAge) {
            return fail(
                L"AMF encoder produced no output for a submitted frame within " +
                std::to_wstring(std::chrono::duration_cast<std::chrono::seconds>(kInFlightMaxAge).count()) +
                L"s; encoder faulted so the capture pipeline can recreate it");
        }
        if (inFlight.size() >= maxInFlight) {
            noteDroppedInput();
            ++inFlightDropCount;
            if (inFlightDropCount == 1 || (inFlightDropCount % 300) == 0) {
                Logger::instance().warning(L"encoder",
                    L"AMF encoder output starved; input backlog full - dropping frames count=" +
                    std::to_wstring(inFlightDropCount));
            }
            return popReadyPacket(packet);
        }

        const auto start = std::chrono::steady_clock::now();

        // AMF submit failures log on the 1st and every 300th consecutive one;
        // the 30th escalates to a full session fault via fail().
        auto noteSubmitFailure = [&](const wchar_t* what, AMF_RESULT r) {
            const uint32_t failures = ++consecutiveSubmitFailures;
            if (failures == 1 || (failures % 300) == 0) {
                Logger::instance().warning(L"encoder",
                    std::wstring(L"AMF ") + what + L" failed: " + amfResultName(r) +
                    L" (count=" + std::to_wstring(failures) + L")");
            }
            return failures == kEncoderMaxConsecutiveFailures;
        };

        amf::AMFSurfacePtr surface;
        AMF_RESULT result = context->CreateSurfaceFromDX11Native(frame.texture.Get(), &surface, nullptr);
        if (result != AMF_OK || !surface) {
            // Frame lost before reaching the encoder; restart the next GOP so
            // the stream never continues from a broken reference chain.
            forceKeyFrame = true;
            noteDroppedInput();
            if (noteSubmitFailure(L"CreateSurfaceFromDX11Native", result)) {
                return fail(L"AMF CreateSurfaceFromDX11Native failed repeatedly: " + amfResultName(result));
            }
            return popReadyPacket(packet);
        }

        surface->SetPts(static_cast<amf_pts>(frame.pts100ns));
        const int64_t duration100ns = frame.duration100ns > 0
            ? frame.duration100ns
            : static_cast<int64_t>(kHundredNanosecondsPerSecond / std::max<uint32_t>(settings.fps, 1));
        surface->SetDuration(static_cast<amf_pts>(duration100ns));

        const bool requestedKeyFrame = forceKeyFrame.exchange(false);
        if (requestedKeyFrame) {
            setForceKeyFrameOn(surface);
        }

        result = encoder->SubmitInput(surface);
        if (result == AMF_INPUT_FULL) {
            // Drain output and retry a few times before dropping the frame.
            bool submitted = false;
            for (int attempt = 0; attempt < 8 && !submitted; ++attempt) {
                if (!harvestOutputs(false)) {
                    forceKeyFrame = true;
                    return false;
                }
                result = encoder->SubmitInput(surface);
                if (result == AMF_OK || result == AMF_NEED_MORE_INPUT) {
                    submitted = true;
                    break;
                }
                if (result != AMF_INPUT_FULL) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!submitted) {
                // The frame was never accepted; restart the next GOP so the
                // stream continues from a clean reference state.
                forceKeyFrame = true;
                noteDroppedInput();
                if (result == AMF_INPUT_FULL) {
                    return popReadyPacket(packet);
                }
                if (noteSubmitFailure(L"SubmitInput", result)) {
                    return fail(L"AMF SubmitInput failed repeatedly: " + amfResultName(result));
                }
                return popReadyPacket(packet);
            }
        } else if (result != AMF_OK && result != AMF_NEED_MORE_INPUT) {
            // The frame was never accepted; restart the next GOP so the
            // stream continues from a clean reference state.
            forceKeyFrame = true;
            noteDroppedInput();
            if (noteSubmitFailure(L"SubmitInput", result)) {
                return fail(L"AMF SubmitInput failed repeatedly: " + amfResultName(result));
            }
            return popReadyPacket(packet);
        }

        consecutiveSubmitFailures = 0;
        ++encoderStats.submittedFrames;

        // Accepted input: AMF produces exactly one output packet for it, in
        // decode order. Record the submission slot for decode-timestamp
        // assignment.
        decodeTimestamps.onSubmit(frame.pts100ns, duration100ns);

        PendingInput pending;
        pending.surface = surface;
        pending.lease = frame.lease;
        pending.fallbackPts100ns = frame.pts100ns;
        pending.fallbackDuration100ns = duration100ns;
        pending.submittedAt = start;
        inFlight.push_back(std::move(pending));

        if (!harvestOutputs(false)) {
            return false;
        }
        return popReadyPacket(packet);
    }

    void drain(std::vector<EncodedPacket>& packets) {
        while (!readyPackets.empty()) {
            packets.push_back(std::move(readyPackets.front()));
            readyPackets.pop_front();
        }
        if (!encoder || encoderFaulted) {
            return;
        }

        // Bound the blocking driver-side flush (kDrainTimeout). Drain() runs
        // on a helper thread; on timeout the worker is detached with its own
        // reference to the component and this session is faulted so the
        // capture pipeline recreates it instead of wedging every caller that
        // holds pipelineGpuMutex_.
        std::promise<AMF_RESULT> drainPromise;
        auto drainFuture = drainPromise.get_future();
        amf::AMFComponentPtr drainingComponent = encoder;
        std::thread drainWorker(
            [promise = std::move(drainPromise), component = drainingComponent]() mutable {
                try {
                    promise.set_value(component ? component->Drain() : AMF_OK);
                } catch (...) {
                    // Never leave the promise broken: get() below must either
                    // yield a value or rethrow after join().
                    promise.set_exception(std::current_exception());
                }
            });
        if (drainFuture.wait_for(kDrainTimeout) == std::future_status::timeout) {
            drainWorker.detach();
            drainAbandoned = true;
            encoderFaulted = true;
            caps.available = false;
            caps.detail =
                L"AMF encoder did not finish flushing within " +
                std::to_wstring(std::chrono::duration_cast<std::chrono::seconds>(kDrainTimeout).count()) +
                L"s; capture pipeline will recreate the encoder";
            publishAvailability(false);
            Logger::instance().error(L"encoder", caps.detail);
        } else {
            // Join before anything else so the thread object can never be
            // destroyed while still joinable (that calls terminate()).
            drainWorker.join();
            AMF_RESULT result = AMF_OK;
            try {
                result = drainFuture.get();
            } catch (const std::exception& error) {
                Logger::instance().error(L"encoder",
                    std::wstring(L"AMF Drain raised an exception: ") + utf8ToWide(error.what()));
            } catch (...) {
                Logger::instance().error(L"encoder", L"AMF Drain raised an unknown exception");
            }
            if (result == AMF_NEED_MORE_INPUT) {
                // Some Adrenalin builds end the drain with NEED_MORE_INPUT even
                // though outputs are still pending.
                Logger::instance().info(L"encoder",
                    L"AMF Drain ended with NEED_MORE_INPUT; harvesting pending outputs");
            } else if (result != AMF_OK && result != AMF_EOF) {
                Logger::instance().error(L"encoder", L"AMF Drain failed: " + amfResultName(result));
            }
        }
        // Harvest regardless of the drain status code: skipping this loses
        // every output still queued in the encoder (the final seconds of the
        // stream). QueryOutput is non-blocking and harvestOutputs() is
        // idle-bounded. After an abandoned flush the component is presumed
        // wedged, so only a single non-blocking sweep runs instead of polling
        // a dead encoder for kIdleDrainLimit.
        harvestOutputs(!drainAbandoned);
        while (!readyPackets.empty()) {
            packets.push_back(std::move(readyPackets.front()));
            readyPackets.pop_front();
        }
    }

    void shutdownEncoder() {
        inFlight.clear();
        retentionBudget.store(0, std::memory_order_release);
        decodeTimestamps.reset(0);
        if (drainAbandoned && (encoder || context)) {
            // An abandoned Drain() may still be executing inside the driver.
            // Terminating the component now would race it, so skip explicit
            // teardown: the detached worker holds references to the AMF
            // objects and releases them only after Drain() finally returns.
            // The runtime DLL stays loaded for the same reason. This
            // deliberately leaks one wedged session's resources in exchange
            // for keeping clip saving and pipeline recreation alive.
            encoder = nullptr;
            context = nullptr;
            factory = nullptr;
            module = nullptr;
            drainAbandoned = false;
            return;
        }
        if (encoder) {
            encoder->Terminate();
            encoder = nullptr;
        }
        if (context) {
            context->Terminate();
            context = nullptr;
        }
        factory = nullptr;
        if (module) {
            FreeLibrary(module);
            module = nullptr;
        }
    }
};

#else

struct AmfEncoder::Impl {
    VideoSettings settings;
    EncoderCapabilities caps;
    EncoderStats encoderStats;
    mutable std::mutex mutex;
    mutable std::mutex snapshotMutex;
    EncoderStats statsSnapshot;
    EncoderCapabilities capsSnapshot;
    std::atomic<bool> forceKeyFrame{false};
};

#endif

AmfEncoder::AmfEncoder()
    : impl_(std::make_unique<Impl>()) {
}

AmfEncoder::~AmfEncoder() {
    shutdown();
}

bool AmfEncoder::initialize(D3DDevice& device, const VideoSettings& settings) {
    std::scoped_lock lock(impl_->mutex);
#if BACKTRACK_HAS_AMF
    impl_->shutdownEncoder();
#endif
    impl_->settings = settings;
    impl_->forceKeyFrame = true;
    impl_->caps = {};
    impl_->encoderStats = {};
#if BACKTRACK_HAS_AMF
    impl_->availableAtomic.store(false, std::memory_order_release);
    impl_->droppedInputAtomic.store(0, std::memory_order_relaxed);
#endif
    impl_->caps.adapterName = device.adapterName();
    impl_->caps.backendName = L"AMF";
#if BACKTRACK_HAS_AMF
    impl_->encoderFaulted = false;
    impl_->drainAbandoned = false;
    impl_->consecutiveSubmitFailures = 0;
    const bool initialized = impl_->initializeEncoder(device.device(), device.adapterName());
    if (!initialized) {
        // RecorderController's own failure line carries no reason, so this is the
        // only place the actionable detail reaches the log.
        Logger::instance().error(L"encoder", impl_->caps.detail);
        impl_->shutdownEncoder();
    }
    return initialized;
#else
    impl_->caps.available = false;
    impl_->caps.detail = L"Built without AMF headers. Bundle the AMF SDK headers and rebuild to enable AMD encoding.";
    impl_->encoderStats.encoderAvailable = false;
    Logger::instance().error(L"encoder", impl_->caps.detail);
    return false;
#endif
}

void AmfEncoder::requestKeyFrame() {
    impl_->forceKeyFrame = true;
}

bool AmfEncoder::resetInputResources() {
    // AMF holds owned scaler outputs, not capture-source frames or registrations.
    return true;
}

bool AmfEncoder::encodeFrame(const GpuFrame& frame, EncodedPacket& packet) {
    std::scoped_lock lock(impl_->mutex);
#if BACKTRACK_HAS_AMF
    return impl_->encode(frame, packet);
#else
    (void)frame;
    packet = {};
    ++impl_->encoderStats.droppedFrames;
    return false;
#endif
}

void AmfEncoder::encodeFrame(const GpuFrame& frame, std::vector<EncodedPacket>& packets) {
    std::scoped_lock lock(impl_->mutex);
    packets.clear();
#if BACKTRACK_HAS_AMF
    impl_->encodeDrain(frame, packets);
#endif
}

void AmfEncoder::drain(std::vector<EncodedPacket>& packets) {
    std::scoped_lock lock(impl_->mutex);
    packets.clear();
#if BACKTRACK_HAS_AMF
    impl_->drain(packets);
#endif
}

void AmfEncoder::shutdown() {
    std::scoped_lock lock(impl_->mutex);
#if BACKTRACK_HAS_AMF
    impl_->shutdownEncoder();
    impl_->availableAtomic.store(false, std::memory_order_release);
#endif
    impl_->caps.available = false;
    impl_->encoderStats.encoderAvailable = false;
}

bool AmfEncoder::encoderAvailable() const noexcept {
#if BACKTRACK_HAS_AMF
    return impl_->availableAtomic.load(std::memory_order_acquire);
#else
    return false;
#endif
}

EncoderCapabilities AmfEncoder::capabilities() const {
    std::unique_lock lock(impl_->mutex, std::try_to_lock);
    std::scoped_lock snap(impl_->snapshotMutex);
    if (lock.owns_lock()) {
        impl_->capsSnapshot = impl_->caps;
    }
    return impl_->capsSnapshot;
}

EncoderStats AmfEncoder::stats() const {
    std::unique_lock lock(impl_->mutex, std::try_to_lock);
    std::scoped_lock snap(impl_->snapshotMutex);
    if (lock.owns_lock()) {
        impl_->statsSnapshot = impl_->encoderStats;
    }
    return impl_->statsSnapshot;
}

uint32_t AmfEncoder::inputRetentionBudget() const {
#if BACKTRACK_HAS_AMF
    return impl_->retentionBudget.load(std::memory_order_acquire);
#else
    return 0;
#endif
}

uint64_t AmfEncoder::droppedInputs() const noexcept {
#if BACKTRACK_HAS_AMF
    return impl_->droppedInputAtomic.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

DXGI_FORMAT AmfEncoder::preferredInputFormat() const {
    // AMD VCN hardware encodes NV12 natively. The capture pipeline's D3D11
    // video processor performs the BGRA->NV12 (BT.709 limited) conversion as a
    // fixed-function pass, so the AMF runtime no longer runs its own compute
    // conversion on every frame. Scaler-side fallbacks cover the drivers where
    // NV12 conversion is unavailable (copied input views; sticky BGRA fallback
    // with a matching encoder re-initialization).
    return DXGI_FORMAT_NV12;
}

} // namespace backtrack
