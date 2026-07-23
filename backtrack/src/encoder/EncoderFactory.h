#pragma once

#include "encoder/IEncoder.h"

#include <memory>

namespace backtrack {

class D3DDevice;

// Selects and constructs the hardware video encoder that matches the active
// GPU. AMD adapters (DXGI VendorId 0x1002) use AMF; NVIDIA adapters use NVENC.
// Other/unknown adapters default to NVENC.
std::unique_ptr<IEncoder> createEncoderForDevice(const D3DDevice& device);

} // namespace backtrack
