#include "studio/StudioWindowImpl.h"
#include "studio/StudioTheme.h"

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
    const int bottom = px(theme::titleBarHeight) + (slot == categoryMarker ? 2 : 1) * px(theme::navHeight);
    return {layoutGrip, bottom - px(theme::navUnderlineBand),
            (std::max)(layoutGrip, static_cast<int>(layoutSize.cx) - layoutGrip), bottom};
}

void StudioWindow::Impl::paintTabMarker(HDC dc, Motion slot) {
    const RECT track = markerTrack(slot);
    RECT rule{0, track.bottom - 1, layoutSize.cx, track.bottom};
    theme::fill(dc, rule, theme::rule);
    if (!RectVisible(dc, &track)) return;
    const int count = static_cast<int>(slot == mainMarker ? pageButtons.size() : categoryButtons.size());
    const float at = motionValue(slot);
    const int first = static_cast<int>(at), second = (std::min)(first + 1, count - 1);
    const auto a = theme::tabBounds(track, first, count, px(theme::gap));
    const auto b = theme::tabBounds(track, second, count, px(theme::gap));
    const auto left = interpolate(a.left, b.left, at - first);
    const auto right = interpolate(a.right, b.right, at - first);
    theme::fill(dc, {left, track.top, right, track.bottom - px(1)}, theme::muted);
}

RECT StudioWindow::Impl::recordingGlyphBox(const RECT& client) const {
    const int size = px(24), x = px(8), y = (client.top + client.bottom - size) / 2;
    return {x, y, x + size, y + size};
}

void StudioWindow::Impl::paintRecordingGlyph(HDC dc, const RECT& box, bool disabled) {
    if (!RectVisible(dc, &box)) return;
    const float p = motionValue(recordGlyph);
    const int size = interpolate(px(14), px(20), p);
    const int corner = std::lround(size * (1.f - p));
    const int x = (box.left + box.right - size) / 2, y = (box.top + box.bottom - size) / 2;
    SetDCBrushColor(dc, disabled ? theme::disabled : theme::recording);
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(NULL_PEN));
    if (corner > 1) RoundRect(dc, x, y, x + size, y + size, corner, corner);
    else Rectangle(dc, x, y, x + size, y + size);
}

void StudioWindow::Impl::paintReplayCheck(HDC dc) {
    if (!RectVisible(dc, &replayCheckBox)) return;
    const float p = motionValue(replayCheck);
    if (p <= 0.f) return;
    const int x = replayCheckBox.left + px(2), y = replayCheckBox.top;
    const POINT start{x, y + px(11)}, elbow{x + px(6), y + px(17)}, end{x + px(19), y + px(4)};
    constexpr float turn = 0.32f;
    const bool first = p < turn;
    const auto from = first ? start : elbow;
    const auto to = first ? elbow : end;
    const float t = first ? p / turn : (p - turn) / (1.f - turn);
    const POINT tip{interpolate(from.x, to.x, t), interpolate(from.y, to.y, t)};
    const POINT stroke[]{start, first ? tip : elbow, tip};
    if (const auto pen = CreatePen(PS_SOLID, px(2), theme::success)) {
        const auto old = SelectObject(dc, pen);
        Polyline(dc, stroke, first ? 2 : 3);
        SelectObject(dc, old);
        DeleteObject(pen);
    }
}

} // namespace backtrack
