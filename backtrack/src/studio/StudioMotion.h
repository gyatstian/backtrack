#pragma once

#include "core/TimeUnits.h"

#include <cmath>

namespace backtrack {

struct StudioMotion {
    float from = 0.f;
    float to = 0.f;
    SteadyClock::time_point start{};
    SteadyClock::duration duration{};

    bool active() const { return duration > SteadyClock::duration::zero(); }
    bool finished(SteadyClock::time_point now) const { return !active() || now - start >= duration; }

    float value(SteadyClock::time_point now, bool travel) const {
        if (finished(now)) return to;
        if (now <= start) return from;
        const float x = std::chrono::duration<float>(now - start) / duration;
        const auto bezier = [](float t, float a, float b) {
            const float u = 1.f - t;
            return t * (3.f * u * (u * a + t * b) + t * t);
        };
        const float a = travel ? 0.77f : 0.23f;
        const float b = travel ? 0.175f : 0.32f;
        float lo = 0.f, hi = 1.f;
        for (int i = 0; i < 10; ++i) {
            const float t = (lo + hi) * 0.5f;
            if (bezier(t, a, b) < x) lo = t;
            else hi = t;
        }
        return std::lerp(from, to, bezier((lo + hi) * 0.5f, travel ? 0.f : 1.f, 1.f));
    }

    void retarget(float next, SteadyClock::time_point now, SteadyClock::duration length, bool travel) {
        const float current = value(now, travel);
        *this = {current, next, now, current == next ? SteadyClock::duration{} : length};
    }
};

} // namespace backtrack
