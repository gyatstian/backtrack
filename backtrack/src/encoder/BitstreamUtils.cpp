#include "encoder/BitstreamUtils.h"

namespace backtrack {

namespace {

// H264 NAL unit types (ITU-T H.264 Table 7-1).
constexpr uint8_t kH264NalTypeMask = 0x1f;
constexpr uint8_t kH264NalCodedSliceNonIdr = 1;
constexpr uint8_t kH264NalIdrSlice = 5;
constexpr uint8_t kH264NalSps = 7;
constexpr uint8_t kH264NalPps = 8;

// HEVC NAL unit types (ITU-T H.265 Table 7-1). Types 16..20 are BLA/IDR
// (closed GOP, decodable from nothing); 21 is CRA (open GOP: its associated
// pictures may reference frames before it, so it is not a safe cut point).
constexpr uint8_t kHevcFirstBla = 16;
constexpr uint8_t kHevcLastIdr = 20;
constexpr uint8_t kHevcCra = 21;
constexpr uint8_t kHevcVps = 32;
constexpr uint8_t kHevcSps = 33;
constexpr uint8_t kHevcPps = 34;

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

uint8_t nalUnitType(VideoCodec codec, const uint8_t* header) {
    return codec == VideoCodec::H264 ? (*header & kH264NalTypeMask) : ((header[0] >> 1) & 0x3f);
}

uint64_t parameterSetMask(VideoCodec codec) {
    return codec == VideoCodec::H264
        ? (uint64_t{1} << kH264NalSps) | (uint64_t{1} << kH264NalPps)
        : (uint64_t{1} << kHevcVps) | (uint64_t{1} << kHevcSps) | (uint64_t{1} << kHevcPps);
}

bool nalUnitIsKeyFrame(VideoCodec codec, const uint8_t* header) {
    const uint8_t type = nalUnitType(codec, header);
    return codec == VideoCodec::H264 ? type == kH264NalIdrSlice
        : type >= kHevcFirstBla && type <= kHevcLastIdr;
}

bool nalUnitIsCra(VideoCodec codec, const uint8_t* header) {
    return codec == VideoCodec::Hevc && nalUnitType(codec, header) == kHevcCra;
}

bool nalUnitIsParameterSet(VideoCodec codec, const uint8_t* header) {
    return (parameterSetMask(codec) & (uint64_t{1} << nalUnitType(codec, header))) != 0;
}

// Walks every NAL unit and invokes the predicate with a pointer to the NAL
// header byte(s). Both hardware backends emit Annex B start-code framing and
// no other caller exists, so that is the only framing; stops at the first
// predicate hit.
template <typename Predicate>
bool scanNalUnits(VideoCodec codec, const std::vector<uint8_t>& bytes, Predicate&& predicate) {
    for (size_t offset = 0; offset + 3 <= bytes.size(); ++offset) {
        const size_t startCodeSize = annexBStartCodeSize(bytes, offset);
        if (startCodeSize == 0) {
            continue;
        }
        const size_t nalOffset = offset + startCodeSize;
        if (nalOffset < bytes.size() && predicate(bytes.data() + nalOffset)) {
            return true;
        }
        offset = nalOffset;
    }
    return false;
}

} // namespace

bool bitstreamContainsKeyFrame(VideoCodec codec, const std::vector<uint8_t>& bytes) {
    return scanNalUnits(codec, bytes, [codec](const uint8_t* header) {
        return nalUnitIsKeyFrame(codec, header);
    });
}

bool bitstreamContainsCraFrame(VideoCodec codec, const std::vector<uint8_t>& bytes) {
    return scanNalUnits(codec, bytes, [codec](const uint8_t* header) {
        return nalUnitIsCra(codec, header);
    });
}

bool bitstreamContainsParameterSets(VideoCodec codec, const std::vector<uint8_t>& bytes) {
    const uint64_t required = parameterSetMask(codec);
    uint64_t seen = 0;
    return scanNalUnits(codec, bytes, [&](const uint8_t* header) {
        seen |= uint64_t{1} << nalUnitType(codec, header);
        return (seen & required) == required;
    });
}

bool extractParameterSets(
    VideoCodec codec,
    const std::vector<uint8_t>& bytes,
    std::vector<uint8_t>& out) {
    out.clear();
    // Annex B only: both hardware backends emit start-code framing, and the
    // result is only ever prepended to Annex B packets.
    for (size_t offset = 0; offset + 3 <= bytes.size();) {
        const size_t startCodeSize = annexBStartCodeSize(bytes, offset);
        if (startCodeSize == 0) {
            ++offset;
            continue;
        }
        const size_t nalOffset = offset + startCodeSize;
        if (nalOffset >= bytes.size()) {
            break;
        }
        // Find the end of this NAL: the next start code or end of buffer.
        size_t end = nalOffset + 1;
        while (end + 3 <= bytes.size()) {
            if (annexBStartCodeSize(bytes, end) != 0) {
                break;
            }
            ++end;
        }
        if (end + 3 > bytes.size()) {
            // The scan can only stop on a start code, which needs a third byte
            // to prove itself, so a NAL running to the last byte leaves `end`
            // short of it. The result is cached as the replay's codec header
            // and prepended to every cut, so those 2 missing bytes would be a
            // PPS stripped of its trailing bits on an undecodable first frame.
            end = bytes.size();
        }
        if (nalUnitIsParameterSet(codec, bytes.data() + nalOffset)) {
            out.insert(out.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                       bytes.begin() + static_cast<std::ptrdiff_t>(end));
        }
        offset = end;
    }
    return !out.empty();
}

} // namespace backtrack
