#include "encoder/AmfEncoder.h"

#include "core/Logger.h"
#include "encoder/BitstreamUtils.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
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

// Maps the requested encoder preset (P1 fastest .. P7 slowest) to the nearest
// AMF quality preset. AMF exposes three effective steps: SPEED, BALANCED,
// QUALITY.
int avcQualityPreset(EncoderPreset preset) {
    switch (preset) {
    case EncoderPreset::P1:
    case EncoderPreset::P2:
        return AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED;
    case EncoderPreset::P3:
    case EncoderPreset::P4:
    case EncoderPreset::P5:
        return AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED;
    case EncoderPreset::P6:
    case EncoderPreset::P7:
        return AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY;
    }
    return AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED;
}

int hevcQualityPreset(EncoderPreset preset) {
    switch (preset) {
    case EncoderPreset::P1:
    case EncoderPreset::P2:
        return AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED;
    case EncoderPreset::P3:
    case EncoderPreset::P4:
    case EncoderPreset::P5:
        return AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED;
    case EncoderPreset::P6:
    case EncoderPreset::P7:
        return AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_QUALITY;
    }
    return AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED;
}

int avcUsage(EncoderMode mode) {
    switch (mode) {
    case EncoderMode::UltraLowLatency:
        return AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY;
    case EncoderMode::LowLatency:
        return AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY;
    case EncoderMode::HighQuality:
    case EncoderMode::UltraHighQuality:
        return AMF_VIDEO_ENCODER_USAGE_TRANSCODING;
    case EncoderMode::Lossless:
        return AMF_VIDEO_ENCODER_USAGE_TRANSCODING;
    }
    return AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY;
}

int hevcUsage(EncoderMode mode) {
    switch (mode) {
    case EncoderMode::UltraLowLatency:
        return AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY;
    case EncoderMode::LowLatency:
        return AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY;
    case EncoderMode::HighQuality:
    case EncoderMode::UltraHighQuality:
        return AMF_VIDEO_ENCODER_HEVC_USAGE_TRANSCODING;
    case EncoderMode::Lossless:
        return AMF_VIDEO_ENCODER_HEVC_USAGE_TRANSCODING;
    }
    return AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY;
}

