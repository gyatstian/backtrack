#include "encoder/NvencEncoder.h"

#include "core/Logger.h"
#include "encoder/BitstreamUtils.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <numeric>
#include <unordered_map>

#ifndef BACKTRACK_HAS_NVENC
#if __has_include(<nvEncodeAPI.h>)
#define BACKTRACK_HAS_NVENC 1
#else
#define BACKTRACK_HAS_NVENC 0
#endif
#endif

#if BACKTRACK_HAS_NVENC
#include <nvEncodeAPI.h>
#endif

namespace backtrack {

#if BACKTRACK_HAS_NVENC

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
    switch (preset) {
    case EncoderPreset::P1:
        return NV_ENC_PRESET_P1_GUID;
    case EncoderPreset::P2:
        return NV_ENC_PRESET_P2_GUID;
    case EncoderPreset::P3:
        return NV_ENC_PRESET_P3_GUID;
    case EncoderPreset::P4:
        return NV_ENC_PRESET_P4_GUID;
    case EncoderPreset::P5:
        return NV_ENC_PRESET_P5_GUID;
    case EncoderPreset::P6:
        return NV_ENC_PRESET_P6_GUID;
    case EncoderPreset::P7:
        return NV_ENC_PRESET_P7_GUID;
    }
    return NV_ENC_PRESET_P4_GUID;
}

NV_ENC_TUNING_INFO tuningInfo(EncoderMode mode) {
    switch (mode) {
    case EncoderMode::HighQuality:
        return NV_ENC_TUNING_INFO_HIGH_QUALITY;
    case EncoderMode::LowLatency:
        return NV_ENC_TUNING_INFO_LOW_LATENCY;
    case EncoderMode::UltraLowLatency:
        return NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    case EncoderMode::Lossless:
        return NV_ENC_TUNING_INFO_LOSSLESS;
    case EncoderMode::UltraHighQuality:
        return NV_ENC_TUNING_INFO_ULTRA_HIGH_QUALITY;
    }
    return NV_ENC_TUNING_INFO_HIGH_QUALITY;
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

NV_ENC_MULTI_PASS nvencMultipass(EncoderMultipass multipass) {
    switch (multipass) {
    case EncoderMultipass::QuarterResolution:
        return NV_ENC_TWO_PASS_QUARTER_RESOLUTION;
    case EncoderMultipass::FullResolution:
        return NV_ENC_TWO_PASS_FULL_RESOLUTION;
    case EncoderMultipass::Disabled:
        break;
    }
    return NV_ENC_MULTI_PASS_DISABLED;
}

NV_ENC_NUM_REF_FRAMES nvencReferenceFrames(uint32_t referenceFrames) {
    switch (std::clamp<uint32_t>(referenceFrames, 1, 4)) {
    case 1:
        return NV_ENC_NUM_REF_FRAMES_1;
    case 2:
        return NV_ENC_NUM_REF_FRAMES_2;
    case 3:
        return NV_ENC_NUM_REF_FRAMES_3;
    case 4:
        return NV_ENC_NUM_REF_FRAMES_4;
    }
    return NV_ENC_NUM_REF_FRAMES_1;
}

} // namespace

