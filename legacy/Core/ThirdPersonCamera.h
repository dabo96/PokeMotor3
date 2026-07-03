#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace physics {
    class TerrainCollider;
    class CollisionSystem;
}

namespace ecs {
    class Registry;
}

class ThirdPersonCamera {
public:
    ThirdPersonCamera();

    // Set the target position to follow (usually player entity's position)
    void SetTarget(const glm::vec3& target);

    // Process mouse input for orbit
    void ProcessMouseMovement(float xOffset, float yOffset);
    void ProcessMouseScroll(float yOffset);

    // Update camera position (call each frame)
    void Update(float dt, physics::TerrainCollider* terrain = nullptr,
                physics::CollisionSystem* collision = nullptr,
                ecs::Registry* registry = nullptr);

    // Get matrices
    glm::mat4 GetViewMatrix() const;
    glm::mat4 GetProjectionMatrix(float aspectRatio) const;

    // Accessors
    glm::vec3 GetPosition() const { return mPosition; }
    glm::vec3 GetFront() const { return mFront; }
    glm::vec3 GetTarget() const { return mTarget; }
    float GetYaw() const { return mYaw; }
    float GetPitch() const { return mPitch; }
    float GetFOV() const { return mFOV; }
    float GetNearPlane() const { return mNearPlane; }
    float GetFarPlane() const { return mFarPlane; }

    /// Resets orbit state to defaults (yaw=-90, pitch=20, position behind target).
    /// Called by App::EnterPlayMode() so the camera starts clean each Play session
    /// instead of carrying stale yaw/pitch from a previous play or pause.
    void Reset() {
        mYaw = -90.0f;
        mPitch = 20.0f;
        mPosition = glm::vec3(0.0f, 5.0f, 10.0f);
        mSmoothedPosition = mPosition;
        mTarget = glm::vec3(0.0f);
        mFront = glm::vec3(0.0f, 0.0f, -1.0f);
    }

    // Configuration
    float mDistance = 8.0f;
    float mMinDistance = 2.0f;
    float mMaxDistance = 20.0f;
    float mHeightOffset = 2.0f;    // How high above target the camera looks
    float mSmoothSpeed = 8.0f;     // Interpolation speed
    float mMouseSensitivity = 0.15f;
    float mScrollSensitivity = 2.0f;
    float mMinPitch = -60.0f;
    float mMaxPitch = 75.0f;
    float mFOV = 45.0f;
    float mNearPlane = 0.1f;
    float mFarPlane = 1000.0f;
    float mCollisionOffset = 0.3f;  // Push camera forward when hitting geometry

private:
    glm::vec3 mTarget{0.0f};
    glm::vec3 mPosition{0.0f, 5.0f, 10.0f};
    glm::vec3 mSmoothedPosition{0.0f, 5.0f, 10.0f};
    glm::vec3 mFront{0.0f, 0.0f, -1.0f};
    glm::vec3 mUp{0.0f, 1.0f, 0.0f};

    float mYaw = -90.0f;
    float mPitch = 20.0f;
};
