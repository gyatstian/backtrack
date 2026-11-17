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
    // False means a cached registration, mapping, or input lease could not be
    // retired. In-flight inputs keep their storage and pool lease until the
    // driver confirms completion. Pure virtual: a backend that pins inputs must
    // say so, and a silent `return true` reads as safe retirement it never did.
    virtual bool resetInputResources() = 0;
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

    // Maximum number of pool-backed input surfaces this encoder can hold
    // (retain) before it must produce output. The encoder-feeding pool is
    // sized from this, so a deep-lookahead session cannot exhaust it and drop
    // frames. Both backends keep an accepted input's texture lease until the
    // output it belongs to is harvested: NVENC through its per-use mapping
    // bindings (bounded by its bitstream buffers, which already account for
    // lookahead and B-frame delay), AMF through its in-flight input backlog
    // (bounded by its own input cap), even though AMF copies the pixels.
    // 0 is the default and means the backend makes no claim: it retains
    // nothing, or has no live session yet. It is not a general sentinel - a
    // backend that retains inputs reports its real count whether or not a
    // session exists.
    virtual uint32_t inputRetentionBudget() const { return 0; }

    // Inputs the encoder accepted and then abandoned before producing an output
    // (a dropped or rejected submission). Lock-free: the recording-stop drain
    // subtracts these from the submissions it is still waiting for, so a drop
    // cannot stall a recording cut. 0 for a backend that never drops silently.
    [[nodiscard]] virtual uint64_t droppedInputs() const noexcept { return 0; }
};

} // namespace backtrack
