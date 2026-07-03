#pragma once
#include <functional>
#include <vector>
#include <string>
#include <cstdint>
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
// Easing functions (all take t in [0,1], return [0,1])
// ─────────────────────────────────────────────────────────────────────────────

namespace easing {
    inline float Linear(float t) { return t; }

    inline float EaseInQuad(float t) { return t * t; }
    inline float EaseOutQuad(float t) { return t * (2.0f - t); }
    inline float EaseInOutQuad(float t) { return t < 0.5f ? 2 * t * t : -1 + (4 - 2 * t) * t; }

    inline float EaseInCubic(float t) { return t * t * t; }
    inline float EaseOutCubic(float t) { float u = t - 1; return u * u * u + 1; }
    inline float EaseInOutCubic(float t) { return t < 0.5f ? 4 * t * t * t : (t - 1) * (2 * t - 2) * (2 * t - 2) + 1; }

    inline float EaseInSine(float t) { return 1.0f - std::cos(t * 3.14159265f * 0.5f); }
    inline float EaseOutSine(float t) { return std::sin(t * 3.14159265f * 0.5f); }
    inline float EaseInOutSine(float t) { return 0.5f * (1.0f - std::cos(3.14159265f * t)); }

    inline float EaseInExpo(float t) { return t == 0.0f ? 0.0f : std::pow(2.0f, 10.0f * (t - 1.0f)); }
    inline float EaseOutExpo(float t) { return t == 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t); }

    inline float EaseInBack(float t) { const float s = 1.70158f; return t * t * ((s + 1) * t - s); }
    inline float EaseOutBack(float t) { const float s = 1.70158f; float u = t - 1; return u * u * ((s + 1) * u + s) + 1; }

    inline float EaseInElastic(float t) {
        if (t <= 0.0f || t >= 1.0f) return t;
        return -std::pow(2.0f, 10.0f * (t - 1.0f)) * std::sin((t - 1.1f) * 5.0f * 3.14159265f);
    }
    inline float EaseOutElastic(float t) {
        if (t <= 0.0f || t >= 1.0f) return t;
        return std::pow(2.0f, -10.0f * t) * std::sin((t - 0.1f) * 5.0f * 3.14159265f) + 1.0f;
    }

    inline float EaseOutBounce(float t) {
        if (t < 1.0f / 2.75f) return 7.5625f * t * t;
        if (t < 2.0f / 2.75f) { t -= 1.5f / 2.75f; return 7.5625f * t * t + 0.75f; }
        if (t < 2.5f / 2.75f) { t -= 2.25f / 2.75f; return 7.5625f * t * t + 0.9375f; }
        t -= 2.625f / 2.75f; return 7.5625f * t * t + 0.984375f;
    }
    inline float EaseInBounce(float t) { return 1.0f - EaseOutBounce(1.0f - t); }

    // Lookup by name
    using EaseFunc = float(*)(float);
    EaseFunc FromName(const std::string& name);
}

// ─────────────────────────────────────────────────────────────────────────────
// TweenManager — manages active tweens with callbacks
// ─────────────────────────────────────────────────────────────────────────────

using TweenID = uint32_t;
constexpr TweenID INVALID_TWEEN = 0;

class TweenManager {
public:
    static TweenManager& Instance();

    // Create a tween from startVal to endVal over duration seconds
    TweenID Create(float startVal, float endVal, float duration,
                   easing::EaseFunc easeFn,
                   std::function<void(float)> onUpdate,
                   std::function<void()> onComplete = nullptr);

    void Cancel(TweenID id);
    void CancelAll();
    bool IsActive(TweenID id) const;

    // Call once per frame
    void Update(float deltaTime);

private:
    TweenManager() = default;

    struct TweenEntry {
        TweenID id = 0;
        float startVal = 0.0f;
        float endVal = 0.0f;
        float duration = 0.0f;
        float elapsed = 0.0f;
        easing::EaseFunc easeFn = easing::Linear;
        std::function<void(float)> onUpdate;
        std::function<void()> onComplete;
        bool active = false;
    };

    std::vector<TweenEntry> mTweens;
    TweenID mNextID = 1;
};