struct NvencEncoder::Impl {
    using CreateInstanceFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);

    HMODULE module = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api{};
    void* session = nullptr;
    VideoSettings settings;
    EncoderCapabilities caps;
    EncoderStats encoderStats;
    mutable std::mutex mutex;

    struct RegisteredResource {
        // Hold a strong ref so a freed+reallocated texture cannot reuse this address
        // while the registration is still cached.
        ComPtr<ID3D11Texture2D> texture;
        NV_ENC_REGISTERED_PTR registered = nullptr;
        uint64_t generation = 0;
    };

    // Bumped on full invalidation (pool recreation / resetInputResources).
    uint64_t registrationGeneration = 1;

    struct PendingOutput {
        NV_ENC_OUTPUT_PTR bitstream = nullptr;
        int64_t fallbackPts100ns = 0;
        int64_t fallbackDuration100ns = 0;
        bool requestedKeyFrame = false;
        std::chrono::steady_clock::time_point submittedAt;
    };

    std::unordered_map<ID3D11Texture2D*, RegisteredResource> registeredResources;
    std::vector<NV_ENC_OUTPUT_PTR> bitstreamBuffers;
    std::deque<NV_ENC_OUTPUT_PTR> availableBitstreams;
    std::deque<PendingOutput> pendingOutputs;
    std::deque<EncodedPacket> readyPackets;
    std::atomic<bool> forceKeyFrame{true};
    size_t readyOutputCount = 0;
    uint32_t consecutiveRegisterFailures = 0;
    uint32_t consecutiveMapFailures = 0;
    uint32_t consecutiveNeedMoreInput = 0;
    uint32_t consecutiveOutputBufferStarvation = 0;
    uint32_t consecutiveLockFailures = 0;
    bool inputMappingFaulted = false;

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
        encoderStats.encoderAvailable = caps.available;
    }

    bool initializeEncoder(ID3D11Device* device, const std::wstring& adapterName) {
        consecutiveMapFailures = 0;
        consecutiveRegisterFailures = 0;
        inputMappingFaulted = false;
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
        encodeConfig.rcParams.enableExtLookahead = 0;
        encodeConfig.rcParams.enableNonRefP = 0;
        encodeConfig.rcParams.strictGOPTarget = 0;
        encodeConfig.rcParams.qpMapMode = NV_ENC_QP_MAP_DISABLED;

        if (settings.codec == VideoCodec::H264) {
            auto& h264 = encodeConfig.encodeCodecConfig.h264Config;
            h264.idrPeriod = encodeConfig.gopLength;
            h264.repeatSPSPPS = 1;
            h264.enableTemporalSVC = 0;
            h264.hierarchicalPFrames = 0;
            h264.hierarchicalBFrames = 0;
            h264.enableIntraRefresh = 0;
            h264.enableLTR = 0;
            h264.maxNumRefFrames = caps.effective.referenceFrames;
            h264.numRefL0 = nvencReferenceFrames(caps.effective.referenceFrames);
            h264.numRefL1 = caps.effective.bFrames ? nvencReferenceFrames(1) : NV_ENC_NUM_REF_FRAMES_AUTOSELECT;
            h264.useBFramesAsRef = NV_ENC_BFRAME_REF_MODE_DISABLED;
            h264.tfLevel = NV_ENC_TEMPORAL_FILTER_LEVEL_0;
        } else {
            auto& hevc = encodeConfig.encodeCodecConfig.hevcConfig;
            hevc.idrPeriod = encodeConfig.gopLength;
            hevc.repeatSPSPPS = 1;
            hevc.enableTemporalSVC = 0;
            hevc.enableIntraRefresh = 0;
            hevc.enableLTR = 0;
            hevc.maxNumRefFramesInDPB = caps.effective.referenceFrames;
            hevc.numRefL0 = nvencReferenceFrames(caps.effective.referenceFrames);
            hevc.numRefL1 = caps.effective.bFrames ? nvencReferenceFrames(1) : NV_ENC_NUM_REF_FRAMES_AUTOSELECT;
            hevc.useBFramesAsRef = NV_ENC_BFRAME_REF_MODE_DISABLED;
            hevc.tfLevel = NV_ENC_TEMPORAL_FILTER_LEVEL_0;
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
            L"NVENC initialized in zero-copy D3D11 mode at " +
            std::to_wstring(settings.width) + L"x" + std::to_wstring(settings.height) +
            L" with " + std::to_wstring(bitstreamBufferCount) + L" output buffers");
        return true;
    }

    uint32_t desiredBitstreamBufferCount() const {
        const uint32_t bFrameDelay = caps.effective.bFrames ? 2 : 0;
        const uint32_t lookaheadDelay = caps.effective.lookahead ? caps.effective.lookaheadDepth : 0;
        return std::clamp<uint32_t>(lookaheadDelay + bFrameDelay + 4, 4, 40);
    }

    NV_ENC_REGISTERED_PTR registeredResourceFor(ID3D11Texture2D* texture, NV_ENC_BUFFER_FORMAT bufferFormat) {
        const auto existing = registeredResources.find(texture);
        if (existing != registeredResources.end()) {
            // Reject stale entries: generation mismatch or pointer identity no longer matches
            // the held ComPtr (defensive against address reuse after partial invalidation).
            if (existing->second.generation == registrationGeneration &&
                existing->second.texture.Get() == texture &&
                existing->second.registered) {
                return existing->second.registered;
            }
            if (existing->second.registered && session) {
                api.nvEncUnregisterResource(session, existing->second.registered);
            }
            registeredResources.erase(existing);
        }

        NV_ENC_REGISTER_RESOURCE resource{};
        resource.version = NV_ENC_REGISTER_RESOURCE_VER;
        resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        resource.resourceToRegister = texture;
        resource.width = settings.width;
        resource.height = settings.height;
        resource.pitch = 0;
        resource.bufferFormat = bufferFormat;
        resource.bufferUsage = NV_ENC_INPUT_IMAGE;

        NVENCSTATUS status = api.nvEncRegisterResource(session, &resource);
        if (status != NV_ENC_SUCCESS) {
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

        consecutiveRegisterFailures = 0;
        RegisteredResource cached;
        cached.texture = texture;
        cached.registered = resource.registeredResource;
        cached.generation = registrationGeneration;
        registeredResources.emplace(texture, std::move(cached));
        return resource.registeredResource;
    }

    void unregisterResource(ID3D11Texture2D* texture, NV_ENC_REGISTERED_PTR registered) {
        if (registered && session) {
            api.nvEncUnregisterResource(session, registered);
        }
        registeredResources.erase(texture);
    }

    void unregisterAllResources() {
        if (session) {
            for (auto& item : registeredResources) {
                api.nvEncUnregisterResource(session, item.second.registered);
            }
        }
        registeredResources.clear();
        // Invalidate any concurrent lookups that might still hold raw NV_ENC_REGISTERED_PTR
        // from a previous pool epoch (pool recreation path).
        ++registrationGeneration;
        if (registrationGeneration == 0) {
            registrationGeneration = 1;
        }
        consecutiveRegisterFailures = 0;
        consecutiveMapFailures = 0;
    }

    void noteMapFailure(NVENCSTATUS status) {
        ++consecutiveMapFailures;
        if (consecutiveMapFailures == 1) {
            Logger::instance().error(L"encoder", L"nvEncMapInputResource failed: " + nvStatusName(status));
        } else if (consecutiveMapFailures == 30) {
            inputMappingFaulted = true;
            caps.available = false;
            caps.detail = L"NVENC input mapping failed repeatedly; capture pipeline disabled encoder until restart";
            encoderStats.encoderAvailable = false;
            Logger::instance().error(L"encoder", caps.detail);
            shutdownEncoder();
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

        consecutiveOutputBufferStarvation = 0;
        NV_ENC_OUTPUT_PTR bitstream = availableBitstreams.front();
        availableBitstreams.pop_front();
        return bitstream;
    }

    void releaseBitstreamBuffer(NV_ENC_OUTPUT_PTR bitstream) {
        if (bitstream) {
            availableBitstreams.push_back(bitstream);
        }
    }

    bool failEncoder(const std::wstring& detail) {
        inputMappingFaulted = true;
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

    bool harvestPendingOutputs(bool waitForCompletion) {
        while (!pendingOutputs.empty() && (waitForCompletion || readyOutputCount > 0)) {
            PendingOutput pending = std::move(pendingOutputs.front());
            pendingOutputs.pop_front();

            NV_ENC_LOCK_BITSTREAM lock{};
            lock.version = NV_ENC_LOCK_BITSTREAM_VER;
            lock.outputBitstream = pending.bitstream;
            // This is a synchronous NVENC session. The driver completes the
            // encode before this lock returns; polling can report unsupported
            // errors on older NVIDIA drivers instead of NV_ENC_ERR_LOCK_BUSY.
            lock.doNotWait = 0;
            const NVENCSTATUS status = api.nvEncLockBitstream(session, &lock);
            if (status == NV_ENC_ERR_LOCK_BUSY && !waitForCompletion) {
                pendingOutputs.push_front(std::move(pending));
                break;
            }
            if (status != NV_ENC_SUCCESS) {
                ++consecutiveLockFailures;
                ++encoderStats.droppedFrames;
                return failEncoder(
                    L"NVENC output lock failed: " + nvStatusName(status) +
                    L"; encoder was reset to stop recycling a bad output buffer");
            }
            if (!waitForCompletion && readyOutputCount > 0) {
                --readyOutputCount;
            }
            consecutiveLockFailures = 0;

            EncodedPacket packet;
            const auto* begin = static_cast<const uint8_t*>(lock.bitstreamBufferPtr);
            packet.bytes.assign(begin, begin + lock.bitstreamSizeInBytes);
            packet.pts100ns = lock.outputTimeStamp != 0
                ? static_cast<int64_t>(lock.outputTimeStamp)
                : pending.fallbackPts100ns;
            packet.duration100ns = lock.outputDuration != 0
                ? static_cast<int64_t>(lock.outputDuration)
                : pending.fallbackDuration100ns;
            const bool metadataKeyFrame = lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I;
            const bool requestedOutputKeyFrame = pending.requestedKeyFrame && packet.pts100ns == pending.fallbackPts100ns;
            packet.keyFrame = requestedOutputKeyFrame || metadataKeyFrame || bitstreamContainsKeyFrame(settings.codec, packet.bytes);
            packet.codec = settings.codec;

            api.nvEncUnlockBitstream(session, pending.bitstream);
            releaseBitstreamBuffer(pending.bitstream);

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
        }
        if (waitForCompletion) {
            readyOutputCount = 0;
        }
        return true;
    }

    bool encode(const GpuFrame& frame, EncodedPacket& packet) {
        if (inputMappingFaulted || !session || bitstreamBuffers.empty() || !frame.texture) {
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

        NV_ENC_REGISTERED_PTR registered = registeredResourceFor(frame.texture.Get(), expectedBufferFormat);
        if (!registered) {
            ++encoderStats.droppedFrames;
            return false;
        }

        NV_ENC_MAP_INPUT_RESOURCE map{};
        map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        map.registeredResource = registered;
        NVENCSTATUS status = api.nvEncMapInputResource(session, &map);
        if (status != NV_ENC_SUCCESS) {
            unregisterResource(frame.texture.Get(), registered);
            registered = registeredResourceFor(frame.texture.Get(), expectedBufferFormat);
            if (registered) {
                map = {};
                map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
                map.registeredResource = registered;
                status = api.nvEncMapInputResource(session, &map);
            }
            if (status != NV_ENC_SUCCESS) {
                noteMapFailure(status);
                ++encoderStats.droppedFrames;
                return popReadyPacket(packet);
            }
        }
        consecutiveMapFailures = 0;

        NV_ENC_OUTPUT_PTR bitstream = acquireBitstreamBuffer();
        if (!bitstream) {
            api.nvEncUnmapInputResource(session, map.mappedResource);
            ++encoderStats.droppedFrames;
            return popReadyPacket(packet);
        }

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
        pic.inputTimeStamp = frame.pts100ns;
        pic.inputDuration = frame.duration100ns > 0
            ? static_cast<uint64_t>(frame.duration100ns)
            : static_cast<uint64_t>(
                kHundredNanosecondsPerSecond / std::max<uint32_t>(settings.fps, 1));
        pic.frameIdx = static_cast<uint32_t>(encoderStats.submittedFrames);
        const bool requestedKeyFrame = forceKeyFrame.exchange(false);
        if (requestedKeyFrame) {
            pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        }

        ++encoderStats.submittedFrames;
        status = api.nvEncEncodePicture(session, &pic);
        api.nvEncUnmapInputResource(session, map.mappedResource);

        PendingOutput pending;
        pending.bitstream = bitstream;
        pending.fallbackPts100ns = frame.pts100ns;
        pending.fallbackDuration100ns = static_cast<int64_t>(pic.inputDuration);
        pending.requestedKeyFrame = requestedKeyFrame;
        pending.submittedAt = start;

        if (status == NV_ENC_ERR_NEED_MORE_INPUT) {
            pendingOutputs.push_back(std::move(pending));
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
        if (status != NV_ENC_SUCCESS) {
            Logger::instance().error(L"encoder", L"nvEncEncodePicture failed: " + nvStatusName(status));
            if (requestedKeyFrame) {
                forceKeyFrame = true;
            }
            releaseBitstreamBuffer(bitstream);
            ++encoderStats.droppedFrames;
            return popReadyPacket(packet);
        }

        consecutiveNeedMoreInput = 0;
        pendingOutputs.push_back(std::move(pending));
        ++readyOutputCount;
        if (!harvestPendingOutputs(false)) {
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

        NV_ENC_PIC_PARAMS pic{};
        pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        const NVENCSTATUS status = api.nvEncEncodePicture(session, &pic);
        if (status != NV_ENC_SUCCESS && status != NV_ENC_ERR_NEED_MORE_INPUT) {
            Logger::instance().error(L"encoder", L"nvEncEncodePicture(EOS) failed: " + nvStatusName(status));
        }

        if (status == NV_ENC_SUCCESS) {
            readyOutputCount = pendingOutputs.size();
            harvestPendingOutputs(true);
        }
        while (!readyPackets.empty()) {
            packets.push_back(std::move(readyPackets.front()));
            readyPackets.pop_front();
        }
    }

    void destroyBitstreamBuffers() {
        pendingOutputs.clear();
        readyPackets.clear();
        availableBitstreams.clear();
        readyOutputCount = 0;
        if (session) {
            for (NV_ENC_OUTPUT_PTR buffer : bitstreamBuffers) {
                if (buffer) {
                    api.nvEncDestroyBitstreamBuffer(session, buffer);
                }
            }
        }
        bitstreamBuffers.clear();
        consecutiveNeedMoreInput = 0;
        consecutiveOutputBufferStarvation = 0;
        consecutiveLockFailures = 0;
    }

    void shutdownEncoder() {
        if (session) {
            unregisterAllResources();
            destroyBitstreamBuffers();

            api.nvEncDestroyEncoder(session);
            session = nullptr;
        }
        if (module) {
            FreeLibrary(module);
            module = nullptr;
        }
    }
};

#else

struct NvencEncoder::Impl {
    VideoSettings settings;
    EncoderCapabilities caps;
    EncoderStats encoderStats;
    mutable std::mutex mutex;
    std::atomic<bool> forceKeyFrame{false};
};

#endif

NvencEncoder::NvencEncoder()
    : impl_(std::make_unique<Impl>()) {
}

NvencEncoder::~NvencEncoder() {
    shutdown();
}

bool NvencEncoder::initialize(D3DDevice& device, const VideoSettings& settings) {
    std::scoped_lock lock(impl_->mutex);
#if BACKTRACK_HAS_NVENC
    impl_->shutdownEncoder();
#endif
    impl_->settings = settings;
    impl_->forceKeyFrame = true;
    impl_->caps = {};
    impl_->encoderStats = {};
    impl_->caps.backendName = L"NVENC";
#if BACKTRACK_HAS_NVENC
    impl_->caps.adapterName = device.adapterName();
    const bool initialized = impl_->initializeEncoder(device.device(), device.adapterName());
    if (!initialized) {
        impl_->shutdownEncoder();
    }
    return initialized;
#else
    impl_->caps.available = false;
    impl_->caps.adapterName = device.adapterName();
    impl_->caps.detail = L"Built without nvEncodeAPI.h. Install NVIDIA Video Codec SDK and rebuild to enable NVENC.";
    impl_->encoderStats.encoderAvailable = false;
    Logger::instance().error(L"encoder", impl_->caps.detail);
    return false;
#endif
}

void NvencEncoder::requestKeyFrame() {
#if BACKTRACK_HAS_NVENC
    impl_->forceKeyFrame = true;
#else
    impl_->forceKeyFrame = true;
#endif
}

void NvencEncoder::resetInputResources() {
    std::scoped_lock lock(impl_->mutex);
#if BACKTRACK_HAS_NVENC
    impl_->unregisterAllResources();
#endif
}

bool NvencEncoder::encodeFrame(const GpuFrame& frame, EncodedPacket& packet) {
    std::scoped_lock lock(impl_->mutex);
#if BACKTRACK_HAS_NVENC
    return impl_->encode(frame, packet);
#else
    (void)frame;
    packet = {};
    ++impl_->encoderStats.droppedFrames;
    return false;
#endif
}

void NvencEncoder::drain(std::vector<EncodedPacket>& packets) {
    std::scoped_lock lock(impl_->mutex);
    packets.clear();
#if BACKTRACK_HAS_NVENC
    impl_->drain(packets);
#endif
}

void NvencEncoder::shutdown() {
    std::scoped_lock lock(impl_->mutex);
#if BACKTRACK_HAS_NVENC
    impl_->shutdownEncoder();
#endif
    impl_->caps.available = false;
    impl_->encoderStats.encoderAvailable = false;
}

EncoderCapabilities NvencEncoder::capabilities() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->caps;
}

EncoderStats NvencEncoder::stats() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->encoderStats;
}

DXGI_FORMAT NvencEncoder::preferredInputFormat() const {
    // NVENC registers BGRA D3D11 textures directly (NV_ENC_BUFFER_FORMAT_ARGB).
    return DXGI_FORMAT_B8G8R8A8_UNORM;
}

} // namespace backtrack
