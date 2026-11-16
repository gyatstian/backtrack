#pragma once

#include <cstdint>

namespace backtrack {

// Which NVENC SDK the session is built against. A tier is one translation unit
// with its own SDK header (NvencEncoder13.cpp, NvencEncoder11.cpp): the headers
// declare the same types, so they cannot be mixed in one binary unit.
enum class NvencApiTier { Api11, Api13 };

// NvEncodeAPIGetMaxSupportedVersion reports the driver's NVENC API as
// (major << 4) | minor, so 11.1 is 177, 12.2 is 194, and 13.0 is 208.
constexpr uint32_t nvencApiVersion(uint32_t major, uint32_t minor) {
    return (major << 4) | minor;
}

constexpr uint32_t kNvencApiVersion11 = nvencApiVersion(11, 1);
constexpr uint32_t kNvencApiVersion13 = nvencApiVersion(13, 0);

// A driver accepts a client only when the client's API version is at or below
// the one it implements, and that version is baked into every NVENC struct at
// compile time. The newest compiled tier at or below the reported version is
// therefore the only one that can open a session; a driver below the oldest
// tier still gets it, so its own failure names the real status.
constexpr NvencApiTier selectNvencApiTier(uint32_t driverApiVersion) {
    return driverApiVersion >= kNvencApiVersion13 ? NvencApiTier::Api13 : NvencApiTier::Api11;
}

} // namespace backtrack
