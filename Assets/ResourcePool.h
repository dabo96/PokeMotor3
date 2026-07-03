// Assets/ResourcePool.h — pool genérico de recursos con 'generation'.
// Diseño: MotorGrafico_AssetManager.md. Slots reutilizables; el generation del
// Handle (Core) detecta un slot liberado y reusado → caza use-after-free.
// `Tag` tipa el Handle (MeshTag/TextureTag/MaterialTag). T: default-construible + movible.
#pragma once

#include "Core/Handle.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace pk {

template <typename T, typename Tag>
class ResourcePool {
public:
    Handle<Tag> add(T&& resource) {
        uint32_t idx;
        if (!m_free.empty()) { idx = m_free.back(); m_free.pop_back(); }
        else { idx = static_cast<uint32_t>(m_slots.size()); m_slots.emplace_back(); }
        m_slots[idx].resource = std::move(resource);
        m_slots[idx].alive    = true;
        return Handle<Tag>{ idx, ++m_slots[idx].generation };
    }

    T* get(Handle<Tag> h) {
        if (h.index >= m_slots.size()) return nullptr;
        Slot& s = m_slots[h.index];
        if (!s.alive || s.generation != h.generation) return nullptr;  // colgado
        return &s.resource;
    }

    void remove(Handle<Tag> h) {
        if (!get(h)) return;
        m_slots[h.index].resource = T{};   // libera el recurso GPU (dtor de T)
        m_slots[h.index].alive    = false;
        m_free.push_back(h.index);
    }

    void clear() { m_slots.clear(); m_free.clear(); }  // destruye todos los recursos

private:
    struct Slot { T resource; uint32_t generation = 0; bool alive = false; };
    std::vector<Slot>     m_slots;
    std::vector<uint32_t> m_free;
};

}  // namespace pk
