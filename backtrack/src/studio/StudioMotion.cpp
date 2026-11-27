#include "studio/StudioWindowImpl.h"
#include "studio/StudioTheme.h"

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>

namespace backtrack {
namespace {
namespace theme = studio_theme;
using namespace std::chrono_literals;

LONG interpolate(LONG a, LONG b, float t) {
    return std::lround(std::lerp(static_cast<float>(a), static_cast<float>(b), t));
}
} // namespace

void StudioWindow::Impl::updateMotionPreference() {
    BOOL enabled = FALSE;
    motionEnabled = SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) && enabled;
    if (!motionEnabled) stopMotion();
}

void StudioWindow::Impl::stopMotion() {
    if (!std::ranges::any_of(motions, &StudioMotion::active)) return;
    KillTimer(window, motionTimer);
    for (auto i = 0; i < motionCount; ++i) {
        if (!motions[i].active()) continue;
        motions[i].duration = {};
        invalidateMotion(static_cast<Motion>(i));
    }
}

void StudioWindow::Impl::tickMotion() {
    if (closing || !IsWindowEnabled(window) || !IsWindowVisible(window) || IsIconic(window)) {
        stopMotion();
        return;
    }
    const auto now = SteadyClock::now();
    bool running = false;
    for (auto i = 0; i < motionCount; ++i) {
        auto& motion = motions[i];
        if (!motion.active()) continue;
        if (motion.finished(now)) motion.duration = {};
        else running = true;
        invalidateMotion(static_cast<Motion>(i));
    }
    if (!running) KillTimer(window, motionTimer);
}

void StudioWindow::Impl::setMotion(Motion slot, float target, bool animate) {
    auto& motion = motions[slot];
    animate = animate && motionEnabled && !closing && !sizing && IsWindowEnabled(window) &&
        IsWindowVisible(window) && !IsIconic(window);
    if (motion.to == target && (animate || !motion.active())) return;
    const bool running = std::ranges::any_of(motions, &StudioMotion::active);
    if (animate) {
        const bool travel = slot == mainMarker || slot == categoryMarker;
        motion.retarget(target, SteadyClock::now(), travel ? 200ms : slot == recordGlyph ? 160ms : 180ms, travel);
        if (motion.active() && !running && !SetTimer(window, motionTimer, 16, nullptr)) motion.duration = {};
    } else {
        motion = {target, target};
    }
    if (running && !std::ranges::any_of(motions, &StudioMotion::active)) KillTimer(window, motionTimer);
    invalidateMotion(slot);
}

void StudioWindow::Impl::invalidateMotion(Motion slot) {
    if (slot == recordGlyph) {
        RECT client{};
        if (!record || !GetClientRect(record, &client)) return;
        const RECT box = recordingGlyphBox(client);
        InvalidateRect(record, &box, FALSE);
    } else if (slot == replayCheck) {
        InvalidateRect(window, &replayCheckBox, FALSE);
    } else if (slot == librarySlide) {
        layoutGallery();
    } else {
        const RECT track = markerTrack(slot);
        InvalidateRect(window, &track, FALSE);
    }
}

float StudioWindow::Impl::motionValue(Motion slot) const {
    const auto& motion = motions[slot];
    return motion.active() ? motion.value(SteadyClock::now(), slot == mainMarker || slot == categoryMarker) : motion.to;
}

RECT StudioWindow::Impl::markerTrack(Motion slot) const {
    const int bottom = (slot == categoryMarker ? 2 : 1) * px(theme::navHeight);
    return {0, bottom - px(theme::navUnderlineBand), static_cast<int>(layoutSize.cx), bottom};
}

void StudioWindow::Impl::paintTabMarker(HDC dc, Motion slot) {
    const RECT track = markerTrack(slot);
    const int hairline = hairlinePx();
    RECT rule{0, track.bottom - hairline, layoutSize.cx, track.bottom};
    theme::fill(dc, rule, theme::rule);
    if (!RectVisible(dc, &track)) return;
    const int count = static_cast<int>(slot == mainMarker ? pageButtons.size() : categoryButtons.size());
    const float at = motionValue(slot);
    const int first = static_cast<int>(at), second = (std::min)(first + 1, count - 1);
    const auto a = theme::tabBounds(track, first, count, px(theme::gap));
    const auto b = theme::tabBounds(track, second, count, px(theme::gap));
    const auto left = interpolate(a.left, b.left, at - first);
    const auto right = interpolate(a.right, b.right, at - first);
    theme::fill(dc, {left, track.top, right, track.bottom - hairline}, theme::muted);
}

RECT StudioWindow::Impl::recordingGlyphBox(const RECT& client) const {
    const int size = px(24), x = px(8), y = (client.top + client.bottom - size) / 2;
    return {x, y, x + size, y + size};
}

void StudioWindow::Impl::paintRecordingGlyph(HDC dc, const RECT& box, bool disabled) {
    if (!RectVisible(dc, &box)) return;
    const float p = motionValue(recordGlyph);
    const float size = std::lerp(static_cast<float>(px(14)), static_cast<float>(px(20)), p);
    const float x = (box.left + box.right - size) * 0.5f;
    const float y = (box.top + box.bottom - size) * 0.5f;
    const COLORREF c = disabled ? theme::disabled : theme::recording;
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)));
    if (p <= 0.01f) {
        g.FillEllipse(&brush, x, y, size, size);
    } else if (p >= 0.99f) {
        g.FillRectangle(&brush, x, y, size, size);
    } else {
        const float r = size * 0.5f * (1.f - p);
        const float d = r * 2.f;
        Gdiplus::GraphicsPath path;
        path.AddArc(x, y, d, d, 180.f, 90.f);
        path.AddArc(x + size - d, y, d, d, 270.f, 90.f);
        path.AddArc(x + size - d, y + size - d, d, d, 0.f, 90.f);
        path.AddArc(x, y + size - d, d, d, 90.f, 90.f);
        path.CloseFigure();
        g.FillPath(&brush, &path);
    }
}

void StudioWindow::Impl::paintReplayCheck(HDC dc) {
    if (!RectVisible(dc, &replayCheckBox)) return;
    const float p = motionValue(replayCheck);
    if (p <= 0.f) return;
    const int x = replayCheckBox.left + px(2), y = replayCheckBox.top;
    const Gdiplus::PointF start{static_cast<float>(x), static_cast<float>(y + px(11))};
    const Gdiplus::PointF elbow{static_cast<float>(x + px(6)), static_cast<float>(y + px(17))};
    const Gdiplus::PointF end{static_cast<float>(x + px(19)), static_cast<float>(y + px(4))};
    constexpr float turn = 0.32f;
    const bool first = p < turn;
    const auto from = first ? start : elbow;
    const auto to = first ? elbow : end;
    const float t = first ? p / turn : (p - turn) / (1.f - turn);
    const Gdiplus::PointF tip{std::lerp(from.X, to.X, t), std::lerp(from.Y, to.Y, t)};
    const Gdiplus::PointF stroke[]{start, first ? tip : elbow, tip};
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(theme::success), GetGValue(theme::success), GetBValue(theme::success)),
                     static_cast<Gdiplus::REAL>(px(2)));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    g.DrawLines(&pen, stroke, first ? 2 : 3);
}

} // namespace backtrack
