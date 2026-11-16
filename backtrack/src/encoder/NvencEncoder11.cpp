#include "encoder/NvencSdkHeader.h"

#if BACKTRACK_HAS_NVENC

#include "encoder/NvencTierFactories.h"

// Same SDK, one major version older. Vendored beside the current header so a
// driver that predates API 13 still gets a session; see NvencApiTier.h.
#include "Interface/nvEncodeAPI_11_1.h"

#define BACKTRACK_NVENC_NAMESPACE nvenc11
#include "encoder/NvencEncoderBody.inl"

namespace backtrack::nvenc_tier {

std::unique_ptr<IEncoder> createApi11() {
    return std::make_unique<nvenc11::Encoder>();
}

} // namespace backtrack::nvenc_tier

#endif
