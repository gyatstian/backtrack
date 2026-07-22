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
    void resetInputResources() override;
    bool encodeFrame(const GpuFrame& frame, EncodedPacket& packet) override;
    void drain(std::vector<EncodedPacket>& packets) override;
    void shutdown() override;
    EncoderCapabilities capabilities() const override;
    EncoderStats stats() const override;
    DXGI_FORMAT preferredInputFormat() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace backtrack
