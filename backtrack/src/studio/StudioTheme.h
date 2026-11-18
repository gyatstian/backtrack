#pragma once

#include <Windows.h>

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

inline constexpr int titleBarHeight = 26;
inline constexpr int navHeight = 24;
inline constexpr int footerHeight = 60;
inline constexpr int rowHeight = 34;
inline constexpr int inset = 14;
inline constexpr int sectionInset = 25;
inline constexpr int dependencyIndent = 12;
inline constexpr int gap = 8;

inline void fill(HDC dc, const RECT& rect, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

inline void frame(HDC dc, const RECT& rect, COLORREF color) {
    SetDCBrushColor(dc, color);
    FrameRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

} // namespace backtrack::studio_theme
