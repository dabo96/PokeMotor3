#pragma once
#include "Entity.h"
#include <vector>
#include <unordered_map>
#include <cassert>
#include <typeindex>

namespace ecs {

// Type-erased base for storing component pools in a single container
class IComponentPool {
public:
    virtual ~IComponentPool() = default;
    virtual void Remove(Entity entity) = 0;
    virtual bool Has(Entity entity) const = 0;
    virtual void Clear() = 0;
};

// Sparse-set based component storage: O(1) add/get/remove, cache-friendly iteration
template<typename T>
class ComponentPool : public IComponentPool {
public:
    T& Add(Entity entity, T component) {
        assert(!Has(entity) && "Entity already has this component");
        size_t index = mComponents.size();
        mComponents.push_back(std::move(component));
        mEntities.push_back(entity);
        mEntityToIndex[entity] = index;
        return mComponents.back();
    }

    template<typename... Args>
    T& Emplace(Entity entity, Args&&... args) {
        assert(!Has(entity) && "Entity already has this component");
        size_t index = mComponents.size();
        mComponents.emplace_back(std::forward<Args>(args)...);
        mEntities.push_back(entity);
        mEntityToIndex[entity] = index;
        return mComponents.back();
    }

    void Remove(Entity entity) override {
        auto it = mEntityToIndex.find(entity);
        if (it == mEntityToIndex.end()) return;

        size_t removedIndex = it->second;
        size_t lastIndex = mComponents.size() - 1;

        if (removedIndex != lastIndex) {
            // Swap with last element
            mComponents[removedIndex] = std::move(mComponents[lastIndex]);
            mEntities[removedIndex] = mEntities[lastIndex];
            mEntityToIndex[mEntities[removedIndex]] = removedIndex;
        }

        mComponents.pop_back();
        mEntities.pop_back();
        mEntityToIndex.erase(entity);
    }

    T& Get(Entity entity) {
        auto it = mEntityToIndex.find(entity);
        assert(it != mEntityToIndex.end() && "Entity does not have this component");
        return mComponents[it->second];
    }

    const T& Get(Entity entity) const {
        auto it = mEntityToIndex.find(entity);
        assert(it != mEntityToIndex.end() && "Entity does not have this component");
        return mComponents[it->second];
    }

    bool Has(Entity entity) const override {
        return mEntityToIndex.find(entity) != mEntityToIndex.end();
    }

    void Clear() override {
        mComponents.clear();
        mEntities.clear();
        mEntityToIndex.clear();
    }

    // For iteration
    size_t Size() const { return mComponents.size(); }
    const std::vector<Entity>& GetEntities() const { return mEntities; }
    std::vector<T>& GetComponents() { return mComponents; }
    const std::vector<T>& GetComponents() const { return mComponents; }

    Entity GetEntity(size_t index) const { return mEntities[index]; }
    T& GetByIndex(size_t index) { return mComponents[index]; }

private:
    std::vector<T> mComponents;
    std::vector<Entity> mEntities;
    std::unordered_map<Entity, size_t> mEntityToIndex;
};

} // namespace ecs
