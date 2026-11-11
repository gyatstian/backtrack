#include "encoder/AmfEncoder.cpp"

#if !BACKTRACK_HAS_AMF
#error AmfEncoderTests requires the AMF SDK headers
#endif

#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace backtrack {

struct AmfEncoderTestAccess {
    static void attach(AmfEncoder& owner, amf::AMFComponent* component,
                       amf::AMFContext* context, amf::AMFFactory* factory,
                       const VideoSettings& settings) {
        auto& impl = *owner.impl_;
        impl.settings = settings;
        impl.encoder = component;
        impl.context = context;
        impl.factory = factory;
        impl.caps.available = true;
        impl.publishAvailability(true);
    }

    static void updateCaps(AmfEncoder& owner) { owner.impl_->updateCapabilities(L"fake"); }
    static bool configure(AmfEncoder& owner, const EncoderEffectiveSettings& effective) {
        auto& impl = *owner.impl_;
        return impl.isHevc() ? impl.configureHevc(effective) : impl.configureAvc(effective);
    }
    static bool configure(AmfEncoder& owner) {
        return configure(owner, effectiveSettingsFor(owner.impl_->settings));
    }
    static void updateDetail(AmfEncoder& owner, const EncoderEffectiveSettings& requested) {
        owner.impl_->updateActiveDetail(requested);
    }
    static bool createComponent(AmfEncoder& owner) { return owner.impl_->createComponent(); }
    static bool switchFormat(AmfEncoder& owner) {
        return owner.impl_->switchInputFormat(amf::AMF_SURFACE_BGRA);
    }
    static bool faulted(const AmfEncoder& owner) { return owner.impl_->encoderFaulted; }
    static bool resourcesReleased(const AmfEncoder& owner) {
        return !owner.impl_->encoder && !owner.impl_->context && !owner.impl_->factory;
    }
    static uint32_t delay(const AmfEncoder& owner) { return owner.impl_->reorderDelayFrames(); }
    static size_t inFlightCap(const AmfEncoder& owner) { return owner.impl_->inFlightCap(); }
    static void retain(AmfEncoder& owner, const std::shared_ptr<void>& lease) {
        AmfEncoder::Impl::PendingInput input;
        input.lease = lease;
        owner.impl_->inFlight.push_back(std::move(input));
    }
    static void setPreviousBFrames(AmfEncoder& owner, bool enabled) {
        owner.impl_->caps.effective.bFrames = enabled;
        owner.impl_->caps.effective.zeroReorderDelay = !enabled;
        owner.impl_->bFrameDelayFrames = enabled ? 2 : 0;
    }
    static void seedTimestamp(AmfEncoder& owner) { owner.impl_->decodeTimestamps.onSubmit(999999); }
    static int64_t firstDts(AmfEncoder& owner) {
        owner.impl_->decodeTimestamps.onSubmit(0);
        return owner.impl_->decodeTimestamps.onOutput(0, 100000);
    }
};

} // namespace backtrack

namespace {

using namespace backtrack;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<class Base>
struct PropertyStorage : Base {
    std::map<std::wstring, amf::AMFVariant> properties;
    std::vector<std::pair<std::wstring, amf::AMFVariant>> writes;
    std::set<std::wstring> rejectWrites;
    std::set<std::wstring> unreadable;
    bool terminated = false;
    mutable int callsAfterTerminate = 0;
    int terminateCalls = 0;

    void touch() const { if (terminated) ++callsAfterTerminate; }
    amf_long AMF_STD_CALL Acquire() override { return 1; }
    amf_long AMF_STD_CALL Release() override { return 1; }
    AMF_RESULT AMF_STD_CALL QueryInterface(const amf::AMFGuid&, void**) override { return AMF_NO_INTERFACE; }
    AMF_RESULT AMF_STD_CALL SetProperty(const wchar_t* name, amf::AMFVariantStruct value) override {
        touch();
        writes.emplace_back(name, amf::AMFVariant(value));
        if (rejectWrites.count(name)) return AMF_NOT_SUPPORTED;
        properties.insert_or_assign(name, amf::AMFVariant(value));
        return AMF_OK;
    }
    AMF_RESULT AMF_STD_CALL GetProperty(const wchar_t* name, amf::AMFVariantStruct* value) const override {
        touch();
        const auto found = properties.find(name);
        if (unreadable.count(name) || found == properties.end()) return AMF_NOT_FOUND;
        return amf::AMFVariantCopy(value, &found->second);
    }
    amf_bool AMF_STD_CALL HasProperty(const wchar_t*) const override { return false; }
    amf_size AMF_STD_CALL GetPropertyCount() const override { return 0; }
    AMF_RESULT AMF_STD_CALL GetPropertyAt(amf_size, wchar_t*, amf_size, amf::AMFVariantStruct*) const override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL Clear() override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL AddTo(amf::AMFPropertyStorage*, amf_bool, amf_bool) const override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL CopyTo(amf::AMFPropertyStorage*, amf_bool) const override { return AMF_NOT_IMPLEMENTED; }
    void AMF_STD_CALL AddObserver(amf::AMFPropertyStorageObserver*) override {}
    void AMF_STD_CALL RemoveObserver(amf::AMFPropertyStorageObserver*) override {}

