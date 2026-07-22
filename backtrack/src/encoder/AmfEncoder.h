#pragma once

#include "encoder/IEncoder.h"

#include <memory>

namespace backtrack {

// AMD Advanced Media Framework (AMF) hardware encoder. Encodes D3D11 NV12
// textures directly on the GPU through amfrt64.dll, mirroring the zero-copy
// design of NvencEncoder. Builds as a stub when the AMF headers are absent.
class AmfEncoder final : public IEncoder {
public:
    AmfEncoder();
    ~AmfEncoder() override;

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
