#pragma once

#include "core/Types.h"

#include <vector>

namespace backtrack {

class D3DDevice;

class IEncoder {
public:
    virtual ~IEncoder() = default;

    virtual bool initialize(D3DDevice& device, const VideoSettings& settings) = 0;
    virtual void requestKeyFrame() = 0;
    virtual void resetInputResources() {}
    // Single-packet form: true means `packet` holds one output, false means no
    // output was ready (never an error signal on its own).
    virtual bool encodeFrame(const GpuFrame& frame, EncodedPacket& packet) = 0;
    // Drain-to-vector form: appends every packet ready after this frame. A
    // submission can complete several outputs while reordering/lookahead
    // drains, so callers that only take the first strand the tail. Clears
    // `out` first, matching drain(). The default body pops at most one through
    // the single-packet form; backends override it to drain fully.
    virtual void encodeFrame(const GpuFrame& frame, std::vector<EncodedPacket>& out) {
        out.clear();
        EncodedPacket packet;
        if (encodeFrame(frame, packet)) {
            out.push_back(std::move(packet));
        }
    }
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