    void seed(const wchar_t* name, const amf::AMFVariant& value) { properties.insert_or_assign(name, value); }
    size_t writesTo(const wchar_t* name) const {
        return static_cast<size_t>(std::count_if(writes.begin(), writes.end(),
            [name](const auto& write) { return write.first == name; }));
    }
};

struct FakeCaps : PropertyStorage<amf::AMFCaps> {
    amf::AMF_ACCELERATION_TYPE AMF_STD_CALL GetAccelerationType() const override { return amf::AMF_ACCEL_HARDWARE; }
    AMF_RESULT AMF_STD_CALL GetInputCaps(amf::AMFIOCaps**) override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL GetOutputCaps(amf::AMFIOCaps**) override { return AMF_NOT_IMPLEMENTED; }
};

struct FakeComponent : PropertyStorage<amf::AMFComponent> {
    FakeCaps caps;
    AMF_RESULT outputResult = AMF_REPEAT;
    int queries = 0;
    size_t writesAtInit = 0;
    bool overridePattern = false;
    bool overridePatternAfterRateControl = false;
    amf_int64 patternAfterInit = 0;
    bool overridePreAnalysis = false;
    bool overridePreAnalysisAfterRateControl = false;
    const wchar_t* preAnalysisName = AMF_VIDEO_ENCODER_PRE_ANALYSIS_ENABLE;

