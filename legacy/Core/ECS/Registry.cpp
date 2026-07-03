#include "Registry.h"

namespace ecs {

Entity Registry::CreateEntity() {
    uint32_t index;
    uint32_t gen;

    if (!mFreeIndices.empty()) {
        // Reuse a recycled index with bumped generation
        index = mFreeIndices.back();
        mFreeIndices.pop_back();
        gen = mGenerations[index]; // generation was bumped on destroy
    } else {
        // Allocate a new index
        index = static_cast<uint32_t>(mGenerations.size());
        mGenerations.push_back(0);
        gen = 0;
    }

    Entity entity = MakeEntity(index, gen);
    mAliveEntities.insert(entity);
    return entity;
}

void Registry::DestroyEntity(Entity entity) {
    if (!IsAlive(entity)) return;

    // Remove from all component pools
    for (auto& [type, pool] : mPools) {
        pool->Remove(entity);
    }

    mAliveEntities.erase(entity);

    // Bump generation so old references to this index become stale
    uint32_t index = GetEntityIndex(entity);
    mGenerations[index] = (mGenerations[index] + 1) & ENTITY_GEN_MASK;
    mFreeIndices.push_back(index);
}

bool Registry::IsAlive(Entity entity) const {
    return mAliveEntities.find(entity) != mAliveEntities.end();
}

bool Registry::IsValid(Entity entity) const {
    if (entity == NULL_ENTITY) return false;
    uint32_t index = GetEntityIndex(entity);
    if (index >= mGenerations.size()) return false;
    return GetEntityGeneration(entity) == mGenerations[index]
        && mAliveEntities.count(entity) > 0;
}

void Registry::Clear() {
    for (auto& [type, pool] : mPools) {
        pool->Clear();
    }
    mAliveEntities.clear();
    mGenerations.clear();
    mFreeIndices.clear();
}

} // namespace ecs
