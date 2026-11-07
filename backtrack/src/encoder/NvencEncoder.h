#pragma once

#include "encoder/IEncoder.h"

#include <memory>

namespace backtrack {

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

private:
    friend struct NvencEncoderTestAccess;
    struct Impl;
    void abandonImpl();
    std::unique_ptr<Impl> impl_;
};

} // namespace backtrack