    AMF_RESULT AMF_STD_CALL SetProperty(const wchar_t* name, amf::AMFVariantStruct value) override {
        const auto result = PropertyStorage<amf::AMFComponent>::SetProperty(name, value);
        const std::wstring key(name);
        if (result == AMF_OK && (key == AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD ||
                                 key == AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD)) {
            if (overridePatternAfterRateControl) {
                seed(AMF_VIDEO_ENCODER_B_PIC_PATTERN, amf::AMFVariant(patternAfterInit));
            }
            if (overridePreAnalysisAfterRateControl) seed(preAnalysisName, amf::AMFVariant(true));
        }
        return result;
    }
    amf_size AMF_STD_CALL GetPropertiesInfoCount() const override { return 0; }
    AMF_RESULT AMF_STD_CALL GetPropertyInfo(amf_size, const amf::AMFPropertyInfo**) const override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL GetPropertyInfo(const wchar_t*, const amf::AMFPropertyInfo**) const override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL ValidateProperty(const wchar_t*, amf::AMFVariantStruct, amf::AMFVariantStruct*) const override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL Init(amf::AMF_SURFACE_FORMAT, amf_int32, amf_int32) override {
        touch();
        writesAtInit = writes.size();
        if (overridePattern) seed(AMF_VIDEO_ENCODER_B_PIC_PATTERN, amf::AMFVariant(patternAfterInit));
        if (overridePreAnalysis) seed(preAnalysisName, amf::AMFVariant(true));
        return AMF_OK;
    }
    AMF_RESULT AMF_STD_CALL ReInit(amf_int32, amf_int32) override { touch(); return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL Terminate() override { touch(); ++terminateCalls; terminated = true; return AMF_OK; }
    AMF_RESULT AMF_STD_CALL Drain() override { touch(); return AMF_OK; }
    AMF_RESULT AMF_STD_CALL Flush() override { touch(); return AMF_OK; }
    AMF_RESULT AMF_STD_CALL SubmitInput(amf::AMFData*) override { touch(); return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL QueryOutput(amf::AMFData** data) override { touch(); ++queries; *data = nullptr; return outputResult; }
    amf::AMFContext* AMF_STD_CALL GetContext() override { touch(); return nullptr; }
    AMF_RESULT AMF_STD_CALL SetOutputDataAllocatorCB(amf::AMFDataAllocatorCB*) override { touch(); return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL GetCaps(amf::AMFCaps** output) override { touch(); caps.Acquire(); *output = &caps; return AMF_OK; }
    AMF_RESULT AMF_STD_CALL Optimize(amf::AMFComponentOptimizationCallback*) override { touch(); return AMF_NOT_IMPLEMENTED; }
};

struct FakeContext : PropertyStorage<amf::AMFContext> {
    AMF_RESULT AMF_STD_CALL Terminate() override { touch(); ++terminateCalls; terminated = true; return AMF_OK; }
    AMF_RESULT AMF_STD_CALL InitDX9(void*) override { return AMF_NOT_IMPLEMENTED; }
    void* AMF_STD_CALL GetDX9Device(amf::AMF_DX_VERSION) override { return nullptr; }
    AMF_RESULT AMF_STD_CALL LockDX9() override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL UnlockDX9() override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL InitDX11(void*, amf::AMF_DX_VERSION) override { return AMF_NOT_IMPLEMENTED; }
    void* AMF_STD_CALL GetDX11Device(amf::AMF_DX_VERSION) override { return nullptr; }
    AMF_RESULT AMF_STD_CALL LockDX11() override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL UnlockDX11() override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL InitOpenCL(void*) override { return AMF_NOT_IMPLEMENTED; }
    void* AMF_STD_CALL GetOpenCLContext() override { return nullptr; }
    void* AMF_STD_CALL GetOpenCLCommandQueue() override { return nullptr; }
    void* AMF_STD_CALL GetOpenCLDeviceID() override { return nullptr; }
    AMF_RESULT AMF_STD_CALL GetOpenCLComputeFactory(amf::AMFComputeFactory**) override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL InitOpenCLEx(amf::AMFComputeDevice*) override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL LockOpenCL() override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL UnlockOpenCL() override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL InitOpenGL(amf_handle, amf_handle, amf_handle) override { return AMF_NOT_IMPLEMENTED; }
    amf_handle AMF_STD_CALL GetOpenGLContext() override { return nullptr; }
    amf_handle AMF_STD_CALL GetOpenGLDrawable() override { return nullptr; }
    AMF_RESULT AMF_STD_CALL LockOpenGL() override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL UnlockOpenGL() override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL InitXV(void*) override { return AMF_NOT_IMPLEMENTED; }
    void* AMF_STD_CALL GetXVDevice() override { return nullptr; }
    AMF_RESULT AMF_STD_CALL LockXV() override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL UnlockXV() override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL InitGralloc(void*) override { return AMF_NOT_IMPLEMENTED; }
    void* AMF_STD_CALL GetGrallocDevice() override { return nullptr; }
    AMF_RESULT AMF_STD_CALL LockGralloc() override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL UnlockGralloc() override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL AllocBuffer(amf::AMF_MEMORY_TYPE, amf_size, amf::AMFBuffer**) override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL AllocSurface(amf::AMF_MEMORY_TYPE, amf::AMF_SURFACE_FORMAT, amf_int32, amf_int32, amf::AMFSurface**) override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL AllocAudioBuffer(amf::AMF_MEMORY_TYPE, amf::AMF_AUDIO_FORMAT, amf_int32, amf_int32, amf_int32, amf::AMFAudioBuffer**) override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL CreateBufferFromHostNative(void*, amf_size, amf::AMFBuffer**, amf::AMFBufferObserver*) override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL CreateSurfaceFromHostNative(amf::AMF_SURFACE_FORMAT, amf_int32, amf_int32, amf_int32, amf_int32, void*, amf::AMFSurface**, amf::AMFSurfaceObserver*) override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL CreateSurfaceFromDX9Native(void*, amf::AMFSurface**, amf::AMFSurfaceObserver*) override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL CreateSurfaceFromDX11Native(void*, amf::AMFSurface**, amf::AMFSurfaceObserver*) override { touch(); return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL CreateSurfaceFromOpenGLNative(amf::AMF_SURFACE_FORMAT, amf_handle, amf::AMFSurface**, amf::AMFSurfaceObserver*) override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL CreateSurfaceFromGrallocNative(amf_handle, amf::AMFSurface**, amf::AMFSurfaceObserver*) override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL CreateSurfaceFromOpenCLNative(amf::AMF_SURFACE_FORMAT, amf_int32, amf_int32, void**, amf::AMFSurface**, amf::AMFSurfaceObserver*) override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL CreateBufferFromOpenCLNative(void*, amf_size, amf::AMFBuffer**) override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL GetCompute(amf::AMF_MEMORY_TYPE, amf::AMFCompute**) override { return AMF_NOT_IMPLEMENTED; }
};

struct FakeFactory : amf::AMFFactory {
    FakeComponent* next = nullptr;
    int creates = 0;
    AMF_RESULT createResult = AMF_OK;
    std::wstring lastComponentId;
    AMF_RESULT AMF_STD_CALL CreateContext(amf::AMFContext**) override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL CreateComponent(amf::AMFContext*, const wchar_t* id, amf::AMFComponent** component) override {
        ++creates;
        lastComponentId = id;
        if (createResult != AMF_OK) return createResult;
        if (!next) return AMF_OK;
        next->Acquire();
        *component = next;
        return AMF_OK;
    }
    AMF_RESULT AMF_STD_CALL SetCacheFolder(const wchar_t*) override { return AMF_NOT_IMPLEMENTED; }
    const wchar_t* AMF_STD_CALL GetCacheFolder() override { return L""; }
    AMF_RESULT AMF_STD_CALL GetDebug(amf::AMFDebug**) override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL GetTrace(amf::AMFTrace**) override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL GetPrograms(amf::AMFPrograms**) override { return AMF_NOT_IMPLEMENTED; }
};

struct FakeTexture : ID3D11Texture2D {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    void STDMETHODCALLTYPE GetDevice(ID3D11Device**) override {}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION*) override {}
    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }
    void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE2D_DESC*) override {}
};

VideoSettings requestedSettings(VideoCodec codec = VideoCodec::H264) {
    VideoSettings settings;
    settings.codec = codec;
    settings.encoderProfile = EncoderProfile::Custom;
    settings.encoderLookahead = true;
    settings.encoderLookaheadDepth = 24;
    settings.encoderBFrames = true;
    settings.encoderAdaptiveBFrames = true;
    settings.encoderAdaptiveIFrames = true;
    settings.encoderZeroReorderDelay = false;
    return settings;
}

void setCaps(FakeComponent& component, bool bFrames, bool preAnalysis) {
    component.caps.seed(AMF_VIDEO_ENCODER_CAP_BFRAMES, amf::AMFVariant(bFrames));
    component.caps.seed(AMF_VIDEO_ENCODER_CAP_PRE_ANALYSIS, amf::AMFVariant(preAnalysis));
    component.caps.seed(AMF_VIDEO_ENCODER_HEVC_CAP_PRE_ANALYSIS, amf::AMFVariant(preAnalysis));
}

void testTerminalOutputFailureDuringSwitch() {
    FakeComponent component;
    FakeContext context;
    FakeFactory factory;
    FakeTexture texture;
    AmfEncoder encoder;
    const auto settings = requestedSettings();
    component.outputResult = AMF_FAIL;
    AmfEncoderTestAccess::attach(encoder, &component, &context, &factory, settings);

    auto lease = std::make_shared<int>(7);
    std::weak_ptr<int> weakLease = lease;
    AmfEncoderTestAccess::retain(encoder, lease);
    lease.reset();

    GpuFrame frame;
    frame.texture = &texture;
    frame.width = settings.width;
    frame.height = settings.height;
    frame.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    EncodedPacket packet;
    expect(!encoder.encodeFrame(frame, packet), "the terminal output failure returned a packet");
    expect(component.queries == 1 && component.terminateCalls == 1,
        "the old component was not queried and terminated exactly once");
    expect(context.terminateCalls == 1 && factory.creates == 0,
        "the failed switch recreated a component or missed context teardown");
    expect(component.callsAfterTerminate == 0 && context.callsAfterTerminate == 0,
        "the failed switch called an invalidated component or context");
    expect(weakLease.expired(), "the failed switch retained a capture lease");
    expect(AmfEncoderTestAccess::faulted(encoder) && AmfEncoderTestAccess::resourcesReleased(encoder),
        "the failed switch did not leave a faulted, torn-down encoder");
    expect(!encoder.encoderAvailable() && !encoder.capabilities().available,
        "the failed switch still reported availability");
    expect(encoder.capabilities().detail.find(L"QueryOutput failed: AMF_FAIL") != std::wstring::npos,
        "the switch masked the original output-query failure");
}

void testLookaheadReportsOnlyConfiguredFeatures() {
    for (const auto codec : {VideoCodec::H264, VideoCodec::Hevc}) {
        for (const bool requested : {false, true}) {
            for (const bool reject : {false, true}) {
                FakeComponent component;
                FakeContext context;
                FakeFactory factory;
                AmfEncoder encoder;
                auto settings = requestedSettings(codec);
                settings.encoderLookahead = requested;
                settings.encoderBFrames = false;
                const wchar_t* preAnalysis = codec == VideoCodec::Hevc
                    ? AMF_VIDEO_ENCODER_HEVC_PRE_ANALYSIS_ENABLE : AMF_VIDEO_ENCODER_PRE_ANALYSIS_ENABLE;
                const wchar_t* preEncode = codec == VideoCodec::Hevc
                    ? AMF_VIDEO_ENCODER_HEVC_PREENCODE_ENABLE : AMF_VIDEO_ENCODER_PREENCODE_ENABLE;
                setCaps(component, true, true);
                component.seed(preAnalysis, amf::AMFVariant(false));
                if (reject) component.rejectWrites.insert(preAnalysis);
                AmfEncoderTestAccess::attach(encoder, &component, &context, &factory, settings);
                AmfEncoderTestAccess::updateCaps(encoder);
                expect(AmfEncoderTestAccess::configure(encoder), "an already-disabled pre-analysis setting failed");
                const auto caps = encoder.capabilities();
                expect(!caps.lookahead && !caps.effective.lookahead && caps.effective.lookaheadDepth == 0,
                    "unconfigured lookahead was reported as available or effective");
                expect(!caps.effective.adaptiveBFrames && !caps.effective.adaptiveIFrames,
                    "lookahead-dependent features were reported effective");
                expect(component.writesTo(preAnalysis) == 1 && component.writesTo(preEncode) == 0 &&
                       component.writesTo(L"PALookAheadBufferDepth") == 0,
                    "the AMF pre-analysis or pre-encode writes did not match the off policy");
                bool disabledBeforeInit = false;
                for (size_t i = 0; i < component.writesAtInit; ++i) {
                    const auto& [name, value] = component.writes[i];
                    if (name == preAnalysis && !value.ToBool()) disabledBeforeInit = true;
                }
                expect(disabledBeforeInit, "pre-analysis was not disabled before Init");
                expect(AmfEncoderTestAccess::delay(encoder) == 0 && AmfEncoderTestAccess::inFlightCap(encoder) == 16,
                    "the requested lookahead leaked into timing or the retention budget");
            }
        }
    }
}

void testEffectiveSettingsDoNotOverwriteRequests() {
    for (const auto codec : {VideoCodec::H264, VideoCodec::Hevc}) {
        for (const bool supported : {false, true}) {
            FakeComponent component;
            FakeContext context;
            FakeFactory factory;
            AmfEncoder encoder;
            auto settings = requestedSettings(codec);
            settings.encoderPreset = EncoderPreset::P7;
            settings.encoderSpatialAQ = true;
            settings.encoderAQStrength = 12;
            settings.encoderTemporalAQ = true;
            settings.encoderMultipass = EncoderMultipass::FullResolution;
            settings.encoderReferenceFrames = 4;
            auto requested = effectiveSettingsFor(settings);
            setCaps(component, supported, true);
            component.caps.seed(codec == VideoCodec::H264 ? AMF_VIDEO_ENCODER_CAP_MAX_REFERENCE_FRAMES
                : AMF_VIDEO_ENCODER_HEVC_CAP_MAX_REFERENCE_FRAMES,
                amf::AMFVariant(static_cast<amf_int64>(supported ? 4 : 1)));
            AmfEncoderTestAccess::attach(encoder, &component, &context, &factory, settings);
            AmfEncoderTestAccess::updateCaps(encoder);
            expect(AmfEncoderTestAccess::configure(encoder, requested), "effective-settings configuration failed");
            expect(requested.lookahead && requested.lookaheadDepth == 24 && requested.temporalAQ &&
                   requested.multipass == EncoderMultipass::FullResolution && requested.adaptiveBFrames &&
                   requested.adaptiveIFrames && requested.bFrames && !requested.zeroReorderDelay &&
                   requested.referenceFrames == 4,
                "publishing AMF effective settings overwrote the request");
            const auto effective = encoder.capabilities().effective;
            const bool bFrames = codec == VideoCodec::H264 && supported;
            expect(!effective.lookahead && effective.lookaheadDepth == 0 && !effective.temporalAQ &&
                   effective.multipass == EncoderMultipass::Disabled && !effective.adaptiveBFrames &&
                   !effective.adaptiveIFrames && effective.bFrames == bFrames &&
                   effective.zeroReorderDelay == !bFrames && effective.referenceFrames == (supported ? 4u : 1u),
                "AMF effective settings did not mask unsupported features or retain established timing");
            expect(effective.profile == requested.profile && effective.preset == requested.preset &&
                   effective.mode == requested.mode && effective.spatialAQ && effective.aqStrength == 12,
                "AMF publishing changed an unrelated effective setting");
            AmfEncoderTestAccess::updateDetail(encoder, requested);
            const std::wstring detail = std::wstring(L"AMF ") + (codec == VideoCodec::H264 ? L"AVC" : L"HEVC") +
                L" Direct3D 11 session active; lookahead is unsupported by the AMF backend" +
                (bFrames ? L"" : L"; B-frames are unavailable");
            const auto lines = Logger::instance().recentLines(1);
            expect(encoder.capabilities().detail == detail && !lines.empty() &&
                   lines.back().find(detail) != std::wstring::npos && lines.back().find(L"WARN") != std::wstring::npos,
                "the AMF warning was emitted before the complete ordered detail was assembled");
        }
    }
}

void testComponentCreationFailureDetailsAndCallerLogging() {
    for (const auto codec : {VideoCodec::H264, VideoCodec::Hevc}) {
        for (const auto result : {AMF_NOT_SUPPORTED, AMF_OK}) {
            FakeComponent oldComponent;
            FakeContext context;
            FakeFactory factory;
            AmfEncoder encoder;
            factory.createResult = result;
            AmfEncoderTestAccess::attach(encoder, nullptr, &context, &factory, requestedSettings(codec));
            const std::wstring detail = std::wstring(L"AMFFactory::CreateComponent(") +
                (codec == VideoCodec::H264 ? L"AVC" : L"HEVC") + L") failed: " +
                (result == AMF_OK ? L"AMF_OK" : L"AMF_NOT_SUPPORTED");
            Logger::instance().info(L"encoder-test", L"before component creation");
            const auto before = Logger::instance().recentLines(200);
            expect(!AmfEncoderTestAccess::createComponent(encoder) && encoder.capabilities().detail == detail,
                "component creation lost the codec-specific status detail or accepted a null component");
            expect(Logger::instance().recentLines(200) == before,
                "the component helper logged instead of leaving logging to its caller");
            AmfEncoderTestAccess::attach(encoder, &oldComponent, &context, &factory, requestedSettings(codec));
            expect(!AmfEncoderTestAccess::switchFormat(encoder), "the failed component replacement succeeded");
            const auto lines = Logger::instance().recentLines(1);
            expect(factory.creates == 2 && factory.lastComponentId ==
                   (codec == VideoCodec::H264 ? AMFVideoEncoderVCE_AVC : AMFVideoEncoder_HEVC) &&
                   encoder.capabilities().detail == detail && !lines.empty() &&
                   lines.back().find(detail) != std::wstring::npos && lines.back().find(L"ERROR") != std::wstring::npos,
                "the format-switch caller changed component selection, failure detail, or error logging");
        }
    }
}

void testPreAnalysisRejectionAndReadback() {
    for (const auto codec : {VideoCodec::H264, VideoCodec::Hevc}) {
        const wchar_t* name = codec == VideoCodec::Hevc
            ? AMF_VIDEO_ENCODER_HEVC_PRE_ANALYSIS_ENABLE : AMF_VIDEO_ENCODER_PRE_ANALYSIS_ENABLE;
        for (int scenario = 0; scenario < 3; ++scenario) {
            const bool rejected = scenario == 1;
            FakeComponent component;
            FakeContext context;
            FakeFactory factory;
            AmfEncoder encoder;
            component.preAnalysisName = name;
            component.overridePreAnalysis = scenario == 0;
            component.overridePreAnalysisAfterRateControl = scenario == 2;
            if (rejected) {
                component.rejectWrites.insert(name);
                component.seed(name, amf::AMFVariant(true));
            }
            setCaps(component, true, true);
            AmfEncoderTestAccess::attach(encoder, &component, &context, &factory, requestedSettings(codec));
            AmfEncoderTestAccess::updateCaps(encoder);
            expect(!AmfEncoderTestAccess::configure(encoder), "active pre-analysis passed as disabled");
            expect(encoder.capabilities().detail.find(L"pre-analysis") != std::wstring::npos,
                "an unverified pre-analysis failure lacked a reason");
        }
        {
            FakeComponent component;
            FakeContext context;
            FakeFactory factory;
            AmfEncoder encoder;
            setCaps(component, false, true);
            component.rejectWrites.insert(name);
            AmfEncoderTestAccess::attach(encoder, &component, &context, &factory, requestedSettings(codec));
            AmfEncoderTestAccess::updateCaps(encoder);
            expect(!AmfEncoderTestAccess::configure(encoder),
                "a supported but unreadable, rejected pre-analysis disable was accepted");
        }
        {
            FakeComponent component;
            FakeContext context;
            FakeFactory factory;
            AmfEncoder encoder;
            setCaps(component, false, true);
            component.caps.properties.erase(codec == VideoCodec::Hevc
                ? AMF_VIDEO_ENCODER_HEVC_CAP_PRE_ANALYSIS : AMF_VIDEO_ENCODER_CAP_PRE_ANALYSIS);
            component.rejectWrites.insert(name);
            AmfEncoderTestAccess::attach(encoder, &component, &context, &factory, requestedSettings(codec));
            AmfEncoderTestAccess::updateCaps(encoder);
            expect(!AmfEncoderTestAccess::configure(encoder),
                "an unknown pre-analysis capability was treated as unsupported");
        }
        // Unsupported hardware may reject and omit the property entirely.
        FakeComponent component;
        FakeContext context;
        FakeFactory factory;
        AmfEncoder encoder;
        setCaps(component, false, false);
        component.rejectWrites.insert(name);
        AmfEncoderTestAccess::attach(encoder, &component, &context, &factory, requestedSettings(codec));
        AmfEncoderTestAccess::updateCaps(encoder);
        expect(AmfEncoderTestAccess::configure(encoder), "unsupported pre-analysis prevented encoding");
        expect(!encoder.capabilities().effective.lookahead, "unsupported pre-analysis was reported active");
    }
}

void testBFramePatternControlsEffectiveTiming() {
    enum class PatternCase { Accepted, Rejected, ChangedOnInit, ChangedAfterRate, Unreadable, Unverifiable, UnknownCapability };
    for (const auto test : {PatternCase::Accepted, PatternCase::Rejected, PatternCase::ChangedOnInit,
                            PatternCase::ChangedAfterRate, PatternCase::Unreadable, PatternCase::Unverifiable,
                            PatternCase::UnknownCapability}) {
        FakeComponent component;
        FakeContext context;
        FakeFactory factory;
        AmfEncoder encoder;
        setCaps(component, true, true);
        component.seed(AMF_VIDEO_ENCODER_B_PIC_PATTERN, amf::AMFVariant(static_cast<amf_int64>(0)));
        if (test == PatternCase::UnknownCapability) {
            component.caps.properties.erase(AMF_VIDEO_ENCODER_CAP_BFRAMES);
        }
        if (test == PatternCase::Rejected || test == PatternCase::Unverifiable || test == PatternCase::UnknownCapability) {
            component.rejectWrites.insert(AMF_VIDEO_ENCODER_B_PIC_PATTERN);
        }
        if (test == PatternCase::ChangedOnInit || test == PatternCase::ChangedAfterRate) {
            component.overridePattern = test == PatternCase::ChangedOnInit;
            component.overridePatternAfterRateControl = test == PatternCase::ChangedAfterRate;
            component.patternAfterInit = 1;
        }
        if (test == PatternCase::Unreadable || test == PatternCase::Unverifiable || test == PatternCase::UnknownCapability) {
            component.unreadable.insert(AMF_VIDEO_ENCODER_B_PIC_PATTERN);
        }
        AmfEncoderTestAccess::attach(encoder, &component, &context, &factory, requestedSettings());
        AmfEncoderTestAccess::updateCaps(encoder);
        const bool configured = AmfEncoderTestAccess::configure(encoder);
        if (test == PatternCase::Unverifiable || test == PatternCase::UnknownCapability) {
            expect(!configured, "an unverifiable B-frame pattern was accepted");
            continue;
        }
        expect(configured, "the AVC configuration failed");
        const uint32_t expected = test == PatternCase::Accepted ? 2 :
            (test == PatternCase::ChangedOnInit || test == PatternCase::ChangedAfterRate) ? 1 : 0;
        const auto effective = encoder.capabilities().effective;
        expect(effective.bFrames == (expected != 0) && effective.zeroReorderDelay == (expected == 0),
            "reported B-frame behavior disagreed with the established pattern");
        expect(AmfEncoderTestAccess::delay(encoder) == expected,
            "timestamp delay disagreed with the established B-frame pattern");
        if (test == PatternCase::Unreadable) {
            expect(component.writesTo(AMF_VIDEO_ENCODER_B_PIC_PATTERN) == 2,
                "unreadable B-frame pattern did not fall back to an explicit disable");
            const auto& writes = component.writes;
            const auto fallback = std::find_if(writes.rbegin(), writes.rend(), [](const auto& write) {
                return write.first == AMF_VIDEO_ENCODER_B_PIC_PATTERN;
            });
            expect(fallback != writes.rend() && fallback->second.ToInt64() == 0,
                "the B-frame fallback did not write zero");
        }
    }
}

void testSuccessfulSwitchResetsTiming() {
    for (const auto codec : {VideoCodec::H264, VideoCodec::Hevc}) {
        FakeComponent oldComponent;
        FakeComponent replacement;
        FakeContext context;
        FakeFactory factory;
        AmfEncoder encoder;
        setCaps(replacement, true, true);
        factory.next = &replacement;
        AmfEncoderTestAccess::attach(encoder, &oldComponent, &context, &factory, requestedSettings(codec));
        AmfEncoderTestAccess::setPreviousBFrames(encoder, codec == VideoCodec::H264);
        AmfEncoderTestAccess::seedTimestamp(encoder);
        expect(AmfEncoderTestAccess::switchFormat(encoder), "the normal format switch failed");
        const uint32_t delay = codec == VideoCodec::H264 ? 2 : 0;
        expect(oldComponent.queries == 1 && oldComponent.terminateCalls == 1 && factory.creates == 1,
            "the normal switch did not replace its component");
        expect(AmfEncoderTestAccess::delay(encoder) == delay &&
               AmfEncoderTestAccess::firstDts(encoder) == -static_cast<int64_t>(delay) * 100000,
            "the recreated component retained stale timestamps or the requested lookahead depth");
        const auto caps = encoder.capabilities();
        expect(!caps.effective.lookahead && caps.effective.lookaheadDepth == 0 &&
               caps.effective.bFrames == (delay != 0),
            "the recreated component reported settings that do not match its configuration");
        expect(caps.detail.find(L"lookahead is unsupported") != std::wstring::npos &&
               (caps.detail.find(L"B-frames are unavailable") != std::wstring::npos) == (delay == 0),
            "the recreated component retained stale capability details");
    }
    FakeComponent oldComponent;
    FakeComponent replacement;
    FakeContext context;
    FakeFactory factory;
    AmfEncoder encoder;
    setCaps(replacement, true, true);
    replacement.seed(AMF_VIDEO_ENCODER_B_PIC_PATTERN, amf::AMFVariant(static_cast<amf_int64>(0)));
    replacement.rejectWrites.insert(AMF_VIDEO_ENCODER_B_PIC_PATTERN);
    factory.next = &replacement;
    AmfEncoderTestAccess::attach(encoder, &oldComponent, &context, &factory, requestedSettings());
    AmfEncoderTestAccess::setPreviousBFrames(encoder, true);
    expect(AmfEncoderTestAccess::switchFormat(encoder), "the degraded AVC switch failed");
    expect(!encoder.capabilities().effective.bFrames && AmfEncoderTestAccess::firstDts(encoder) == 0 &&
           encoder.capabilities().detail.find(L"B-frames are unavailable") != std::wstring::npos,
        "the degraded AVC switch retained B-frame timing or stale details");
}

void testSwitchEnablingReorderingFaultsForPipelineRecreation() {
    FakeComponent oldComponent;
    FakeComponent replacement;
    FakeContext context;
    FakeFactory factory;
    FakeTexture texture;
    AmfEncoder encoder;
    const auto settings = requestedSettings();
    setCaps(replacement, true, true);
    factory.next = &replacement;
    AmfEncoderTestAccess::attach(encoder, &oldComponent, &context, &factory, settings);
    AmfEncoderTestAccess::setPreviousBFrames(encoder, false);
    GpuFrame frame;
    frame.texture = &texture;
    frame.width = settings.width;
    frame.height = settings.height;
    frame.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    EncodedPacket packet;
    expect(!encoder.encodeFrame(frame, packet) && !encoder.encoderAvailable(),
        "a format switch enabled B-frames without requesting pipeline recreation");
    expect(factory.creates == 1 && oldComponent.terminateCalls == 1 && replacement.terminateCalls == 1 &&
           context.terminateCalls == 1 && AmfEncoderTestAccess::resourcesReleased(encoder),
        "the unsafe format switch did not tear down both components and the context");
    expect(encoder.capabilities().detail.find(L"enabled B-frames") != std::wstring::npos,
        "the unsafe format switch did not report why it faulted");
}

} // namespace

int main() {
    try {
        testTerminalOutputFailureDuringSwitch();
        testLookaheadReportsOnlyConfiguredFeatures();
        testEffectiveSettingsDoNotOverwriteRequests();
        testComponentCreationFailureDetailsAndCallerLogging();
        testPreAnalysisRejectionAndReadback();
        testBFramePatternControlsEffectiveTiming();
        testSuccessfulSwitchResetsTiming();
        testSwitchEnablingReorderingFaultsForPipelineRecreation();
        std::cout << "AmfEncoder tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AmfEncoder test failure: " << error.what() << '\n';
        return 1;
    }
}
