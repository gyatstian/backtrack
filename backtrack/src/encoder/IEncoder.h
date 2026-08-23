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

    // Lock-free availability probe mirroring stats().encoderAvailable. The
    // capture loop polls this every iteration; reading it through stats()
    // would lock the encoder mutex (contended while a frame is being encoded)
    // and copy the whole struct just to reach one bool.
    [[nodiscard]] virtual bool encoderAvailable() const noexcept = 0;

    // Preferred D3D11 input surface format for this encoder. The capture scaler
    // converts frames to this format before submission (NV12 via the fixed-
    // function video processor when the hardware encoder wants YUV, BGRA when
    // it takes RGB directly).
    virtual DXGI_FORMAT preferredInputFormat() const = 0;
};

} // namespace backtrack
