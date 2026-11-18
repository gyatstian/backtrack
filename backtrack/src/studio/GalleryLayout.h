#pragma once

#include <algorithm>
#include <cstddef>
#include <utility>

namespace backtrack {

struct StudioGalleryLayout {
    int columns = 1;
    int rowHeight = 1;

    constexpr size_t rowCount(size_t clips) const {
        return clips / columns + (clips % columns != 0);
    }
    constexpr std::pair<size_t, size_t> visible(size_t clips, int scroll, int height, int overscan = 0) const {
        if (height <= 0) return {0, 0};
        const auto first = static_cast<size_t>(scroll / rowHeight);
        const auto end = static_cast<size_t>((static_cast<long long>(scroll) + height + rowHeight - 1) / rowHeight);
        return {(std::min)(clips, first * columns),
                (std::min)(clips, (end + overscan) * columns)};
    }
};

} // namespace backtrack
