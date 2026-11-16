// NVENC tier body, included by one translation unit per API tier. Each tier
// includes its own SDK header and defines BACKTRACK_NVENC_NAMESPACE first: the
// headers define the same type names, so two tiers can never share a
// translation unit. No SDK type may escape the tier namespace.

#include "encoder/IEncoder.h"

#include "capture/D3DDevice.h"
#include "core/LogContext.h"
#include "core/Logger.h"
#include "core/EncodingNames.h"
#include "encoder/DecodeOrderTimestamps.h"
#include "encoder/EncoderCommon.h"
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace backtrack {

struct NvencEncoderTestAccess;

namespace BACKTRACK_NVENC_NAMESPACE {

// Settings and enum members added after the 11.1 tier. The two compiled tiers
// are an API major version apart, so a major test is exact and there is no
// minor-version arithmetic to get wrong.
#if NVENCAPI_MAJOR_VERSION >= 12
#define BACKTRACK_NVENC_API12 1
#else
#define BACKTRACK_NVENC_API12 0
#endif

namespace {

std::wstring nvStatusName(NVENCSTATUS status) {
    switch (status) {
    case NV_ENC_SUCCESS:
        return L"NV_ENC_SUCCESS";
    case NV_ENC_ERR_NO_ENCODE_DEVICE:
        return L"NV_ENC_ERR_NO_ENCODE_DEVICE";
    case NV_ENC_ERR_UNSUPPORTED_DEVICE:
        return L"NV_ENC_ERR_UNSUPPORTED_DEVICE";
    case NV_ENC_ERR_INVALID_ENCODERDEVICE:
        return L"NV_ENC_ERR_INVALID_ENCODERDEVICE";
    case NV_ENC_ERR_INVALID_DEVICE:
        return L"NV_ENC_ERR_INVALID_DEVICE";
    case NV_ENC_ERR_DEVICE_NOT_EXIST:
        return L"NV_ENC_ERR_DEVICE_NOT_EXIST";
    case NV_ENC_ERR_INVALID_PTR:
        return L"NV_ENC_ERR_INVALID_PTR";
    case NV_ENC_ERR_INVALID_EVENT:
        return L"NV_ENC_ERR_INVALID_EVENT";
    case NV_ENC_ERR_INVALID_PARAM:
        return L"NV_ENC_ERR_INVALID_PARAM";
    case NV_ENC_ERR_INVALID_CALL:
        return L"NV_ENC_ERR_INVALID_CALL";
    case NV_ENC_ERR_OUT_OF_MEMORY:
        return L"NV_ENC_ERR_OUT_OF_MEMORY";
    case NV_ENC_ERR_ENCODER_NOT_INITIALIZED:
        return L"NV_ENC_ERR_ENCODER_NOT_INITIALIZED";
    case NV_ENC_ERR_UNSUPPORTED_PARAM:
        return L"NV_ENC_ERR_UNSUPPORTED_PARAM";
    case NV_ENC_ERR_LOCK_BUSY:
        return L"NV_ENC_ERR_LOCK_BUSY";
    case NV_ENC_ERR_NOT_ENOUGH_BUFFER:
        return L"NV_ENC_ERR_NOT_ENOUGH_BUFFER";
    case NV_ENC_ERR_INVALID_VERSION:
        return L"NV_ENC_ERR_INVALID_VERSION";
    case NV_ENC_ERR_MAP_FAILED:
        return L"NV_ENC_ERR_MAP_FAILED";
    case NV_ENC_ERR_NEED_MORE_INPUT:
        return L"NV_ENC_ERR_NEED_MORE_INPUT";
    case NV_ENC_ERR_ENCODER_BUSY:
        return L"NV_ENC_ERR_ENCODER_BUSY";
    case NV_ENC_ERR_EVENT_NOT_REGISTERD:
        return L"NV_ENC_ERR_EVENT_NOT_REGISTERD";
    case NV_ENC_ERR_GENERIC:
        return L"NV_ENC_ERR_GENERIC";
    case NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY:
        return L"NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY";
    case NV_ENC_ERR_UNIMPLEMENTED:
        return L"NV_ENC_ERR_UNIMPLEMENTED";
    case NV_ENC_ERR_RESOURCE_REGISTER_FAILED:
        return L"NV_ENC_ERR_RESOURCE_REGISTER_FAILED";
    case NV_ENC_ERR_RESOURCE_NOT_REGISTERED:
        return L"NV_ENC_ERR_RESOURCE_NOT_REGISTERED";
    case NV_ENC_ERR_RESOURCE_NOT_MAPPED:
        return L"NV_ENC_ERR_RESOURCE_NOT_MAPPED";
    default:
        return L"NVENC error " + std::to_wstring(static_cast<int>(status));
    }
}

GUID codecGuid(VideoCodec codec) {
    return codec == VideoCodec::Hevc ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
}

GUID presetGuid(EncoderPreset preset) {
    static const GUID presets[] = {
        NV_ENC_PRESET_P1_GUID, NV_ENC_PRESET_P2_GUID, NV_ENC_PRESET_P3_GUID, NV_ENC_PRESET_P4_GUID,
        NV_ENC_PRESET_P5_GUID, NV_ENC_PRESET_P6_GUID, NV_ENC_PRESET_P7_GUID};
    const auto index = static_cast<unsigned>(preset);
    return index < std::size(presets) ? presets[index] : NV_ENC_PRESET_P4_GUID;
}

NV_ENC_TUNING_INFO tuningInfo(EncoderMode mode) {
    // NV_ENC_TUNING_INFO_ULTRA_HIGH_QUALITY arrived with API 12.0. The 11.x
    // tier has no equivalent, so the requested mode falls through to high
    // quality instead of failing the session.
    static constexpr NV_ENC_TUNING_INFO modes[] = {
        NV_ENC_TUNING_INFO_HIGH_QUALITY, NV_ENC_TUNING_INFO_LOW_LATENCY,
        NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, NV_ENC_TUNING_INFO_LOSSLESS
#if BACKTRACK_NVENC_API12
        , NV_ENC_TUNING_INFO_ULTRA_HIGH_QUALITY
#endif
    };
    const auto index = static_cast<unsigned>(mode);
    return index < std::size(modes) ? modes[index] : NV_ENC_TUNING_INFO_HIGH_QUALITY;
}

NV_ENC_MULTI_PASS nvencMultipass(EncoderMultipass multipass) {
    return multipass == EncoderMultipass::QuarterResolution ? NV_ENC_TWO_PASS_QUARTER_RESOLUTION :
           multipass == EncoderMultipass::FullResolution ? NV_ENC_TWO_PASS_FULL_RESOLUTION :
           NV_ENC_MULTI_PASS_DISABLED;
}

NV_ENC_NUM_REF_FRAMES nvencReferenceFrames(uint32_t referenceFrames) {
    static constexpr NV_ENC_NUM_REF_FRAMES frames[] = {
        NV_ENC_NUM_REF_FRAMES_1, NV_ENC_NUM_REF_FRAMES_2, NV_ENC_NUM_REF_FRAMES_3, NV_ENC_NUM_REF_FRAMES_4};
    return frames[std::clamp<uint32_t>(referenceFrames, 1, 4) - 1];
}

// Captured desktop content is RGB; the encoder converts to YUV. Pin BT.709
// matrix/primaries/transfer with limited (studio) range in the SPS/VPS VUI so
// every clip decodes with identical colors regardless of driver defaults.
void applyBt709LimitedRangeVui(NV_ENC_CONFIG_H264_VUI_PARAMETERS& vui) {
    vui.videoSignalTypePresentFlag = 1;
    vui.videoFullRangeFlag = 0;
    vui.colourDescriptionPresentFlag = 1;
#if BACKTRACK_NVENC_API12
    vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
    vui.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    vui.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
    vui.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
#else
    // The named VUI enums arrived with API 12.0. The 11.x fields are uint32_t
    // and take the same ITU-T H.264 Annex E values.
    vui.videoFormat = 5;         // unspecified
    vui.colourPrimaries = 1;     // BT.709
    vui.transferCharacteristics = 1;
    vui.colourMatrix = 1;
#endif
}

} // namespace

// One tier's implementation. Everything NVENC-facing lives here, so the
// dispatcher in NvencEncoder.cpp stays free of SDK types.
class Encoder final : public IEncoder {
public:
    Encoder();
    ~Encoder() override;

    bool initialize(D3DDevice& device, const VideoSettings& settings) override;
    void requestKeyFrame() override;
    bool resetInputResources() override;
    bool encodeFrame(const GpuFrame& frame, EncodedPacket& packet) override;
    void encodeFrame(const GpuFrame& frame, std::vector<EncodedPacket>& packets) override;
    void drain(std::vector<EncodedPacket>& packets) override;
    void shutdown() override;
    EncoderCapabilities capabilities() const override;
    EncoderStats stats() const override;
    bool encoderAvailable() const noexcept override;
    DXGI_FORMAT preferredInputFormat() const override;
    uint32_t inputRetentionBudget() const override;

private:
    // The lifecycle suite includes one tier translation unit directly and
    // drives the implementation through this accessor.
    friend struct ::backtrack::NvencEncoderTestAccess;
    struct Impl;
    void abandonImpl();
    std::unique_ptr<Impl> impl_;
};

struct Encoder::Impl {
    using CreateInstanceFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);

    HMODULE module = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api{};
    void* session = nullptr;
    ComPtr<ID3D11Device> sessionDevice;
    bool sessionFlushed = false;
    uint64_t nextInputToken = 1;
    VideoSettings settings;
    EncoderCapabilities caps;
    EncoderStats encoderStats;
    mutable std::mutex mutex;
    // Last stats/capabilities copied while mutex was free. stats() and
    // capabilities() serve these when try_lock fails so a diagnostics poll
    // never queues behind a driver call (encode/drain hold mutex across
    // sleeps and blocking locks). Slightly stale under load; the lock-free
    // availableAtomic stays the health signal.
    mutable std::mutex snapshotMutex;
    EncoderStats statsSnapshot;
    EncoderCapabilities capsSnapshot;
    // Lock-free mirror of encoderStats.encoderAvailable for the capture loop's
    // per-iteration health probe (IEncoder::encoderAvailable). Written only on
    // paths that already hold mutex, so the pair never disagrees observably.
    std::atomic<bool> availableAtomic{false};

    void publishAvailability(bool available) {
        encoderStats.encoderAvailable = available;
        availableAtomic.store(available, std::memory_order_release);
    }

    struct RegisteredResource {
        // The COM reference protects registration across pool destruction. The
        // mapping and lease live on the pending submission, not here, so a
        // producer never sees a registration-level cached mapping.
        ComPtr<ID3D11Texture2D> texture;
        NV_ENC_REGISTERED_PTR registered = nullptr;
    };

    // Output buffers are consumed strictly in submission order: the driver
    // writes encoded pictures into them in encode order, so which input a
    // buffer carries is only known from its output timestamp.
    struct PendingOutput {
        NV_ENC_OUTPUT_PTR bitstream = nullptr;
        std::chrono::steady_clock::time_point submittedAt;
        uint64_t submittedFramesAtSubmission = 0;
        uint64_t outputToken = 0;
        const uint8_t* lockedData = nullptr;
        uint32_t lockedSize = 0;
        int64_t lockedDuration = 0;
        NV_ENC_PIC_TYPE lockedPictureType = NV_ENC_PIC_TYPE_UNKNOWN;
        bool completed = false;
        bool locked = false;
    };

    // Submitted inputs are a separate set, matched to an output by token in
    // any order. The mapping and lease live until that output is locked.
    struct PendingInput {
        NV_ENC_INPUT_PTR mapped = nullptr;
        ID3D11Texture2D* inputTexture = nullptr;
        std::shared_ptr<void> lease;
        uint64_t token = 0;
        int64_t pts100ns = 0;
        int64_t duration100ns = 0;
        std::chrono::steady_clock::time_point submittedAt;
        bool finalized = false;
    };

    struct RetainedMapping {
        NV_ENC_INPUT_PTR mapped = nullptr;
        ID3D11Texture2D* inputTexture = nullptr;
        std::shared_ptr<void> lease;
    };

    // Cached codec parameter sets (SPS/PPS or VPS/SPS/PPS) captured from the
    // first packet that carried them. Prepended to any keyframe packet that
    // lacks them so a replay cut at an IDR is always self-contained even if
    // the driver ignores repeatSPSPPS/header-insertion settings.
    std::vector<uint8_t> cachedParameterSets;

    std::unordered_map<ID3D11Texture2D*, RegisteredResource> registeredResources;
    std::vector<NV_ENC_OUTPUT_PTR> bitstreamBuffers;
    std::deque<NV_ENC_OUTPUT_PTR> availableBitstreams;
    std::deque<PendingOutput> pendingOutputs;
    std::deque<PendingInput> pendingInputs;
    std::deque<RetainedMapping> retainedMappings;
    std::deque<EncodedPacket> readyPackets;
    DecodeOrderTimestamps decodeTimestamps;
    std::atomic<bool> forceKeyFrame{true};
    size_t readyOutputCount = 0;
    uint32_t consecutiveRegisterFailures = 0;
    uint32_t consecutiveMapFailures = 0;
    uint32_t consecutiveEncodeFailures = 0;
    uint32_t consecutiveNeedMoreInput = 0;
    uint32_t consecutiveOutputBufferStarvation = 0;
    // Terminal fault: stop submitting into a session that cannot produce
    // output. `inputMappingFaulted` is the narrower subset where the driver's
    // input bindings are in an unknown state (a failed unmap/unregister/EOS),
    // so the caller must also keep every mapping and lease alive.
    bool sessionFaulted = false;
    bool inputMappingFaulted = false;

    // Non-blocking bitstream-lock support. Healthy drivers honour
    // NV_ENC_LOCK_BITSTREAM::doNotWait=1 (returning LOCK_BUSY until the encode
    // completes); some older drivers instead answer with an unsupported-
    // parameter error. Probed on first use; a session that falls back to the
    // blocking lock stays on it permanently so behaviour never changes
    // mid-session.
    enum class BitstreamPolling { Unknown, Supported, Unsupported };
    BitstreamPolling bitstreamPolling = BitstreamPolling::Unknown;

    // Hard ceiling for acquiring an output whose encode was already accepted.
    // A LOCK_BUSY output is only wedged once the encoder received enough
    // later frames to finish its reorder pipeline (see
    // reorderDelayFrames()); before that, waiting is healthy. Once both
    // conditions hold, escalate to an encoder fault (encoderAvailable=false)
    // so the capture loop recreates the pipeline instead of hanging or
    // recycling a stuck buffer while pipelineGpuMutex_ is held.
    static constexpr auto kBitstreamLockTimeout = kEncoderOutputTimeout;

    uint32_t reorderDelayFrames() const {
        return (caps.effective.bFrames ? 2u : 0u) +
               (caps.effective.lookahead ? caps.effective.lookaheadDepth : 0u);
    }

    // True when this output must be lockable on request. Two independent
    // bounds, either of which is sufficient: enough later frames entered the
    // encoder to flush its reorder pipeline, or the frame's real-time reorder
    // window plus a generous margin has passed since submission (capture feeds
    // frames continuously at settings.fps, so age implies successors unless
    // the pipeline itself stalled - and then the LOCK_BUSY escalation below
    // faults it instead of waiting forever). The successor bound alone would
    // deadlock once every output buffer sits in pendingOutputs: submissions
    // stop because no buffer can be acquired, so the count can never advance.
    bool outputShouldBeComplete(const PendingOutput& pending) const {
        if (encoderStats.submittedFrames >=
            pending.submittedFramesAtSubmission + reorderDelayFrames() + 1) {
            return true;
        }
        const auto reorderWindowMs = std::chrono::milliseconds(
            (1000 * (reorderDelayFrames() + 8)) / std::max<uint32_t>(settings.fps, 1));
        return SteadyClock::now() - pending.submittedAt >= reorderWindowMs;
    }

    // Matches the AMF escalation threshold: a driver TDR/reset leaves the
    // NVENC session permanently broken, so every submit fails while stats keep
    // reporting healthy and the recording degrades into a freeze-frame. After
    // this many consecutive failures the session is faulted (encoderAvailable
    // = false) so the capture loop's recreateGpuPipeline path rebuilds it.
    static constexpr uint32_t kMaxConsecutiveEncodeFailures = kEncoderMaxConsecutiveFailures;

    bool loadApi() {
        module = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) {
            caps.detail = L"nvEncodeAPI64.dll was not found in System32. Install a current NVIDIA driver.";
            return false;
        }

        auto createInstance = reinterpret_cast<CreateInstanceFn>(GetProcAddress(module, "NvEncodeAPICreateInstance"));
        if (!createInstance) {
            caps.detail = L"NvEncodeAPICreateInstance was not exported by nvEncodeAPI64.dll.";
            return false;
        }

        api = {};
        api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        NVENCSTATUS status = createInstance(&api);
        if (status != NV_ENC_SUCCESS) {
            caps.detail = L"NvEncodeAPICreateInstance failed: " + nvStatusName(status);
            return false;
        }
        return true;
    }

    bool openSession(ID3D11Device* device) {
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS openParams{};
        openParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        openParams.device = device;
        openParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        openParams.apiVersion = NVENCAPI_VERSION;

        NVENCSTATUS status = api.nvEncOpenEncodeSessionEx(&openParams, &session);
        if (status != NV_ENC_SUCCESS) {
            caps.detail = L"nvEncOpenEncodeSessionEx failed: " + nvStatusName(status);
            return false;
        }
        // Keep the device alive with a session that may have to be abandoned.
        sessionDevice = device;
        return true;
    }

    int queryCap(GUID codec, NV_ENC_CAPS capsToQuery) {
        NV_ENC_CAPS_PARAM param{};
        param.version = NV_ENC_CAPS_PARAM_VER;
        param.capsToQuery = capsToQuery;
        int value = 0;
        if (session && api.nvEncGetEncodeCaps(session, codec, &param, &value) == NV_ENC_SUCCESS) {
            return value;
        }
        return 0;
    }

    void updateCapabilities(const std::wstring& adapterName) {
        caps.available = session != nullptr;
        caps.adapterName = adapterName;
        caps.backendName = L"NVENC";
        caps.h264 = queryCap(NV_ENC_CODEC_H264_GUID, NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES) != 0;
        caps.hevc = queryCap(NV_ENC_CODEC_HEVC_GUID, NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES) != 0;
        caps.bFrames = queryCap(codecGuid(settings.codec), NV_ENC_CAPS_NUM_MAX_BFRAMES) > 0;
        caps.lookahead = queryCap(codecGuid(settings.codec), NV_ENC_CAPS_SUPPORT_LOOKAHEAD) != 0;
        caps.multipleReferenceFrames = queryCap(codecGuid(settings.codec), NV_ENC_CAPS_SUPPORT_MULTIPLE_REF_FRAMES) != 0;
        caps.tenBitHevc = queryCap(NV_ENC_CODEC_HEVC_GUID, NV_ENC_CAPS_SUPPORT_10BIT_ENCODE) != 0;
        caps.maxWidth = static_cast<uint32_t>(queryCap(codecGuid(settings.codec), NV_ENC_CAPS_WIDTH_MAX));
        caps.maxHeight = static_cast<uint32_t>(queryCap(codecGuid(settings.codec), NV_ENC_CAPS_HEIGHT_MAX));
        caps.detail = caps.available ? L"NVENC Direct3D 11 session active" : caps.detail;
        publishAvailability(caps.available);
    }

    bool initializeEncoder(ID3D11Device* device, const std::wstring& adapterName) {
        consecutiveMapFailures = 0;
        consecutiveRegisterFailures = 0;
        consecutiveEncodeFailures = 0;
        sessionFaulted = false;
        inputMappingFaulted = false;
        cachedParameterSets.clear();
        if (!loadApi() || !openSession(device)) {
            return false;
        }

        updateCapabilities(adapterName);
        if ((settings.codec == VideoCodec::H264 && !caps.h264) || (settings.codec == VideoCodec::Hevc && !caps.hevc)) {
            caps.detail = L"Requested codec is not supported by this NVENC device.";
            return false;
        }

        caps.effective = effectiveSettingsFor(settings);
        if (!caps.bFrames) {
            caps.effective.bFrames = false;
            caps.effective.adaptiveBFrames = false;
        }
        if (!caps.lookahead) {
            caps.effective.lookahead = false;
            caps.effective.lookaheadDepth = 0;
            caps.effective.adaptiveBFrames = false;
            caps.effective.adaptiveIFrames = false;
        }
        if (queryCap(codecGuid(settings.codec), NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ) == 0) {
            caps.effective.temporalAQ = false;
        }
        if (!caps.multipleReferenceFrames) {
            caps.effective.referenceFrames = 1;
        }
        const uint32_t effectiveBFrameCount = caps.effective.bFrames ? 2 : 0;
        caps.effective.lookaheadDepth = std::min<uint32_t>(caps.effective.lookaheadDepth, 31 - effectiveBFrameCount);
        // Warm up with the full pipeline depth the heuristics below assume:
        // frameIntervalP holds bFrames + 1 frames before the first output
        // appears, and enabled lookahead keeps roughly its configured depth
        // of submissions unharvested in steady state. Resetting with less
        // would make every output look surplus once the pipeline filled,
        // tripping the desync watchdog (see DecodeOrderTimestamps).
        decodeTimestamps.reset(reorderDelayFrames());

        const GUID requestedPreset = presetGuid(caps.effective.preset);
        const NV_ENC_TUNING_INFO requestedTuning = tuningInfo(caps.effective.mode);

        NV_ENC_PRESET_CONFIG presetConfig{};
        presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
        presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;
        NVENCSTATUS status = api.nvEncGetEncodePresetConfigEx(
            session,
            codecGuid(settings.codec),
            requestedPreset,
            requestedTuning,
            &presetConfig);
        if (status != NV_ENC_SUCCESS) {
            caps.detail = L"nvEncGetEncodePresetConfigEx failed: " + nvStatusName(status);
            return false;
        }

        NV_ENC_CONFIG encodeConfig = presetConfig.presetCfg;
        encodeConfig.version = NV_ENC_CONFIG_VER;
        encodeConfig.gopLength = std::max<uint32_t>(1, settings.fps * settings.gopSeconds);
        encodeConfig.frameIntervalP = caps.effective.bFrames ? 3 : 1;
        encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        encodeConfig.rcParams.averageBitRate = settings.bitrateKbps * 1000;
        encodeConfig.rcParams.maxBitRate = settings.bitrateKbps * 1000;
        encodeConfig.rcParams.vbvBufferSize = settings.bitrateKbps * 1000;
        encodeConfig.rcParams.vbvInitialDelay = encodeConfig.rcParams.vbvBufferSize;
        encodeConfig.rcParams.enableAQ = caps.effective.spatialAQ ? 1 : 0;
        encodeConfig.rcParams.aqStrength = caps.effective.spatialAQ ? caps.effective.aqStrength : 0;
        encodeConfig.rcParams.enableLookahead = caps.effective.lookahead ? 1 : 0;
        encodeConfig.rcParams.lookaheadDepth = caps.effective.lookahead
            ? static_cast<uint16_t>(caps.effective.lookaheadDepth)
            : 0;
        encodeConfig.rcParams.enableTemporalAQ = caps.effective.temporalAQ ? 1 : 0;
        encodeConfig.rcParams.multiPass = nvencMultipass(caps.effective.multipass);
        encodeConfig.rcParams.disableIadapt = caps.effective.adaptiveIFrames ? 0 : 1;
        encodeConfig.rcParams.disableBadapt = caps.effective.adaptiveBFrames ? 0 : 1;
        encodeConfig.rcParams.zeroReorderDelay = caps.effective.zeroReorderDelay ? 1 : 0;
#if BACKTRACK_NVENC_API12
        encodeConfig.rcParams.enableExtLookahead = 0;
#endif
        encodeConfig.rcParams.enableNonRefP = 0;
        encodeConfig.rcParams.strictGOPTarget = 0;
        encodeConfig.rcParams.qpMapMode = NV_ENC_QP_MAP_DISABLED;

        if (settings.codec == VideoCodec::H264) {
            auto& h264 = encodeConfig.encodeCodecConfig.h264Config;
            h264.idrPeriod = encodeConfig.gopLength;
            h264.repeatSPSPPS = 1;
#if BACKTRACK_NVENC_API12
            h264.enableTemporalSVC = 0;
#endif
            h264.hierarchicalPFrames = 0;
            h264.hierarchicalBFrames = 0;
            h264.enableIntraRefresh = 0;
            h264.enableLTR = 0;
            h264.maxNumRefFrames = caps.effective.referenceFrames;
            h264.numRefL0 = nvencReferenceFrames(caps.effective.referenceFrames);
            h264.numRefL1 = caps.effective.bFrames ? nvencReferenceFrames(1) : NV_ENC_NUM_REF_FRAMES_AUTOSELECT;
            h264.useBFramesAsRef = NV_ENC_BFRAME_REF_MODE_DISABLED;
#if BACKTRACK_NVENC_API12
            h264.tfLevel = NV_ENC_TEMPORAL_FILTER_LEVEL_0;
#endif
            applyBt709LimitedRangeVui(h264.h264VUIParameters);
        } else {
            auto& hevc = encodeConfig.encodeCodecConfig.hevcConfig;
            hevc.idrPeriod = encodeConfig.gopLength;
            hevc.repeatSPSPPS = 1;
#if BACKTRACK_NVENC_API12
            hevc.enableTemporalSVC = 0;
#endif
            hevc.enableIntraRefresh = 0;
            hevc.enableLTR = 0;
            hevc.maxNumRefFramesInDPB = caps.effective.referenceFrames;
            hevc.numRefL0 = nvencReferenceFrames(caps.effective.referenceFrames);
            hevc.numRefL1 = caps.effective.bFrames ? nvencReferenceFrames(1) : NV_ENC_NUM_REF_FRAMES_AUTOSELECT;
            hevc.useBFramesAsRef = NV_ENC_BFRAME_REF_MODE_DISABLED;
#if BACKTRACK_NVENC_API12
            hevc.tfLevel = NV_ENC_TEMPORAL_FILTER_LEVEL_0;
#endif
            applyBt709LimitedRangeVui(hevc.hevcVUIParameters);
        }

        NV_ENC_INITIALIZE_PARAMS initParams{};
        initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
        initParams.encodeGUID = codecGuid(settings.codec);
        initParams.presetGUID = requestedPreset;
        initParams.tuningInfo = requestedTuning;
        initParams.encodeWidth = settings.width;
        initParams.encodeHeight = settings.height;
        initParams.darWidth = settings.width;
        initParams.darHeight = settings.height;
        initParams.frameRateNum = settings.fps;
        initParams.frameRateDen = 1;
        initParams.enableEncodeAsync = 0;
        initParams.enablePTD = 1;
        initParams.encodeConfig = &encodeConfig;

        status = api.nvEncInitializeEncoder(session, &initParams);
        if (status != NV_ENC_SUCCESS) {
            caps.detail = L"nvEncInitializeEncoder failed: " + nvStatusName(status);
            return false;
        }
        sessionFlushed = false;
        nextInputToken = 1;

        const uint32_t bitstreamBufferCount = desiredBitstreamBufferCount();
        for (uint32_t index = 0; index < bitstreamBufferCount; ++index) {
            NV_ENC_CREATE_BITSTREAM_BUFFER createBuffer{};
            createBuffer.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
            status = api.nvEncCreateBitstreamBuffer(session, &createBuffer);
            if (status != NV_ENC_SUCCESS) {
                caps.detail = L"nvEncCreateBitstreamBuffer failed: " + nvStatusName(status);
                destroyBitstreamBuffers();
                return false;
            }
            bitstreamBuffers.push_back(createBuffer.bitstreamBuffer);
            availableBitstreams.push_back(createBuffer.bitstreamBuffer);
        }

        Logger::instance().info(L"encoder",
            L"NVENC initialized in D3D11 mode at " +
            std::to_wstring(settings.width) + L"x" + std::to_wstring(settings.height) +
            L", codec=" + codecName(settings.codec) +
            L", bitrateKbps=" + std::to_wstring(settings.bitrateKbps) +
            L", gopSeconds=" + std::to_wstring(settings.gopSeconds) +
            L", preset=" + encoderPresetName(caps.effective.preset) +
            L", mode=" + encoderModeName(caps.effective.mode) +
            L", bFrames=" + (caps.effective.bFrames ? L"yes" : L"no") +
            L", lookahead=" + (caps.effective.lookahead ? L"yes" : L"no") +
            (caps.effective.lookahead
                ? L" depth=" + std::to_wstring(caps.effective.lookaheadDepth) : L"") +
            L" with " + std::to_wstring(bitstreamBufferCount) + L" output buffers");
        return true;
    }

    uint32_t desiredBitstreamBufferCount() const {
        return std::clamp<uint32_t>(reorderDelayFrames() + 4, 4, 40);
    }

    RegisteredResource* registeredResourceFor(ID3D11Texture2D* texture, NV_ENC_BUFFER_FORMAT bufferFormat) {
        const auto existing = registeredResources.find(texture);
        if (existing != registeredResources.end()) {
            return &existing->second;
        }

        // Allocate the entry before giving its texture to the driver.
        const auto [it, inserted] = registeredResources.try_emplace(texture);
        (void)inserted;
        it->second.texture = texture;
        NV_ENC_REGISTER_RESOURCE resource{};
        resource.version = NV_ENC_REGISTER_RESOURCE_VER;
        resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        resource.resourceToRegister = texture;
        resource.width = settings.width;
        resource.height = settings.height;
        resource.bufferFormat = bufferFormat;
        resource.bufferUsage = NV_ENC_INPUT_IMAGE;

        const NVENCSTATUS status = api.nvEncRegisterResource(session, &resource);
        if (status != NV_ENC_SUCCESS) {
            registeredResources.erase(it);
            ++consecutiveRegisterFailures;
            if (consecutiveRegisterFailures == 1) {
                Logger::instance().error(L"encoder", L"nvEncRegisterResource failed: " + nvStatusName(status));
            } else if ((consecutiveRegisterFailures % 300) == 0) {
                Logger::instance().warning(L"encoder",
                    L"Still suppressing repeated nvEncRegisterResource failures; count=" +
                    std::to_wstring(consecutiveRegisterFailures));
            }
            return nullptr;
        }
        it->second.registered = resource.registeredResource;
        consecutiveRegisterFailures = 0;
        Logger::instance().debug(L"encoder",
            L"NVENC registered input resources: " + std::to_wstring(registeredResources.size()));
        return &it->second;
    }

    bool unregisterResource(ID3D11Texture2D* texture) {
        const auto it = registeredResources.find(texture);
        if (it == registeredResources.end()) {
            return true;
        }
        if (!it->second.registered) {
            registeredResources.erase(it);
            return true;
        }
        const NVENCSTATUS status = api.nvEncUnregisterResource(session, it->second.registered);
        if (status != NV_ENC_SUCCESS) {
            return failInputMapping(L"nvEncUnregisterResource failed: " + nvStatusName(status));
        }
        registeredResources.erase(it);
        return true;
    }

    bool sendEos() {
        NV_ENC_PIC_PARAMS pic{};
        pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        const NVENCSTATUS status = api.nvEncEncodePicture(session, &pic);
        if (status != NV_ENC_SUCCESS && status != NV_ENC_ERR_NEED_MORE_INPUT) {
            return failInputMapping(L"nvEncEncodePicture(EOS) failed: " + nvStatusName(status));
        }
        sessionFlushed = true;
        readyOutputCount = pendingOutputs.size();
        return true;
    }

    bool flushAndUnregisterResources() {
        if (!session) {
            return registeredResources.empty() && pendingOutputs.empty() &&
                   pendingInputs.empty() && retainedMappings.empty();
        }
        while (!retainedMappings.empty()) {
            const auto& retained = retainedMappings.front();
            const NVENCSTATUS status = api.nvEncUnmapInputResource(session, retained.mapped);
            if (status != NV_ENC_SUCCESS) {
                return failInputMapping(L"nvEncUnmapInputResource failed: " + nvStatusName(status));
            }
            retainedMappings.pop_front();
        }
        if (!pendingOutputs.empty()) {
            if (!sessionFlushed && !sendEos()) {
                // Only outputs already credited by successful submissions may be locked.
                harvestPendingOutputs(false);
                return false;
            }
            if (!harvestPendingOutputs(true)) {
                return false;
            }
        }
        if (!pendingOutputs.empty() || !pendingInputs.empty() || !retainedMappings.empty()) {
            return false;
        }
        while (!registeredResources.empty()) {
            if (!unregisterResource(registeredResources.begin()->first)) {
                return false;
            }
        }
        consecutiveRegisterFailures = 0;
        consecutiveMapFailures = 0;
        return true;
    }

    void noteMapFailure(NVENCSTATUS status) {
        ++consecutiveMapFailures;
        if (consecutiveMapFailures == 1) {
            Logger::instance().error(L"encoder", L"nvEncMapInputResource failed: " + nvStatusName(status));
        } else if (consecutiveMapFailures == 30) {
            inputMappingFaulted = true;
            caps.available = false;
            caps.detail = L"NVENC input mapping failed repeatedly; capture pipeline disabled encoder until restart";
            publishAvailability(false);
            Logger::instance().error(L"encoder", caps.detail);
        } else if ((consecutiveMapFailures % 300) == 0) {
            Logger::instance().warning(L"encoder",
                L"Still suppressing repeated nvEncMapInputResource failures; count=" +
                std::to_wstring(consecutiveMapFailures));
        }
    }

    NV_ENC_OUTPUT_PTR acquireBitstreamBuffer() {
        if (availableBitstreams.empty()) {
            ++consecutiveOutputBufferStarvation;
            if (consecutiveOutputBufferStarvation == 1) {
                Logger::instance().warning(L"encoder", L"NVENC output buffer pool is full; dropping frame until delayed output drains");
            } else if ((consecutiveOutputBufferStarvation % 120) == 0) {
                Logger::instance().warning(L"encoder",
                    L"Still waiting for NVENC output buffers; count=" +
                    std::to_wstring(consecutiveOutputBufferStarvation));
            }
            return nullptr;
        }

        const uint32_t starvation = consecutiveOutputBufferStarvation;
        consecutiveOutputBufferStarvation = 0;
        if (starvation > 0) {
            LogContext::breadcrumb(L"NVENC output buffers drained after starvation");
        }
        NV_ENC_OUTPUT_PTR bitstream = availableBitstreams.front();
        availableBitstreams.pop_front();
        return bitstream;
    }

    void releaseBitstreamBuffer(NV_ENC_OUTPUT_PTR bitstream) {
        if (bitstream) {
            availableBitstreams.push_back(bitstream);
        }
    }

    // Terminal encoder fault: the session cannot produce output, so stop
    // submitting and publish unavailable for the capture loop's recreate path.
    // Driver state is still known well enough to retire resources.
    bool failEncoder(const std::wstring& detail) {
        sessionFaulted = true;
        caps.available = false;
        caps.detail = detail;
        publishAvailability(false);
        Logger::instance().error(L"encoder", detail);
        return false;
    }

    // A failed unmap/unregister/EOS leaves the driver's input bindings in an
    // unknown state: additionally latch inputMappingFaulted so no further input
    // is submitted and the caller keeps every mapping and lease alive.
    bool failInputMapping(const std::wstring& detail) {
        inputMappingFaulted = true;
        return failEncoder(detail);
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

    // A non-blocking lock that cannot return the picture yet means "not ready",
    // not a fault. This driver answers that case with NV_ENC_ERR_OUT_OF_MEMORY
    // instead of NV_ENC_ERR_LOCK_BUSY, so a status-only check turns every early
    // poll into a terminal fault. Only the per-output deadline escalates it.
    static bool lockNotReady(NVENCSTATUS status, bool nonBlocking) {
        return status == NV_ENC_ERR_LOCK_BUSY ||
               (nonBlocking && status == NV_ENC_ERR_OUT_OF_MEMORY);
    }

    bool harvestPendingOutputs(bool waitForCompletion) {
        // Drain mode uses an idle bound refreshed on every successfully locked
        // output instead of one absolute budget: a large queue of completed
        // outputs drains fully, while a wedged driver still escalates after
        // kBitstreamLockTimeout without progress.
        SteadyClock::time_point idleDeadline = waitForCompletion
            ? SteadyClock::now() + kBitstreamLockTimeout
            : SteadyClock::time_point::max();

        while (!pendingOutputs.empty() &&
               (waitForCompletion || readyOutputCount > 0 || pendingOutputs.front().completed)) {
            PendingOutput& pending = pendingOutputs.front();
            if (!pending.completed) {
                const bool oldestShouldBeComplete = outputShouldBeComplete(pending);
                NV_ENC_LOCK_BITSTREAM lock{};
                lock.version = NV_ENC_LOCK_BITSTREAM_VER;
                lock.outputBitstream = pending.bitstream;
                NVENCSTATUS status = NV_ENC_SUCCESS;
                bool lastLockNonBlocking = false;
                for (;;) {
                    const bool polling = bitstreamPolling != BitstreamPolling::Unsupported;
                    // Legacy blocking locks have no timeout, including during drain.
                    if (!polling && !waitForCompletion &&
                        (!oldestShouldBeComplete ||
                         SteadyClock::now() - pending.submittedAt >= kBitstreamLockTimeout)) {
                        status = NV_ENC_ERR_LOCK_BUSY;
                        break;
                    }
                    lock.doNotWait = polling ? 1 : 0;
                    lastLockNonBlocking = polling;
                    status = api.nvEncLockBitstream(session, &lock);
                    if (status == NV_ENC_SUCCESS) {
                        bitstreamPolling = polling ? BitstreamPolling::Supported
                                                   : BitstreamPolling::Unsupported;
                        break;
                    }
                    if (lockNotReady(status, polling)) {
                        if (!waitForCompletion || SteadyClock::now() >= idleDeadline) {
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
                    if (polling && bitstreamPolling == BitstreamPolling::Unknown &&
                        (status == NV_ENC_ERR_UNSUPPORTED_PARAM || status == NV_ENC_ERR_INVALID_PARAM ||
                         status == NV_ENC_ERR_UNIMPLEMENTED)) {
                        bitstreamPolling = BitstreamPolling::Unsupported;
                        continue;
                    }
                    break;
                }
                if (lockNotReady(status, lastLockNonBlocking) && !waitForCompletion) {
                    // Retry later, but only until the deadline from submission:
                    // a genuinely stuck driver still escalates to a recreate.
                    if (SteadyClock::now() - pending.submittedAt >= kBitstreamLockTimeout) {
                        return failEncoder(L"NVENC output did not complete before the lock deadline (" +
                            nvStatusName(status) + L")");
                    }
                    break;
                }
                if (lockNotReady(status, lastLockNonBlocking)) {
                    return failEncoder(L"NVENC drain did not produce a lockable output before the deadline (" +
                        nvStatusName(status) + L")");
                }
                if (status != NV_ENC_SUCCESS) {
                    ++encoderStats.droppedFrames;
                    return failEncoder(L"NVENC output lock failed: " + nvStatusName(status));
                }
                pending.completed = true;
                pending.locked = true;
                pending.outputToken = static_cast<uint64_t>(lock.outputTimeStamp);
                pending.lockedData = static_cast<const uint8_t*>(lock.bitstreamBufferPtr);
                pending.lockedSize = lock.bitstreamSizeInBytes;
                pending.lockedDuration = static_cast<int64_t>(lock.outputDuration);
                pending.lockedPictureType = lock.pictureType;
                if (readyOutputCount > 0) {
                    --readyOutputCount;
                }
                idleDeadline = SteadyClock::now() + kBitstreamLockTimeout;
            }
            // The locked output carries the picture the driver encoded into
            // this buffer, which is not necessarily the input submitted with
            // it. Identify that submission by its token and finalize it there.
            if (pending.outputToken == 0) {
                return failEncoder(L"NVENC output did not carry an input timestamp");
            }
            const auto matched = std::find_if(pendingInputs.begin(), pendingInputs.end(),
                [token = pending.outputToken](const PendingInput& entry) {
                    return entry.token == token;
                });
            if (matched == pendingInputs.end()) {
                return failEncoder(L"NVENC output did not identify a submitted input");
            }
            if (pending.locked) {
                if (!matched->finalized) {
                    EncodedPacket packet;
                    const bool finalized = finalizePacket(
                        settings.codec, pending.lockedData, pending.lockedSize,
                        matched->pts100ns, true, pending.lockedDuration, matched->pts100ns,
                        matched->duration100ns, pending.lockedPictureType == NV_ENC_PIC_TYPE_IDR,
                        decodeTimestamps, cachedParameterSets, encoderStats,
                        matched->submittedAt, packet);
                    matched->finalized = true;
                    if (finalized) {
                        readyPackets.push_back(std::move(packet));
                    }
                }
                const NVENCSTATUS status = api.nvEncUnlockBitstream(session, pending.bitstream);
                if (status != NV_ENC_SUCCESS) {
                    return failEncoder(L"nvEncUnlockBitstream failed: " + nvStatusName(status));
                }
                pending.locked = false;
            }
            // Retire the matched input before recycling its buffer slot: a
            // failed unmap must keep the mapping and lease alive, so the entry
            // is only dropped once the driver released it.
            if (matched->mapped) {
                const NVENCSTATUS status = api.nvEncUnmapInputResource(session, matched->mapped);
                if (status != NV_ENC_SUCCESS) {
                    return failEncoder(L"nvEncUnmapInputResource failed: " + nvStatusName(status));
                }
            }
            pendingInputs.erase(matched);
            // The slot itself is always consumed in submission order, whatever
            // input it ended up carrying.
            const NV_ENC_OUTPUT_PTR bitstream = pending.bitstream;
            pendingOutputs.pop_front();
            releaseBitstreamBuffer(bitstream);
        }
        if (waitForCompletion) {
            readyOutputCount = 0;
        }
        return true;
    }

    bool encode(const GpuFrame& frame, EncodedPacket& packet) {
        if (sessionFaulted || inputMappingFaulted || !session || bitstreamBuffers.empty() || !frame.texture) {
            return popReadyPacket(packet);
        }
        if (frame.width != settings.width || frame.height != settings.height) {
            Logger::instance().warning(L"encoder", L"Frame size changed; dropping frame until capture pipeline is recreated");
            ++encoderStats.droppedFrames;
            return popReadyPacket(packet);
        }

        NV_ENC_BUFFER_FORMAT expectedBufferFormat = NV_ENC_BUFFER_FORMAT_UNDEFINED;
        switch (frame.format) {
        case DXGI_FORMAT_NV12:
            expectedBufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
            break;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            expectedBufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
            break;
        default:
            Logger::instance().warning(L"encoder", L"Unsupported D3D11 encoder input format; dropping frame");
            ++encoderStats.droppedFrames;
            return popReadyPacket(packet);
        }

        const auto start = std::chrono::steady_clock::now();
        ID3D11Texture2D* texture = frame.texture.Get();

        // frame loss must restart the reference chain without stranding harvested output.
        auto dropFrame = [&](const wchar_t* reason) {
            forceKeyFrame = true;
            ++encoderStats.droppedFrames;
            if (reason) {
                Logger::instance().warning(L"encoder", reason);
            }
            return popReadyPacket(packet);
        };

        // A texture stays mapped until the output carrying its timestamp is
        // harvested. Submitting the same zero-copy lastFrame again while it is
        // still mapped would touch a texture the encoder owns; after unmap the
        // producer may reuse it, so only live mappings block. A same-size
        // producer reuses one texture for idle heartbeats, so try one bounded
        // harvest first: dropping without it never reaches the lock that would
        // unmap the texture and the repeated submissions could stall forever.
        const auto textureStillMapped = [this, texture] {
            return std::any_of(pendingInputs.begin(), pendingInputs.end(),
                [texture](const PendingInput& input) {
                    return input.mapped != nullptr && input.inputTexture == texture;
                });
        };
        if (textureStillMapped()) {
            if (!harvestPendingOutputs(false)) {
                return false;
            }
            if (textureStillMapped()) {
                return dropFrame(L"NVENC frame dropped: input texture still mapped; keyframe re-armed");
            }
        }
        for (const auto& retained : retainedMappings) {
            if (retained.inputTexture == texture) {
                return dropFrame(L"NVENC frame dropped: retained input mapping; keyframe re-armed");
            }
        }

        RegisteredResource* entry = registeredResourceFor(texture, expectedBufferFormat);
        if (!entry) {
            return dropFrame(L"NVENC frame dropped: no registered input resource; keyframe re-armed");
        }

        NV_ENC_OUTPUT_PTR bitstream = acquireBitstreamBuffer();
        if (!bitstream && !pendingOutputs.empty()) {
            // Every output buffer is an accepted-but-unlocked picture. This
            // also means submission has stopped, so per-submission harvesting
            // would never run again and neither the locks nor the
            // wedged-driver escalation could ever make progress. Credit the
            // pending outputs (the completion counter can legitimately be
            // lower when every accept returned NEED_MORE_INPUT), then harvest
            // once here - bounded, see harvestPendingOutputs - to free a
            // buffer or fault a wedged session, and try again.
            readyOutputCount = std::max<size_t>(readyOutputCount, pendingOutputs.size());
            if (!harvestPendingOutputs(false)) {
                return false;
            }
            bitstream = acquireBitstreamBuffer();
        }
        if (!bitstream) {
            return dropFrame(nullptr);
        }

        NV_ENC_MAP_INPUT_RESOURCE map{};
        map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        map.registeredResource = entry->registered;
        NVENCSTATUS status = api.nvEncMapInputResource(session, &map);
        if (status != NV_ENC_SUCCESS) {
            // A stale registration heals through one fresh register+map pair.
            if (!unregisterResource(texture)) {
                releaseBitstreamBuffer(bitstream);
                return false;
            }
            entry = registeredResourceFor(texture, expectedBufferFormat);
            if (entry) {
                map = {};
                map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
                map.registeredResource = entry->registered;
                status = api.nvEncMapInputResource(session, &map);
            }
            if (!entry || status != NV_ENC_SUCCESS) {
                forceKeyFrame = true;
                noteMapFailure(status);
                ++encoderStats.droppedFrames;
                releaseBitstreamBuffer(bitstream);
                return popReadyPacket(packet);
            }
        }
        consecutiveMapFailures = 0;

        NV_ENC_PIC_PARAMS pic{};
        pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.inputBuffer = map.mappedResource;
        pic.bufferFmt = map.mappedBufferFmt == NV_ENC_BUFFER_FORMAT_UNDEFINED
            ? expectedBufferFormat
            : map.mappedBufferFmt;
        pic.inputWidth = settings.width;
        pic.inputHeight = settings.height;
        pic.outputBitstream = bitstream;
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        pic.inputDuration = frame.duration100ns > 0
            ? static_cast<uint64_t>(frame.duration100ns)
            : static_cast<uint64_t>(
                kHundredNanosecondsPerSecond / std::max<uint32_t>(settings.fps, 1));
        pic.frameIdx = static_cast<uint32_t>(encoderStats.submittedFrames);
        const uint64_t token = nextInputToken++;
        pic.inputTimeStamp = token;
        const bool requestedKeyFrame = forceKeyFrame.exchange(false);
        if (requestedKeyFrame) {
            pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        }

        ++encoderStats.submittedFrames;
        status = api.nvEncEncodePicture(session, &pic);

        PendingOutput pending;
        pending.bitstream = bitstream;
        pending.submittedAt = start;
        pending.submittedFramesAtSubmission = encoderStats.submittedFrames;

        PendingInput pendingInput;
        pendingInput.mapped = map.mappedResource;
        pendingInput.inputTexture = texture;
        pendingInput.lease = frame.lease;
        pendingInput.token = token;
        pendingInput.pts100ns = frame.pts100ns;
        pendingInput.duration100ns = static_cast<int64_t>(pic.inputDuration);
        pendingInput.submittedAt = start;

        if (status != NV_ENC_SUCCESS && status != NV_ENC_ERR_NEED_MORE_INPUT) {
            Logger::instance().error(L"encoder", L"nvEncEncodePicture failed: " + nvStatusName(status));
            ++consecutiveEncodeFailures;
            // The frame never reached the encoder; restart the next GOP so the
            // stream continues from a clean reference state.
            forceKeyFrame = true;
            releaseBitstreamBuffer(bitstream);
            ++encoderStats.droppedFrames;
            if (api.nvEncUnmapInputResource(session, pendingInput.mapped) != NV_ENC_SUCCESS) {
                retainedMappings.push_back(
                    {pendingInput.mapped, pendingInput.inputTexture, std::move(pendingInput.lease)});
                return failInputMapping(L"nvEncUnmapInputResource failed after a rejected submission");
            }
            if (consecutiveEncodeFailures >= kMaxConsecutiveEncodeFailures) {
                return failEncoder(
                    L"nvEncEncodePicture failed " +
                    std::to_wstring(consecutiveEncodeFailures) +
                    L" times in a row (" + nvStatusName(status) +
                    L"); encoder faulted so the capture pipeline can recreate it");
            }
            return popReadyPacket(packet);
        }
        consecutiveEncodeFailures = 0;
        sessionFlushed = false;

        // Accepted input: NVENC produces exactly one output packet for it, in
        // decode order. Record the submission slot for decode-timestamp
        // assignment.
        decodeTimestamps.onSubmit(frame.pts100ns, pendingInput.duration100ns);

        if (status == NV_ENC_ERR_NEED_MORE_INPUT) {
            pendingOutputs.push_back(std::move(pending));
            pendingInputs.push_back(std::move(pendingInput));
            ++consecutiveNeedMoreInput;
            if (consecutiveNeedMoreInput == 1) {
                Logger::instance().info(L"encoder", L"NVENC delayed output until more input is available");
            } else if ((consecutiveNeedMoreInput % 300) == 0) {
                Logger::instance().warning(L"encoder",
                    L"NVENC is still delaying output for reordering/lookahead; count=" +
                    std::to_wstring(consecutiveNeedMoreInput));
            }
            return popReadyPacket(packet);
        }

        consecutiveNeedMoreInput = 0;
        pendingOutputs.push_back(std::move(pending));
        pendingInputs.push_back(std::move(pendingInput));
        ++readyOutputCount;
        // A submission cannot be complete while the reorder pipeline is still
        // filling, so polling then only costs a driver call (and this driver
        // answers that poll with OUT_OF_MEMORY). readyOutputCount keeps the
        // credit for the next harvest.
        if (outputShouldBeComplete(pendingOutputs.front()) &&
            !harvestPendingOutputs(false)) {
            return false;
        }
        return popReadyPacket(packet);
    }

    void drain(std::vector<EncodedPacket>& packets) {
        packets.clear();
        while (!readyPackets.empty()) {
            packets.push_back(std::move(readyPackets.front()));
            readyPackets.pop_front();
        }
        if (!session) {
            return;
        }

        // Harvest regardless of the EOS result: NEED_MORE_INPUT (and even some
        // error codes on quirky drivers) can precede valid frames still queued
        // in the encoder. Skipping the harvest here loses the tail of the
        // recording. The drain harvest is idle-bounded, so this stays safe.
        const bool flushed = sessionFlushed || sendEos();
        if (!flushed) {
            readyOutputCount = pendingOutputs.size();
            harvestPendingOutputs(false);
        } else {
            harvestPendingOutputs(true);
        }
        while (!readyPackets.empty()) {
            packets.push_back(std::move(readyPackets.front()));
            readyPackets.pop_front();
        }
    }

    bool destroyBitstreamBuffers() {
        if (!pendingOutputs.empty() || !pendingInputs.empty() || !retainedMappings.empty()) {
            return false;
        }
        readyPackets.clear();
        availableBitstreams.clear();
        readyOutputCount = 0;
        decodeTimestamps.reset(0);
        if (session) {
            for (NV_ENC_OUTPUT_PTR buffer : bitstreamBuffers) {
                if (buffer && api.nvEncDestroyBitstreamBuffer(session, buffer) != NV_ENC_SUCCESS) {
                    return failEncoder(L"nvEncDestroyBitstreamBuffer failed");
                }
            }
        }
        bitstreamBuffers.clear();
        consecutiveNeedMoreInput = 0;
        consecutiveEncodeFailures = 0;
        consecutiveOutputBufferStarvation = 0;
        return true;
    }

    bool shutdownEncoder() {
        if (session) {
            if (!flushAndUnregisterResources()) {
                Logger::instance().error(L"encoder",
                    L"NVENC session retained because its inputs could not be retired safely");
                return false;
            }
            if (!destroyBitstreamBuffers()) {
                return false;
            }
            if (api.nvEncDestroyEncoder(session) != NV_ENC_SUCCESS) {
                Logger::instance().error(L"encoder", L"nvEncDestroyEncoder failed; session retained");
                return false;
            }
            session = nullptr;
            sessionDevice.Reset();
            sessionFlushed = false;
        }
        if (module) {
            FreeLibrary(module);
            module = nullptr;
        }
        return true;
    }
};


Encoder::Encoder()
    : impl_(std::make_unique<Impl>()) {
}

Encoder::~Encoder() {
    shutdown();
}

void Encoder::abandonImpl() {
    // A failed retirement may still have driver work referencing the session,
    // its mappings, leases, and device. Keep the whole state alive for the
    // process lifetime instead of destroying storage underneath the driver.
    static auto* abandoned = new std::vector<std::unique_ptr<Impl>>();
    abandoned->push_back(std::move(impl_));
    impl_ = std::make_unique<Impl>();
}

bool Encoder::initialize(D3DDevice& device, const VideoSettings& settings) {
    {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->shutdownEncoder()) {
            abandonImpl();
        }
    }
    std::scoped_lock lock(impl_->mutex);
    impl_->availableAtomic.store(false, std::memory_order_release);
    impl_->settings = settings;
    impl_->forceKeyFrame = true;
    impl_->caps = {};
    impl_->encoderStats = {};
    impl_->caps.backendName = L"NVENC";
    impl_->caps.adapterName = device.adapterName();
    const bool initialized = impl_->initializeEncoder(device.device(), device.adapterName());
    if (!initialized) {
        impl_->shutdownEncoder();
    }
    return initialized;
}

void Encoder::requestKeyFrame() {
    impl_->forceKeyFrame = true;
}

bool Encoder::resetInputResources() {
    std::scoped_lock lock(impl_->mutex);
    return impl_->flushAndUnregisterResources();
}

bool Encoder::encodeFrame(const GpuFrame& frame, EncodedPacket& packet) {
    std::scoped_lock lock(impl_->mutex);
    return impl_->encode(frame, packet);
}

void Encoder::encodeFrame(const GpuFrame& frame, std::vector<EncodedPacket>& packets) {
    std::scoped_lock lock(impl_->mutex);
    packets.clear();
    impl_->encodeDrain(frame, packets);
}

void Encoder::drain(std::vector<EncodedPacket>& packets) {
    std::scoped_lock lock(impl_->mutex);
    packets.clear();
    impl_->drain(packets);
}

void Encoder::shutdown() {
    {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->shutdownEncoder()) {
            abandonImpl();
        }
    }
    std::scoped_lock lock(impl_->mutex);
    impl_->availableAtomic.store(false, std::memory_order_release);
    impl_->caps.available = false;
    impl_->encoderStats.encoderAvailable = false;
}

bool Encoder::encoderAvailable() const noexcept {
    return impl_->availableAtomic.load(std::memory_order_acquire);
}

EncoderCapabilities Encoder::capabilities() const {
    std::unique_lock lock(impl_->mutex, std::try_to_lock);
    std::scoped_lock snap(impl_->snapshotMutex);
    if (lock.owns_lock()) {
        impl_->capsSnapshot = impl_->caps;
    }
    return impl_->capsSnapshot;
}

EncoderStats Encoder::stats() const {
    std::unique_lock lock(impl_->mutex, std::try_to_lock);
    std::scoped_lock snap(impl_->snapshotMutex);
    if (lock.owns_lock()) {
        impl_->statsSnapshot = impl_->encoderStats;
    }
    return impl_->statsSnapshot;
}

DXGI_FORMAT Encoder::preferredInputFormat() const {
    // NVENC registers BGRA D3D11 textures directly (NV_ENC_BUFFER_FORMAT_ARGB).
    return DXGI_FORMAT_B8G8R8A8_UNORM;
}

uint32_t Encoder::inputRetentionBudget() const {
    // Accepted inputs stay mapped and pinned until their output is locked;
    // pending outputs are bounded by the bitstream buffers we created, which
    // already account for lookahead and B-frame delay (min 4). This is what
    // the encoder-feeding pool must hold so deep lookahead cannot starve the
    // producer.
    return impl_ ? impl_->desiredBitstreamBufferCount() : 0;
}

} // namespace BACKTRACK_NVENC_NAMESPACE
} // namespace backtrack
