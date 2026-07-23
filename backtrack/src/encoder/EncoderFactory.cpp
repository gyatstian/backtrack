#include "encoder/EncoderFactory.h"

#include "capture/D3DDevice.h"
#include "core/Logger.h"
#include "encoder/AmfEncoder.h"
#include "encoder/NvencEncoder.h"

namespace backtrack {

std::unique_ptr<IEncoder> createEncoderForDevice(const D3DDevice& device) {
    switch (device.vendor()) {
    case GpuVendor::Amd:
        Logger::instance().info(L"encoder", L"GPU vendor is AMD; selecting AMF hardware encoder");
        return std::make_unique<AmfEncoder>();
    case GpuVendor::Nvidia:
        Logger::instance().info(L"encoder", L"GPU vendor is NVIDIA; selecting NVENC hardware encoder");
        return std::make_unique<NvencEncoder>();
    case GpuVendor::Intel:
    case GpuVendor::Unknown:
    default:
        // No dedicated Intel path yet. NVENC is the historical default; if its
        // runtime DLL is absent, initialization fails cleanly and the Encoder
        // Stats page reports why. AMD systems are covered by the Amd case above.
        Logger::instance().info(L"encoder",
            L"GPU vendor is not AMD/NVIDIA; defaulting to NVENC hardware encoder");
        return std::make_unique<NvencEncoder>();
    }
}

} // namespace backtrack
