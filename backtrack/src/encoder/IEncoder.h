#pragma once

#include "capture/D3DDevice.h"
#include "core/Types.h"

#include <vector>

namespace backtrack {

class IEncoder {
public:
    virtual ~IEncoder() = default;

    virtual bool initialize(D3DDevice& device, const VideoSettings& settings) = 0;
    virtual void requestKeyFrame() = 0;
    virtual void resetInputResources() {}
    virtual bool encodeFrame(const GpuFrame& frame, EncodedPacket& packet) = 0;
    virtual void drain(std::vector<EncodedPacket>& packets) = 0;
    virtual void shutdown() = 0;
    virtual EncoderCapabilities capabilities() const = 0;
    virtual EncoderStats stats() const = 0;

    // Preferred D3D11 input surface format for this encoder. The capture scaler
    // converts frames to this format before submission. NVENC accepts BGRA
    // directly; AMF AVC/HEVC prefer NV12.
    virtual DXGI_FORMAT preferredInputFormat() const = 0;
};

} // namespace backtrack
