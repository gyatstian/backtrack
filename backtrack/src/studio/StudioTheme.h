#pragma once

#include <Windows.h>

#include <algorithm>

namespace backtrack::studio_theme {

inline constexpr COLORREF surface = RGB(13, 14, 16);
inline constexpr COLORREF active = RGB(27, 29, 32);
inline constexpr COLORREF field = RGB(21, 23, 26);
inline constexpr COLORREF hover = RGB(36, 39, 44);
inline constexpr COLORREF press = RGB(45, 49, 56);
inline constexpr COLORREF closeHover = RGB(160, 36, 53);
inline constexpr COLORREF closePress = RGB(127, 29, 43);
inline constexpr COLORREF text = RGB(236, 238, 240);
inline constexpr COLORREF muted = RGB(164, 169, 176);
inline constexpr COLORREF disabled = RGB(116, 121, 129);
inline constexpr COLORREF rule = RGB(44, 47, 52);
inline constexpr COLORREF divider = RGB(56, 61, 67);
inline constexpr COLORREF border = RGB(101, 107, 115);
inline constexpr COLORREF accent = RGB(152, 201, 253);
inline constexpr COLORREF recording = RGB(250, 112, 122);
inline constexpr COLORREF success = RGB(122, 223, 166);

inline constexpr int navHeight = 24;
inline constexpr int navUnderlineBand = 3;
inline constexpr int footerHeight = 60;
inline constexpr int rowHeight = 34;
inline constexpr int inset = 14;
inline constexpr int sectionInset = 25;
inline constexpr int dependencyIndent = 12;
inline constexpr int gap = 8;
// Form pages share one label column, measured against the content width, so
// every control of a page sits on the same x. A dependent label narrows by its
// indent instead of pushing its control right.
inline constexpr int labelColumnPercent = 36;

// UI scale = DPI scale * zoom * window factor. The window factor follows the
// content width against the default window size in both directions, so the layout
// keeps its density at any size; the product clamp keeps text legible and stops
// one huge window from swallowing a whole page.
inline constexpr int minUiScale = 60;
inline constexpr int maxUiScale = 400;
inline constexpr int minUiZoom = 60;
inline constexpr int maxUiZoom = 300;

inline int uiScalePercent(int zoomPercent, int windowWidth, int baseWidth, bool followWindow) {
    const int follow = followWindow && windowWidth > 0 && baseWidth > 0
        ? (std::max)(1, MulDiv(windowWidth, 100, baseWidth)) : 100;
    return std::clamp(MulDiv(zoomPercent, follow, 100), minUiScale, maxUiScale);
}

inline RECT tabBounds(const RECT& row, int index, int count, int gap) {
    const int width = row.right - row.left;
    if (gap > width / count) gap = width / count;
    const int available = width - (count - 1) * gap;
    const int left = row.left + MulDiv(available, index, count) + index * gap;
    return {left, row.top, row.left + MulDiv(available, index + 1, count) + index * gap, row.bottom};
}

inline void fill(HDC dc, const RECT& rect, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

inline void frame(HDC dc, const RECT& rect, COLORREF color) {
    SetDCBrushColor(dc, color);
    FrameRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

} // namespace backtrack::studio_theme
