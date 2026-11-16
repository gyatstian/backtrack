#pragma once

#include "encoder/IEncoder.h"

#include <memory>

namespace backtrack::nvenc_tier {

// One factory per compiled API tier. The definition lives in that tier's
// translation unit because only it may include the tier's SDK header.
std::unique_ptr<IEncoder> createApi13();
std::unique_ptr<IEncoder> createApi11();

} // namespace backtrack::nvenc_tier