EncoderEffectiveSettings effectiveSettingsFor(const VideoSettings& settings) {
    EncoderEffectiveSettings effective;
    effective.profile = settings.encoderProfile;

    switch (settings.encoderProfile) {
    case EncoderProfile::LowestGpu:
        effective.preset = EncoderPreset::P1;
        effective.mode = EncoderMode::UltraLowLatency;
        effective.zeroReorderDelay = true;
        effective.referenceFrames = 1;
        break;
    case EncoderProfile::Balanced:
        effective.preset = EncoderPreset::P3;
        effective.mode = EncoderMode::LowLatency;
        effective.zeroReorderDelay = true;
        effective.referenceFrames = 2;
        break;
    case EncoderProfile::Custom:
        effective.preset = settings.encoderPreset;
        effective.mode = settings.encoderMode;
        effective.lookahead = settings.encoderLookahead;
        effective.lookaheadDepth = std::min<uint32_t>(31, settings.encoderLookaheadDepth);
        effective.spatialAQ = settings.encoderSpatialAQ;
        effective.aqStrength = std::clamp<uint32_t>(settings.encoderAQStrength, 1, 15);
        effective.temporalAQ = settings.encoderTemporalAQ;
        effective.multipass = settings.encoderMultipass;
        effective.bFrames = settings.encoderBFrames;
        effective.adaptiveBFrames = settings.encoderAdaptiveBFrames;
        effective.adaptiveIFrames = settings.encoderAdaptiveIFrames;
        effective.zeroReorderDelay = settings.encoderZeroReorderDelay;
        effective.referenceFrames = std::clamp<uint32_t>(settings.encoderReferenceFrames, 1, 4);
        break;
    }

    if (!effective.lookahead) {
        effective.lookaheadDepth = 0;
        effective.adaptiveBFrames = false;
        effective.adaptiveIFrames = false;
    }
    if (!effective.spatialAQ) {
        effective.aqStrength = 0;
    }
    if (!effective.bFrames) {
        effective.adaptiveBFrames = false;
    }
    return effective;
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

    // Capture feeds BGRA D3D11 textures; AMF converts to NV12 internally.
    amf::AMF_SURFACE_FORMAT inputSurfaceFormat = amf::AMF_SURFACE_BGRA;

    std::atomic<bool> forceKeyFrame{true};

    struct PendingInput {
        amf::AMFSurfacePtr surface;
        std::shared_ptr<void> lease;
        ComPtr<ID3D11Texture2D> texture;
        int64_t fallbackPts100ns = 0;
        int64_t fallbackDuration100ns = 0;
        std::chrono::steady_clock::time_point submittedAt;
    };

    std::deque<PendingInput> inFlight;
    std::deque<EncodedPacket> readyPackets;
    bool encoderFaulted = false;
    uint32_t consecutiveSubmitFailures = 0;

    bool isHevc() const { return settings.codec == VideoCodec::Hevc; }

    void setProp(amf::AMFPropertyStorage* store, const wchar_t* name, const amf::AMFVariant& value) {
        if (store) {
            store->SetProperty(name, value);
        }
    }

    bool loadFactory() {
        module = LoadLibraryW(AMF_DLL_NAME);
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
        caps.available = encoder != nullptr;
        caps.adapterName = adapterName;
        caps.backendName = L"AMF";
        caps.h264 = true;
        caps.hevc = true;

        amf::AMFCapsPtr encoderCaps;
        if (encoder && encoder->GetCaps(&encoderCaps) == AMF_OK && encoderCaps) {
            const wchar_t* bframeCap = isHevc() ? nullptr : AMF_VIDEO_ENCODER_CAP_BFRAMES;
            if (bframeCap) {
                amf::AMFVariant value;
                if (encoderCaps->GetProperty(bframeCap, &value) == AMF_OK) {
                    caps.bFrames = value.ToBool();
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
            if (encoderCaps->GetProperty(paCap, &paValue) == AMF_OK) {
                caps.lookahead = paValue.ToBool();
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
        encoderStats.encoderAvailable = caps.available;
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

        // Init with the actual input surface format. The capture pipeline feeds
        // BGRA D3D11 textures; the AMF encoder performs BGRA->NV12 color
        // conversion internally (AMF_VIDEO_ENCODER_CAP_COLOR_CONVERSION), so no
        // separate video-processor pass is required.
        AMF_RESULT result = encoder->Init(inputSurfaceFormat,
                                          static_cast<amf_int32>(settings.width),
                                          static_cast<amf_int32>(settings.height));
        if (result != AMF_OK) {
            caps.detail = L"AMF AVC encoder Init failed: " + amfResultName(result);
            return false;
        }

        // Dynamic rate-control properties applied after Init so USAGE presets do
        // not overwrite them.
        setProp(encoder, AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
                amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR)));
        setProp(encoder, AMF_VIDEO_ENCODER_TARGET_BITRATE, amf::AMFVariant(bitrate));
        setProp(encoder, AMF_VIDEO_ENCODER_PEAK_BITRATE, amf::AMFVariant(bitrate));
        setProp(encoder, AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, amf::AMFVariant(bitrate));
        setProp(encoder, AMF_VIDEO_ENCODER_FILLER_DATA_ENABLE, amf::AMFVariant(false));
        setProp(encoder, AMF_VIDEO_ENCODER_ENFORCE_HRD, amf::AMFVariant(true));
        setProp(encoder, AMF_VIDEO_ENCODER_FRAMERATE,
                amf::AMFVariant(AMFConstructRate(settings.fps, 1)));
        setProp(encoder, AMF_VIDEO_ENCODER_IDR_PERIOD, amf::AMFVariant(static_cast<amf_int64>(idrPeriod)));
        // Insert SPS/PPS periodically (AMF range 0-1000). Forced IDRs also set InsertSPS/PPS.
        setProp(encoder, AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING,
                amf::AMFVariant(static_cast<amf_int64>(std::min<uint32_t>(idrPeriod, 1000))));
        setProp(encoder, AMF_VIDEO_ENCODER_ENABLE_VBAQ, amf::AMFVariant(effective.spatialAQ));

        // B-frames reorder output; keep them off when zero-latency/idle coalescing is desired.
        const bool useBFrames = effective.bFrames && caps.bFrames && !effective.zeroReorderDelay;
        setProp(encoder, AMF_VIDEO_ENCODER_B_PIC_PATTERN,
                amf::AMFVariant(static_cast<amf_int64>(useBFrames ? 2 : 0)));
        if (effective.lookahead && caps.lookahead) {
            setProp(encoder, AMF_VIDEO_ENCODER_PREENCODE_ENABLE,
                    amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_PREENCODE_ENABLED)));
        }
        const uint32_t refFrames = caps.multipleReferenceFrames
            ? std::clamp<uint32_t>(effective.referenceFrames, 1, 4)
            : 1;
        setProp(encoder, AMF_VIDEO_ENCODER_MAX_NUM_REFRAMES, amf::AMFVariant(static_cast<amf_int64>(refFrames)));

        caps.effective = effective;
        caps.effective.bFrames = useBFrames;
        caps.effective.zeroReorderDelay = !useBFrames;
        if (!caps.multipleReferenceFrames) {
            caps.effective.referenceFrames = 1;
        }
        if (!caps.lookahead) {
            caps.effective.lookahead = false;
            caps.effective.lookaheadDepth = 0;
        }
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

        AMF_RESULT result = encoder->Init(inputSurfaceFormat,
                                          static_cast<amf_int32>(settings.width),
                                          static_cast<amf_int32>(settings.height));
        if (result != AMF_OK) {
            caps.detail = L"AMF HEVC encoder Init failed: " + amfResultName(result);
            return false;
        }

        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD,
                amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR)));
        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, amf::AMFVariant(bitrate));
        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, amf::AMFVariant(bitrate));
        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, amf::AMFVariant(bitrate));
        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_FILLER_DATA_ENABLE, amf::AMFVariant(false));
        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_ENFORCE_HRD, amf::AMFVariant(true));
        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_FRAMERATE,
                amf::AMFVariant(AMFConstructRate(settings.fps, 1)));
        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, amf::AMFVariant(static_cast<amf_int64>(gopSize)));
        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_NUM_GOPS_PER_IDR, amf::AMFVariant(static_cast<amf_int64>(1)));
        // VPS/SPS/PPS on every IDR for clean mid-session mux starts.
        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE,
                amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE_IDR_ALIGNED)));
        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_ENABLE_VBAQ, amf::AMFVariant(effective.spatialAQ));
        if (effective.lookahead && caps.lookahead) {
            setProp(encoder, AMF_VIDEO_ENCODER_HEVC_PREENCODE_ENABLE, amf::AMFVariant(true));
        }
        const uint32_t refFrames = caps.multipleReferenceFrames
            ? std::clamp<uint32_t>(effective.referenceFrames, 1, 4)
            : 1;
        setProp(encoder, AMF_VIDEO_ENCODER_HEVC_MAX_NUM_REFRAMES,
                amf::AMFVariant(static_cast<amf_int64>(refFrames)));

        caps.effective = effective;
        caps.effective.bFrames = false; // AMF HEVC has no B-frame pattern control here.
        caps.effective.zeroReorderDelay = true;
        if (!caps.multipleReferenceFrames) {
            caps.effective.referenceFrames = 1;
        }
        if (!caps.lookahead) {
            caps.effective.lookahead = false;
            caps.effective.lookaheadDepth = 0;
        }
        return true;
    }

    bool initializeEncoder(ID3D11Device* device, const std::wstring& adapterName) {
        if (!loadFactory() || !openContext(device)) {
            return false;
        }

        const wchar_t* componentId = isHevc() ? AMFVideoEncoder_HEVC : AMFVideoEncoderVCE_AVC;
        AMF_RESULT result = factory->CreateComponent(context, componentId, &encoder);
        if (result != AMF_OK || !encoder) {
            caps.detail = std::wstring(L"AMFFactory::CreateComponent(") +
                          (isHevc() ? L"HEVC" : L"AVC") + L") failed: " + amfResultName(result);
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

        caps.available = true;
        caps.detail = std::wstring(L"AMF ") + (isHevc() ? L"HEVC" : L"AVC") +
                      L" Direct3D 11 session active";
        encoderStats.encoderAvailable = true;

        Logger::instance().info(L"encoder",
            std::wstring(L"AMF ") + (isHevc() ? L"HEVC" : L"AVC") +
            L" encoder initialized in zero-copy D3D11 mode at " +
            std::to_wstring(settings.width) + L"x" + std::to_wstring(settings.height));
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
        encoderStats.encoderAvailable = false;
        Logger::instance().error(L"encoder", detail);
        shutdownEncoder();
        return false;
    }

    bool popReadyPacket(EncodedPacket& packet) {
        if (readyPackets.empty()) {
            return false;
        }
        packet = std::move(readyPackets.front());
        readyPackets.pop_front();
        return true;
    }

    // Retrieves any available encoded output. Non-blocking unless waitForAll.
    bool harvestOutputs(bool waitForAll) {
        while (true) {
            amf::AMFDataPtr data;
            AMF_RESULT result = encoder->QueryOutput(&data);

            if (result == AMF_EOF) {
                break;
            }
            if (result == AMF_REPEAT || (result == AMF_OK && !data)) {
                if (waitForAll) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                break;
            }
            if (result != AMF_OK) {
                return fail(L"AMF QueryOutput failed: " + amfResultName(result));
            }

            amf::AMFBufferPtr buffer(data);
            if (!buffer) {
                continue;
            }

            const int64_t outputPts = static_cast<int64_t>(buffer->GetPts());
            const int64_t outputDuration = static_cast<int64_t>(buffer->GetDuration());

            // Match by PTS when B-frames reorder output; FIFO only as last resort.
            PendingInput pending;
            bool matched = false;
            if (outputPts != 0) {
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
            packet.bytes.assign(begin, begin + size);
            packet.pts100ns = outputPts != 0 ? outputPts : pending.fallbackPts100ns;
            packet.duration100ns = outputDuration != 0 ? outputDuration : pending.fallbackDuration100ns;
            packet.codec = settings.codec;
            packet.keyFrame = outputIsKeyFrame(buffer) || bitstreamContainsKeyFrame(settings.codec, packet.bytes);

            if (packet.bytes.empty()) {
                ++encoderStats.droppedFrames;
                continue;
            }

            const auto elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - pending.submittedAt).count();
            encoderStats.averageEncodeMs =
                encoderStats.encodedFrames == 0
                ? elapsed
                : (encoderStats.averageEncodeMs * 0.95 + elapsed * 0.05);
            ++encoderStats.encodedFrames;
            encoderStats.encodedBytes += packet.bytes.size();
            if (packet.keyFrame) {
                ++encoderStats.keyFrames;
            }
            readyPackets.push_back(std::move(packet));

            if (!waitForAll) {
                // Drain everything currently ready, then return.
                continue;
            }
        }
        return true;
    }

    bool encode(const GpuFrame& frame, EncodedPacket& packet) {
        if (encoderFaulted || !encoder || !frame.texture) {
            return popReadyPacket(packet);
        }
        if (frame.width != settings.width || frame.height != settings.height) {
            Logger::instance().warning(L"encoder", L"Frame size changed; dropping frame until capture pipeline is recreated");
            ++encoderStats.droppedFrames;
            return popReadyPacket(packet);
        }

        const auto start = std::chrono::steady_clock::now();

        amf::AMFSurfacePtr surface;
        AMF_RESULT result = context->CreateSurfaceFromDX11Native(frame.texture.Get(), &surface, nullptr);
        if (result != AMF_OK || !surface) {
            ++encoderStats.droppedFrames;
            if (++consecutiveSubmitFailures == 30) {
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
                    if (requestedKeyFrame) {
                        forceKeyFrame = true;
                    }
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
                if (requestedKeyFrame) {
                    forceKeyFrame = true;
                }
                ++encoderStats.droppedFrames;
                if (result == AMF_INPUT_FULL) {
                    return popReadyPacket(packet);
                }
                if (++consecutiveSubmitFailures == 30) {
                    return fail(L"AMF SubmitInput failed repeatedly: " + amfResultName(result));
                }
                return popReadyPacket(packet);
            }
        } else if (result != AMF_OK && result != AMF_NEED_MORE_INPUT) {
            if (requestedKeyFrame) {
                forceKeyFrame = true;
            }
            ++encoderStats.droppedFrames;
            if (++consecutiveSubmitFailures == 30) {
                return fail(L"AMF SubmitInput failed repeatedly: " + amfResultName(result));
            }
            return popReadyPacket(packet);
        }

        consecutiveSubmitFailures = 0;
        ++encoderStats.submittedFrames;

        PendingInput pending;
        pending.surface = surface;
        pending.lease = frame.lease;
        pending.texture = frame.texture;
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

        const AMF_RESULT result = encoder->Drain();
        if (result != AMF_OK && result != AMF_EOF) {
            Logger::instance().error(L"encoder", L"AMF Drain failed: " + amfResultName(result));
        } else {
            harvestOutputs(true);
        }
        while (!readyPackets.empty()) {
            packets.push_back(std::move(readyPackets.front()));
            readyPackets.pop_front();
        }
    }

    void shutdownEncoder() {
        inFlight.clear();
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
    impl_->caps.adapterName = device.adapterName();
    impl_->caps.backendName = L"AMF";
#if BACKTRACK_HAS_AMF
    impl_->encoderFaulted = false;
    impl_->consecutiveSubmitFailures = 0;
    const bool initialized = impl_->initializeEncoder(device.device(), device.adapterName());
    if (!initialized) {
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

void AmfEncoder::resetInputResources() {
    // AMF wraps a fresh AMFSurface around each input texture, so there is no
    // cached per-texture registration to release (unlike NVENC). The in-flight
    // surface queue is cleared on drain/shutdown.
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
#endif
    impl_->caps.available = false;
    impl_->encoderStats.encoderAvailable = false;
}

EncoderCapabilities AmfEncoder::capabilities() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->caps;
}

EncoderStats AmfEncoder::stats() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->encoderStats;
}

DXGI_FORMAT AmfEncoder::preferredInputFormat() const {
    // AMF wraps the captured BGRA D3D11 texture directly and performs color
    // conversion on the encoder input. This avoids a per-frame BGRA->NV12 pass
    // through the D3D11 video processor, which fails on some AMD driver/adapter
    // combinations (CreateVideoProcessorInputView), and matches the NVENC path
    // so native-resolution capture is genuinely zero-copy.
    return DXGI_FORMAT_B8G8R8A8_UNORM;
}

} // namespace backtrack
