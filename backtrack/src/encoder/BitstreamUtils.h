#pragma once

#include "core/Types.h"

#include <cstdint>
#include <vector>

namespace backtrack {

// Scans an encoded video bitstream (Annex B start codes or length-prefixed
// NAL units). Shared by all hardware encoders.

// True when the bitstream contains an IDR (or HEVC BLA/IDR) picture. Only
// such frames are safe replay cut points: a plain I or CRA frame may leave
// the decoder referencing pictures from before the cut, which corrupts
// playback and seeking for clips that start there.
bool bitstreamContainsKeyFrame(VideoCodec codec, const std::vector<uint8_t>& bytes);

// True when the bitstream contains a CRA (open-GOP) picture. NVENC reports
// those as plain I-frames in its picture-type metadata; they must not be
// marked as replay cut points.
bool bitstreamContainsCraFrame(VideoCodec codec, const std::vector<uint8_t>& bytes);

// Collects the codec parameter-set NAL units (H264: SPS + PPS; HEVC:
// VPS + SPS + PPS) in stream order, as Annex B. Used to guarantee that a
// clip cut at an IDR always starts with decodable headers even when a
// driver drops its header-insertion setting.
bool extractParameterSets(
    VideoCodec codec,
    const std::vector<uint8_t>& bytes,
    std::vector<uint8_t>& out);

// True when every parameter set required to decode the stream is present.
bool bitstreamContainsParameterSets(VideoCodec codec, const std::vector<uint8_t>& bytes);

} // namespace backtrack
