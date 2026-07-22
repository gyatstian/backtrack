#include "encoder/BitstreamUtils.h"

namespace backtrack {

namespace {

size_t annexBStartCodeSize(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset + 3 > bytes.size() || bytes[offset] != 0 || bytes[offset + 1] != 0) {
        return 0;
    }
    if (bytes[offset + 2] == 1) {
        return 3;
    }
    if (offset + 4 <= bytes.size() && bytes[offset + 2] == 0 && bytes[offset + 3] == 1) {
        return 4;
    }
    return 0;
}

bool nalUnitIsKeyFrame(VideoCodec codec, const std::vector<uint8_t>& bytes, size_t nalOffset) {
    if (nalOffset >= bytes.size()) {
        return false;
    }
    if (codec == VideoCodec::H264) {
        return (bytes[nalOffset] & 0x1f) == 5;
    }
    if (nalOffset + 1 >= bytes.size()) {
        return false;
    }
    const uint8_t nalType = (bytes[nalOffset] >> 1) & 0x3f;
    return nalType >= 16 && nalType <= 21;
}

bool annexBBitstreamContainsKeyFrame(VideoCodec codec, const std::vector<uint8_t>& bytes) {
    for (size_t offset = 0; offset + 3 <= bytes.size(); ++offset) {
        const size_t startCodeSize = annexBStartCodeSize(bytes, offset);
        if (startCodeSize == 0) {
            continue;
        }

        const size_t nalOffset = offset + startCodeSize;
        if (nalUnitIsKeyFrame(codec, bytes, nalOffset)) {
            return true;
        }

        offset = nalOffset;
    }
    return false;
}

bool lengthPrefixedBitstreamContainsKeyFrame(VideoCodec codec, const std::vector<uint8_t>& bytes) {
    size_t offset = 0;
    while (offset + 4 <= bytes.size()) {
        const uint32_t nalSize =
            (static_cast<uint32_t>(bytes[offset]) << 24) |
            (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
            (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
            static_cast<uint32_t>(bytes[offset + 3]);
        const size_t nalOffset = offset + 4;
        if (nalSize == 0 || nalSize > bytes.size() - nalOffset) {
            return false;
        }
        if (nalUnitIsKeyFrame(codec, bytes, nalOffset)) {
            return true;
        }
        offset = nalOffset + nalSize;
    }
    return false;
}

} // namespace

bool bitstreamContainsKeyFrame(VideoCodec codec, const std::vector<uint8_t>& bytes) {
    return annexBBitstreamContainsKeyFrame(codec, bytes) ||
           lengthPrefixedBitstreamContainsKeyFrame(codec, bytes);
}

} // namespace backtrack
