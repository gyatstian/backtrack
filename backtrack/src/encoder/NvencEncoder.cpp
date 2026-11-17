#include "encoder/NvencEncoder.h"

#include "core/Logger.h"
#include "encoder/NvencApiTier.h"
#include "encoder/NvencSdkHeader.h"

#if BACKTRACK_HAS_NVENC
#include "encoder/NvencTierFactories.h"

#include <Windows.h>
#else
#include "capture/D3DDevice.h"
#endif

#include <string>

namespace backtrack {

#if BACKTRACK_HAS_NVENC

namespace {

// 0 means unknown (no DLL, or no export), which selects the oldest tier so that
// tier's own load/open-session detail is what gets reported.
uint32_t driverNvencApiVersion() {
    HMODULE module = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        return 0;
    }
    // Declared by every SDK header as NVENCSTATUS NVENCAPI
    // NvEncodeAPIGetMaxSupportedVersion(uint32_t*). The DLL is loaded, never
    // linked, so the entry point comes from GetProcAddress.
    auto getMaxVersion = reinterpret_cast<int(WINAPI*)(uint32_t*)>(
        GetProcAddress(module, "NvEncodeAPIGetMaxSupportedVersion"));
    uint32_t version = 0;
    if (getMaxVersion) {
        getMaxVersion(&version);
    }
    FreeLibrary(module);
    return version;
}

std::wstring describeApiVersion(uint32_t version) {
    return std::to_wstring(version >> 4) + L"." + std::to_wstring(version & 0xF);
}

} // namespace

NvencEncoder::NvencEncoder() {
    const uint32_t driverApi = driverNvencApiVersion();
    const NvencApiTier tier = selectNvencApiTier(driverApi);
    Logger::instance().info(
        L"encoder",
        L"NVENC API: driver reports " + describeApiVersion(driverApi) +
            (driverApi ? std::wstring() : std::wstring(L" (unreadable)")) + L"; using " +
            (tier == NvencApiTier::Api13 ? L"13.0" : L"11.1") + L" headers");
    inner_ = tier == NvencApiTier::Api13 ? nvenc_tier::createApi13() : nvenc_tier::createApi11();
}

#else

namespace {

// A build without the SDK keeps the backend present and reports why it cannot
// record, so adapter selection and the Diagnostics page still name a backend.
class UnavailableEncoder final : public IEncoder {
public:
    bool initialize(D3DDevice& device, const VideoSettings& settings) override {
        (void)settings;
        caps_ = {};
        stats_ = {};
        caps_.backendName = L"NVENC";
        caps_.adapterName = device.adapterName();
        caps_.available = false;
        caps_.detail = L"Built without nvEncodeAPI.h. Install NVIDIA Video Codec SDK and rebuild to enable NVENC.";
        Logger::instance().error(L"encoder", caps_.detail);
        return false;
    }

    void requestKeyFrame() override {}
    bool resetInputResources() override { return true; }

    bool encodeFrame(const GpuFrame& frame, EncodedPacket& packet) override {
        (void)frame;
        packet = {};
        ++stats_.droppedFrames;
        return false;
    }

    void encodeFrame(const GpuFrame& frame, std::vector<EncodedPacket>& packets) override {
        (void)frame;
        packets.clear();
    }

    void drain(std::vector<EncodedPacket>& packets) override {
        packets.clear();
    }

    void shutdown() override {
        caps_.available = false;
        stats_.encoderAvailable = false;
    }

    EncoderCapabilities capabilities() const override { return caps_; }
    EncoderStats stats() const override { return stats_; }
    bool encoderAvailable() const noexcept override { return false; }
    DXGI_FORMAT preferredInputFormat() const override { return DXGI_FORMAT_B8G8R8A8_UNORM; }
    uint32_t inputRetentionBudget() const override { return 0; }

private:
    EncoderCapabilities caps_;
    EncoderStats stats_;
};

} // namespace

NvencEncoder::NvencEncoder()
    : inner_(std::make_unique<UnavailableEncoder>()) {
}

#endif

NvencEncoder::~NvencEncoder() {
    shutdown();
}

bool NvencEncoder::initialize(D3DDevice& device, const VideoSettings& settings) {
    return inner_->initialize(device, settings);
}

void NvencEncoder::requestKeyFrame() {
    inner_->requestKeyFrame();
}

bool NvencEncoder::resetInputResources() {
    return inner_->resetInputResources();
}

bool NvencEncoder::encodeFrame(const GpuFrame& frame, EncodedPacket& packet) {
    return inner_->encodeFrame(frame, packet);
}

void NvencEncoder::encodeFrame(const GpuFrame& frame, std::vector<EncodedPacket>& packets) {
    inner_->encodeFrame(frame, packets);
}

void NvencEncoder::drain(std::vector<EncodedPacket>& packets) {
    inner_->drain(packets);
}

void NvencEncoder::shutdown() {
    inner_->shutdown();
}

EncoderCapabilities NvencEncoder::capabilities() const {
    return inner_->capabilities();
}

EncoderStats NvencEncoder::stats() const {
    return inner_->stats();
}

bool NvencEncoder::encoderAvailable() const noexcept {
    return inner_->encoderAvailable();
}

DXGI_FORMAT NvencEncoder::preferredInputFormat() const {
    return inner_->preferredInputFormat();
}

uint32_t NvencEncoder::inputRetentionBudget() const {
    return inner_->inputRetentionBudget();
}

uint64_t NvencEncoder::droppedInputs() const noexcept {
    return inner_->droppedInputs();
}

} // namespace backtrack
