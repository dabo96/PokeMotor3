#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <string>

class CameraController {
public:
    enum class Mode { ThirdPerson, Fixed, Follow, Cinematic };

    CameraController() = default;

    // Mode management
    void SetMode(Mode mode) { mMode = mode; }
    Mode GetMode() const { return mMode; }

    // Camera shake
    void Shake(float intensity, float duration);

    // FOV tween
    void TweenFOV(float targetFOV, float duration);
    bool IsFOVTweenActive() const { return mFOVTweenActive; }

    // Distance tween (for ThirdPerson mode)
    void TweenDistance(float targetDist, float duration);
    bool IsDistTweenActive() const { return mDistTweenActive; }
    float GetTweenedDistance() const;

    // Fixed mode
    void SetFixedPosition(const glm::vec3& pos) { mFixedPosition = pos; }
    void SetFixedLookAt(const glm::vec3& target) { mFixedLookAt = target; }
    glm::vec3 GetFixedPosition() const { return mFixedPosition; }
    glm::vec3 GetFixedLookAt() const { return mFixedLookAt; }

    // Follow mode
    void SetFollowOffset(const glm::vec3& offset) { mFollowOffset = offset; }
    glm::vec3 GetFollowOffset() const { return mFollowOffset; }

    // Spline paths (Cinematic mode)
    void StartPath(const std::vector<glm::vec3>& positions,
                   const std::vector<glm::vec3>& lookAts,
                   float duration, bool loop = false);
    void StopPath();
    bool IsPathActive() const { return mPath.active; }
    float GetPathProgress() const;
    glm::vec3 GetPathPosition() const;
    glm::vec3 GetPathLookAt() const;

    // Per-frame update
    void Update(float dt);

    // Post-processing results
    glm::vec3 GetShakeOffset() const { return mShakeOffset; }
    float GetFOVOverride() const;

private:
    Mode mMode = Mode::ThirdPerson;

    // Shake
    float mShakeIntensity = 0.0f;
    float mShakeDuration = 0.0f;
    float mShakeElapsed = 0.0f;
    glm::vec3 mShakeOffset{0.0f};

    // FOV tween
    float mFOVStart = 45.0f;
    float mFOVTarget = 45.0f;
    float mFOVDuration = 0.0f;
    float mFOVElapsed = 0.0f;
    bool mFOVTweenActive = false;

    // Distance tween
    float mDistStart = 0.0f;
    float mDistTarget = 0.0f;
    float mDistDuration = 0.0f;
    float mDistElapsed = 0.0f;
    bool mDistTweenActive = false;

    // Fixed mode
    glm::vec3 mFixedPosition{0.0f, 5.0f, 10.0f};
    glm::vec3 mFixedLookAt{0.0f};

    // Follow mode
    glm::vec3 mFollowOffset{0.0f, 5.0f, -8.0f};

    // Spline path
    struct SplinePath {
        std::vector<glm::vec3> positions;
        std::vector<glm::vec3> lookAts;
        float duration = 0.0f;
        float elapsed = 0.0f;
        bool loop = false;
        bool active = false;
    } mPath;

    // Helpers
    static glm::vec3 CatmullRom(const glm::vec3& p0, const glm::vec3& p1,
                                 const glm::vec3& p2, const glm::vec3& p3, float t);
    static glm::vec3 EvaluateSpline(const std::vector<glm::vec3>& points, float t);
    static float EaseInOut(float t);
};
