#pragma once
#include <cstdint>
#include <limits>

namespace ecs {

// Generational Entity ID: 20 bits index + 12 bits generation
// Prevents stale references — if an entity is destroyed and its index reused,
// the old reference has a different generation and fails validation.
using Entity = uint32_t;
constexpr Entity NULL_ENTITY = std::numeric_limits<Entity>::max();

// Pack/unpack entity ID
constexpr uint32_t ENTITY_INDEX_BITS = 20;
constexpr uint32_t ENTITY_GEN_BITS = 12;
constexpr uint32_t ENTITY_INDEX_MASK = (1u << ENTITY_INDEX_BITS) - 1;
constexpr uint32_t ENTITY_GEN_MASK = (1u << ENTITY_GEN_BITS) - 1;

inline uint32_t GetEntityIndex(Entity e) { return e & ENTITY_INDEX_MASK; }
inline uint32_t GetEntityGeneration(Entity e) { return (e >> ENTITY_INDEX_BITS) & ENTITY_GEN_MASK; }
inline Entity MakeEntity(uint32_t index, uint32_t generation) {
    return (generation << ENTITY_INDEX_BITS) | index;
}

} // namespace ecs
