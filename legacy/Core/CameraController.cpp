#include "CameraController.h"
#include <cmath>
#include <algorithm>
#include <cstdlib>

// ─────────────────────────────────────────────────────────────────────────────
// Shake
// ─────────────────────────────────────────────────────────────────────────────

void CameraController::Shake(float intensity, float duration) {
    mShakeIntensity = intensity;
    mShakeDuration = duration;
    mShakeElapsed = 0.0f;
}

// ─────────────────────────────────────────────────────────────────────────────
// FOV tween
// ─────────────────────────────────────────────────────────────────────────────

void CameraController::TweenFOV(float targetFOV, float duration) {
    mFOVStart = mFOVTweenActive ? GetFOVOverride() : mFOVTarget;
    mFOVTarget = targetFOV;
    mFOVDuration = duration;
    mFOVElapsed = 0.0f;
    mFOVTweenActive = true;
}

float CameraController::GetFOVOverride() const {
    if (!mFOVTweenActive) return 0.0f;
    float t = std::clamp(mFOVElapsed / mFOVDuration, 0.0f, 1.0f);
    t = EaseInOut(t);
    return glm::mix(mFOVStart, mFOVTarget, t);
}

// ─────────────────────────────────────────────────────────────────────────────
// Distance tween
// ─────────────────────────────────────────────────────────────────────────────

void CameraController::TweenDistance(float targetDist, float duration) {
    mDistStart = mDistTweenActive ? GetTweenedDistance() : mDistTarget;
    mDistTarget = targetDist;
    mDistDuration = duration;
    mDistElapsed = 0.0f;
    mDistTweenActive = true;
}

float CameraController::GetTweenedDistance() const {
    if (!mDistTweenActive) return 0.0f;
    float t = std::clamp(mDistElapsed / mDistDuration, 0.0f, 1.0f);
    t = EaseInOut(t);
    return glm::mix(mDistStart, mDistTarget, t);
}

// ─────────────────────────────────────────────────────────────────────────────
// Spline paths
// ─────────────────────────────────────────────────────────────────────────────

void CameraController::StartPath(const std::vector<glm::vec3>& positions,
                                  const std::vector<glm::vec3>& lookAts,
                                  float duration, bool loop) {
    mPath.positions = positions;
    mPath.lookAts = lookAts;
    mPath.duration = duration;
    mPath.elapsed = 0.0f;
    mPath.loop = loop;
    mPath.active = !positions.empty() && duration > 0.0f;
    if (mPath.active)
        mMode = Mode::Cinematic;
}

void CameraController::StopPath() {
    mPath.active = false;
    if (mMode == Mode::Cinematic)
        mMode = Mode::ThirdPerson;
}

float CameraController::GetPathProgress() const {
    if (!mPath.active || mPath.duration <= 0.0f) return 0.0f;
    return std::clamp(mPath.elapsed / mPath.duration, 0.0f, 1.0f);
}

glm::vec3 CameraController::GetPathPosition() const {
    if (!mPath.active || mPath.positions.empty()) return glm::vec3(0.0f);
    return EvaluateSpline(mPath.positions, GetPathProgress());
}

glm::vec3 CameraController::GetPathLookAt() const {
    if (!mPath.active) return glm::vec3(0.0f);
    if (mPath.lookAts.empty()) return GetPathPosition() + glm::vec3(0.0f, 0.0f, -1.0f);
    return EvaluateSpline(mPath.lookAts, GetPathProgress());
}

// ─────────────────────────────────────────────────────────────────────────────
// Update
// ─────────────────────────────────────────────────────────────────────────────

void CameraController::Update(float dt) {
    // Shake
    if (mShakeElapsed < mShakeDuration) {
        mShakeElapsed += dt;
        float decay = 1.0f - (mShakeElapsed / mShakeDuration);
        decay = std::max(decay, 0.0f);
        float strength = mShakeIntensity * decay;
        auto randF = []() { return (static_cast<float>(std::rand()) / RAND_MAX) * 2.0f - 1.0f; };
        mShakeOffset = glm::vec3(randF() * strength, randF() * strength, randF() * strength);
    } else {
        mShakeOffset = glm::vec3(0.0f);
    }

    // FOV tween
    if (mFOVTweenActive) {
        mFOVElapsed += dt;
        if (mFOVElapsed >= mFOVDuration)
            mFOVTweenActive = false;
    }

    // Distance tween
    if (mDistTweenActive) {
        mDistElapsed += dt;
        if (mDistElapsed >= mDistDuration)
            mDistTweenActive = false;
    }

    // Spline path
    if (mPath.active) {
        mPath.elapsed += dt;
        if (mPath.elapsed >= mPath.duration) {
            if (mPath.loop) {
                mPath.elapsed = std::fmod(mPath.elapsed, mPath.duration);
            } else {
                mPath.active = false;
                mMode = Mode::ThirdPerson;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

glm::vec3 CameraController::CatmullRom(const glm::vec3& p0, const glm::vec3& p1,
                                         const glm::vec3& p2, const glm::vec3& p3, float t) {
    float t2 = t * t;
    float t3 = t2 * t;
    return 0.5f * ((2.0f * p1) +
                    (-p0 + p2) * t +
                    (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                    (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

glm::vec3 CameraController::EvaluateSpline(const std::vector<glm::vec3>& points, float t) {
    if (points.empty()) return glm::vec3(0.0f);
    if (points.size() == 1) return points[0];
    if (points.size() == 2) return glm::mix(points[0], points[1], t);

    int n = static_cast<int>(points.size()) - 1;
    float segT = t * n;
    int seg = static_cast<int>(segT);
    seg = std::clamp(seg, 0, n - 1);
    float localT = segT - seg;

    auto pt = [&](int i) -> glm::vec3 {
        return points[std::clamp(i, 0, n)];
    };

    return CatmullRom(pt(seg - 1), pt(seg), pt(seg + 1), pt(seg + 2), localT);
}

float CameraController::EaseInOut(float t) {
    return t * t * (3.0f - 2.0f * t);
}
