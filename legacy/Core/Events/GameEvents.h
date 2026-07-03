#pragma once
#include "../ECS/Entity.h"
#include <glm/glm.hpp>
#include <string>

namespace events {

struct CollisionEvent {
    ecs::Entity entityA;
    ecs::Entity entityB;
    glm::vec3 contactPoint;
    glm::vec3 contactNormal;
};

struct TriggerEvent {
    ecs::Entity trigger;
    ecs::Entity other;
    bool entered; // true = enter, false = exit
};

struct SceneChangeEvent {
    std::string sceneName;
    glm::vec3 spawnPosition{0.0f};
};

struct InputActionEvent {
    int action; // Maps to InputAction enum
    bool pressed;
};

struct BattleStartEvent {
    ecs::Entity wildPokemon;
    ecs::Entity trainer; // NULL_ENTITY for wild encounters
};

struct BattleEndEvent {
    bool playerWon;
};

struct DialogueStartEvent {
    std::string dialogueID;
    ecs::Entity speaker;
};

struct DialogueEndEvent {
    std::string dialogueID;
};

struct EntityDestroyedEvent {
    ecs::Entity entity;
};

struct PlaySoundEvent {
    std::string clipName;
    float volume = 1.0f;
    bool loop = false;
};

struct StopSoundEvent {
    uint32_t handle = 0;
    bool stopAll = false;
};

} // namespace events
