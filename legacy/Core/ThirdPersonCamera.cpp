#include "ThirdPersonCamera.h"
#include "Physics/TerrainCollider.h"
#include "Physics/CollisionSystem.h"
#include "ECS/Registry.h"
#include <algorithm>
#include <cmath>

ThirdPersonCamera::ThirdPersonCamera() {}

void ThirdPersonCamera::SetTarget(const glm::vec3& target) {
    mTarget = target;
}

void ThirdPersonCamera::ProcessMouseMovement(float xOffset, float yOffset) {
    mYaw += xOffset * mMouseSensitivity;
    mPitch -= yOffset * mMouseSensitivity;
    mPitch = std::clamp(mPitch, mMinPitch, mMaxPitch);
}

void ThirdPersonCamera::ProcessMouseScroll(float yOffset) {
    mDistance -= yOffset * mScrollSensitivity;
    mDistance = std::clamp(mDistance, mMinDistance, mMaxDistance);
}

void ThirdPersonCamera::Update(float dt, physics::TerrainCollider* terrain,
                                physics::CollisionSystem* collision,
                                ecs::Registry* registry) {
    // Calculate desired position based on spherical coordinates around target
    float yawRad = glm::radians(mYaw);
    float pitchRad = glm::radians(mPitch);

    glm::vec3 offset;
    offset.x = mDistance * cos(pitchRad) * cos(yawRad);
    offset.y = mDistance * sin(pitchRad);
    offset.z = mDistance * cos(pitchRad) * sin(yawRad);

    glm::vec3 lookAt = mTarget + glm::vec3(0.0f, mHeightOffset, 0.0f);
    glm::vec3 desiredPosition = lookAt + offset;

    // Terrain collision: don't let camera go below terrain
    if (terrain) {
        auto height = terrain->GetHeightAt(desiredPosition.x, desiredPosition.z);
        if (height.has_value()) {
            float minY = *height + 1.0f; // 1m above terrain
            if (desiredPosition.y < minY)
                desiredPosition.y = minY;
        }
    }

    // Wall collision: raycast from target to desired position
    if (collision && registry) {
        physics::Ray ray;
        ray.origin = lookAt;
        glm::vec3 dir = desiredPosition - lookAt;
        float dist = glm::length(dir);
        if (dist > 0.001f) {
            ray.direction = dir / dist;
            auto hit = collision->Raycast(*registry, ray, dist, physics::LAYER_STATIC | physics::LAYER_TERRAIN);
            if (hit.hit && hit.distance < dist) {
                desiredPosition = lookAt + ray.direction * (hit.distance - mCollisionOffset);
            }
        }
    }

    // Smooth interpolation
    float t = 1.0f - std::exp(-mSmoothSpeed * dt);
    mSmoothedPosition = glm::mix(mSmoothedPosition, desiredPosition, t);
    mPosition = mSmoothedPosition;

    // Update front vector
    mFront = glm::normalize(lookAt - mPosition);
    mUp = glm::vec3(0.0f, 1.0f, 0.0f);
}

glm::mat4 ThirdPersonCamera::GetViewMatrix() const {
    glm::vec3 lookAt = mTarget + glm::vec3(0.0f, mHeightOffset, 0.0f);
    return glm::lookAt(mPosition, lookAt, mUp);
}

glm::mat4 ThirdPersonCamera::GetProjectionMatrix(float aspectRatio) const {
    return glm::perspective(glm::radians(mFOV), aspectRatio, mNearPlane, mFarPlane);
}
