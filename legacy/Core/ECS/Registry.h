#pragma once
#include "Entity.h"
#include "Component.h"
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <vector>

namespace ecs {

class Registry {
public:
    Registry() = default;
    ~Registry() = default;

    // Entity management
    Entity CreateEntity();
    void DestroyEntity(Entity entity);
    bool IsAlive(Entity entity) const;
    bool IsValid(Entity entity) const;  // checks generation match + alive

    // Component operations
    template<typename T, typename... Args>
    T& AddComponent(Entity entity, Args&&... args) {
        auto& pool = GetOrCreatePool<T>();
        return pool.Emplace(entity, std::forward<Args>(args)...);
    }

    template<typename T>
    void RemoveComponent(Entity entity) {
        auto* pool = GetPool<T>();
        if (pool) pool->Remove(entity);
    }

    template<typename T>
    T& GetComponent(Entity entity) {
        auto* pool = GetPool<T>();
        assert(pool && "Component pool does not exist");
        return pool->Get(entity);
    }

    template<typename T>
    const T& GetComponent(Entity entity) const {
        auto* pool = GetPoolConst<T>();
        assert(pool && "Component pool does not exist");
        return pool->Get(entity);
    }

    template<typename T>
    bool HasComponent(Entity entity) const {
        auto* pool = GetPoolConst<T>();
        return pool && pool->Has(entity);
    }

    // View: iterate entities that have ALL specified components
    // Usage: for (auto [entity, transform, render] : registry.View<Transform, Render>()) { ... }
    template<typename... Ts>
    class View {
    public:
        View(Registry& reg) : mRegistry(reg) {}

        class Iterator {
        public:
            Iterator(Registry& reg, size_t index, ComponentPool<std::tuple_element_t<0, std::tuple<Ts...>>>* primary)
                : mRegistry(reg), mIndex(index), mPrimary(primary) {
                if (mPrimary) AdvanceToValid();
            }

            bool operator!=(const Iterator& other) const { return mIndex != other.mIndex; }
            void operator++() { ++mIndex; if (mPrimary) AdvanceToValid(); }

            auto operator*() {
                Entity entity = mPrimary->GetEntity(mIndex);
                return std::tuple<Entity, Ts&...>(entity, mRegistry.GetComponent<Ts>(entity)...);
            }

        private:
            void AdvanceToValid() {
                while (mIndex < mPrimary->Size()) {
                    Entity entity = mPrimary->GetEntity(mIndex);
                    if (mRegistry.IsAlive(entity) && HasAll<Ts...>(entity))
                        return;
                    ++mIndex;
                }
            }

            template<typename First, typename... Rest>
            bool HasAll(Entity entity) {
                if (!mRegistry.HasComponent<First>(entity)) return false;
                if constexpr (sizeof...(Rest) > 0)
                    return HasAll<Rest...>(entity);
                return true;
            }

            Registry& mRegistry;
            size_t mIndex;
            ComponentPool<std::tuple_element_t<0, std::tuple<Ts...>>>* mPrimary;
        };

        Iterator begin() {
            using First = std::tuple_element_t<0, std::tuple<Ts...>>;
            auto* pool = mRegistry.GetPool<First>();
            return Iterator(mRegistry, 0, pool);
        }

        Iterator end() {
            using First = std::tuple_element_t<0, std::tuple<Ts...>>;
            auto* pool = mRegistry.GetPool<First>();
            return Iterator(mRegistry, pool ? pool->Size() : 0, pool);
        }

    private:
        Registry& mRegistry;
    };

    template<typename... Ts>
    View<Ts...> GetView() { return View<Ts...>(*this); }

    // Iterate all entities with a single component type (fast path)
    template<typename T>
    void Each(std::function<void(Entity, T&)> func) {
        auto* pool = GetPool<T>();
        if (!pool) return;
        for (size_t i = 0; i < pool->Size(); ++i) {
            Entity e = pool->GetEntity(i);
            if (IsAlive(e))
                func(e, pool->GetByIndex(i));
        }
    }

    void Clear();
    size_t EntityCount() const { return mAliveEntities.size(); }
    const std::unordered_set<Entity>& GetAliveEntities() const { return mAliveEntities; }

    template<typename T>
    ComponentPool<T>* GetPool() {
        auto it = mPools.find(std::type_index(typeid(T)));
        if (it == mPools.end()) return nullptr;
        return static_cast<ComponentPool<T>*>(it->second.get());
    }

private:
    template<typename T>
    const ComponentPool<T>* GetPoolConst() const {
        auto it = mPools.find(std::type_index(typeid(T)));
        if (it == mPools.end()) return nullptr;
        return static_cast<const ComponentPool<T>*>(it->second.get());
    }

    template<typename T>
    ComponentPool<T>& GetOrCreatePool() {
        auto key = std::type_index(typeid(T));
        auto it = mPools.find(key);
        if (it != mPools.end())
            return *static_cast<ComponentPool<T>*>(it->second.get());
        auto pool = std::make_unique<ComponentPool<T>>();
        auto* ptr = pool.get();
        mPools[key] = std::move(pool);
        return *ptr;
    }

    std::unordered_set<Entity> mAliveEntities;
    std::unordered_map<std::type_index, std::unique_ptr<IComponentPool>> mPools;

    // Generational ID system: index → current generation
    std::vector<uint32_t> mGenerations;
    std::vector<uint32_t> mFreeIndices; // recycled indices
};

} // namespace ecs
