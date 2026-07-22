#pragma once

#include "encoder/IEncoder.h"

#include <memory>

namespace backtrack {

class D3DDevice;

// Selects and constructs the hardware video encoder that matches the active
// GPU. AMD adapters use AMF; NVIDIA adapters use NVENC. On other/unknown
// adapters the factory falls back to trying NVENC then AMF so a working
// encoder is still found on hybrid or virtualized systems.
std::unique_ptr<IEncoder> createEncoderForDevice(const D3DDevice& device);

} // namespace backtrack
