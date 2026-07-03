#include "TweenManager.h"
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Easing lookup
// ─────────────────────────────────────────────────────────────────────────────

namespace easing {

EaseFunc FromName(const std::string& name) {
    if (name == "linear")       return Linear;
    if (name == "easeInQuad")   return EaseInQuad;
    if (name == "easeOutQuad")  return EaseOutQuad;
    if (name == "easeInOutQuad") return EaseInOutQuad;
    if (name == "easeInCubic")  return EaseInCubic;
    if (name == "easeOutCubic") return EaseOutCubic;
    if (name == "easeInOutCubic") return EaseInOutCubic;
    if (name == "easeInSine")   return EaseInSine;
    if (name == "easeOutSine")  return EaseOutSine;
    if (name == "easeInOutSine") return EaseInOutSine;
    if (name == "easeInExpo")   return EaseInExpo;
    if (name == "easeOutExpo")  return EaseOutExpo;
    if (name == "easeInBack")   return EaseInBack;
    if (name == "easeOutBack")  return EaseOutBack;
    if (name == "easeInElastic") return EaseInElastic;
    if (name == "easeOutElastic") return EaseOutElastic;
    if (name == "easeInBounce") return EaseInBounce;
    if (name == "easeOutBounce") return EaseOutBounce;
    return Linear;
}

} // namespace easing

// ─────────────────────────────────────────────────────────────────────────────
// TweenManager
// ─────────────────────────────────────────────────────────────────────────────

TweenManager& TweenManager::Instance() {
    static TweenManager instance;
    return instance;
}

TweenID TweenManager::Create(float startVal, float endVal, float duration,
                              easing::EaseFunc easeFn,
                              std::function<void(float)> onUpdate,
                              std::function<void()> onComplete) {
    TweenID id = mNextID++;
    mTweens.push_back({id, startVal, endVal, std::max(duration, 0.001f), 0.0f,
                        easeFn ? easeFn : easing::Linear,
                        std::move(onUpdate), std::move(onComplete), true});
    return id;
}

void TweenManager::Cancel(TweenID id) {
    for (auto& t : mTweens) {
        if (t.id == id) { t.active = false; break; }
    }
}

void TweenManager::CancelAll() {
    mTweens.clear();
}

bool TweenManager::IsActive(TweenID id) const {
    for (auto& t : mTweens) {
        if (t.id == id) return t.active;
    }
    return false;
}

void TweenManager::Update(float deltaTime) {
    for (auto& t : mTweens) {
        if (!t.active) continue;

        t.elapsed += deltaTime;
        float raw = std::min(t.elapsed / t.duration, 1.0f);
        float eased = t.easeFn(raw);
        float value = t.startVal + (t.endVal - t.startVal) * eased;

        if (t.onUpdate) t.onUpdate(value);

        if (raw >= 1.0f) {
            t.active = false;
            if (t.onComplete) t.onComplete();
        }
    }

    // Cleanup finished tweens
    mTweens.erase(
        std::remove_if(mTweens.begin(), mTweens.end(),
            [](const TweenEntry& t) { return !t.active; }),
        mTweens.end());
}
