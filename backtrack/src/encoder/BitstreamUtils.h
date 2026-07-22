#pragma once

#include "core/Types.h"

#include <cstdint>
#include <vector>

namespace backtrack {

// Scans an encoded video bitstream (Annex B start codes or length-prefixed
// NAL units) for an IDR/keyframe NAL unit. Shared by all hardware encoders.
bool bitstreamContainsKeyFrame(VideoCodec codec, const std::vector<uint8_t>& bytes);

} // namespace backtrack
