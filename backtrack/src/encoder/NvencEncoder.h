#pragma once

#include "encoder/IEncoder.h"

#include <memory>

namespace backtrack {

// NVENC entry point. The driver only accepts a client built with an NVENC API
// version at or below the one it implements, and that version is compiled into
// every NVENC struct, so the tier cannot be switched at run time. This picks the
// compiled tier once from the driver's reported version and delegates to it.
class NvencEncoder final : public IEncoder {
public:
    NvencEncoder();
    ~NvencEncoder() override;

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
    uint64_t droppedInputs() const noexcept override;

private:
    std::unique_ptr<IEncoder> inner_;
};

} // namespace backtrack
