#if !defined(BACKTRACK_HAS_NVENC) || BACKTRACK_HAS_NVENC != 0
#error NvencStubTests requires BACKTRACK_HAS_NVENC=0
#endif

#include "capture/D3DDevice.h"
#include "encoder/NvencEncoder.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testUnavailableStub() {
    backtrack::D3DDevice device;
    backtrack::NvencEncoder encoder;
    backtrack::VideoSettings settings;

    expect(!encoder.encoderAvailable(), "the stub started available");
    expect(!encoder.initialize(device, settings), "the stub initialized successfully");
    const auto caps = encoder.capabilities();
    expect(!caps.available && caps.backendName == L"NVENC", "the stub reported an available backend");
    expect(caps.detail.find(L"nvEncodeAPI.h") != std::wstring::npos, "the stub did not explain why NVENC is unavailable");
    expect(!encoder.encoderAvailable() && !encoder.stats().encoderAvailable,
        "the availability probes disagree with the stub");

    backtrack::GpuFrame frame;
    backtrack::EncodedPacket packet;
    packet.bytes.push_back(1);
    expect(!encoder.encodeFrame(frame, packet) && packet.bytes.empty(), "the stub emitted a packet");
    expect(encoder.stats().droppedFrames == 1, "the stub did not count the dropped frame");

    std::vector<backtrack::EncodedPacket> packets(1);
    encoder.encodeFrame(frame, packets);
    expect(packets.empty(), "the stub did not clear the output vector");
    packets.emplace_back();
    encoder.drain(packets);
    expect(packets.empty(), "the stub drained an output");
    encoder.requestKeyFrame();
    expect(encoder.resetInputResources(), "the stub refused an input reset");
    encoder.shutdown();
    expect(!encoder.encoderAvailable() && !encoder.stats().encoderAvailable, "the stub became available on shutdown");

    expect(!encoder.initialize(device, settings), "the stub initialized on a second attempt");
    expect(encoder.stats().droppedFrames == 0, "reinitialization did not reset stub stats");
}

} // namespace

int main() {
    try {
        testUnavailableStub();
    } catch (const std::exception& error) {
        std::cerr << "NvencStubTests: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
