#include "encoder/NvencSdkHeader.h"

#if BACKTRACK_HAS_NVENC

#include "encoder/NvencTierFactories.h"

#include "Interface/nvEncodeAPI.h"

#define BACKTRACK_NVENC_NAMESPACE nvenc13
#include "encoder/NvencEncoderBody.inl"

namespace backtrack::nvenc_tier {

std::unique_ptr<IEncoder> createApi13() {
    return std::make_unique<nvenc13::Encoder>();
}

} // namespace backtrack::nvenc_tier

#endif
