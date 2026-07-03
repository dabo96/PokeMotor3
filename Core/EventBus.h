// Core/EventBus.h — el "glue" entre subsistemas: emiten/escuchan sin conocerse.
// Diseño: MotorGrafico_EventBus.md. Identidad por tipo de C++ (std::type_index),
// inmediato (emit) vs diferido (queue+dispatch), y token RAII Subscription que
// se desuscribe solo al morir (evita el bug de callback colgado a memoria
// liberada). NO es singleton: lo posee la Application y se pasa por referencia.
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace pk {

class EventBus;

// Token RAII de suscripción. No copiable, sí movible. Al destruirse llama de
// vuelta al bus para limpiarse.
class Subscription {
public:
    Subscription() = default;
    Subscription(EventBus* bus, std::type_index t, uint64_t id)
        : m_bus(bus), m_type(t), m_id(id) {}
    ~Subscription();

    Subscription(const Subscription&)            = delete;
    Subscription& operator=(const Subscription&) = delete;
    Subscription(Subscription&& o) noexcept { *this = std::move(o); }
    Subscription& operator=(Subscription&& o) noexcept;

    void release();

private:
    EventBus*       m_bus  = nullptr;
    std::type_index m_type = std::type_index(typeid(void));
    uint64_t        m_id   = 0;
};

class EventBus {
public:
    // Suscribirse a un tipo de evento. Devuelve un token RAII; guárdalo como
    // miembro de la clase suscriptora.
    template <typename E>
    [[nodiscard]] Subscription subscribe(std::function<void(const E&)> handler) {
        auto     type = std::type_index(typeid(E));
        uint64_t id   = m_nextId++;
        m_handlers[type].push_back({ id,
            [h = std::move(handler)](const void* e) {
                h(*static_cast<const E*>(e));
            } });
        return Subscription{ this, type, id };
    }

    // Entrega inmediata, en la misma pila.
    template <typename E>
    void emit(const E& event) {
        auto it = m_handlers.find(std::type_index(typeid(E)));
        if (it == m_handlers.end()) return;
        // Copia la lista: un handler puede (des)suscribirse durante la iteración
        // sin invalidar este bucle.
        auto handlers = it->second;
        for (auto& entry : handlers) entry.fn(&event);
    }

    // Entrega diferida: encola una copia; se reparte en dispatch().
    template <typename E>
    void queue(const E& event) {
        m_queue.push_back([this, event] { emit(event); });
    }

    // Vacía la cola una vez por frame (tras la lógica, antes del render).
    void dispatch() {
        auto pending = std::move(m_queue);  // swap: un handler puede encolar
        m_queue.clear();                    // sin invalidar este bucle
        for (auto& deliver : pending) deliver();
    }

private:
    struct HandlerEntry {
        uint64_t                         id;
        std::function<void(const void*)> fn;
    };
    std::unordered_map<std::type_index, std::vector<HandlerEntry>> m_handlers;
    std::vector<std::function<void()>>                            m_queue;
    uint64_t                                                       m_nextId = 1;

    void unsubscribe(std::type_index type, uint64_t id) {
        auto it = m_handlers.find(type);
        if (it == m_handlers.end()) return;
        auto& v = it->second;
        v.erase(std::remove_if(v.begin(), v.end(),
                               [id](auto& h) { return h.id == id; }),
                v.end());
    }
    friend class Subscription;
};

// --- Subscription: cuerpos que necesitan EventBus completo ---
inline Subscription::~Subscription() { release(); }

inline void Subscription::release() {
    if (m_bus) {
        m_bus->unsubscribe(m_type, m_id);
        m_bus = nullptr;
    }
}

inline Subscription& Subscription::operator=(Subscription&& o) noexcept {
    if (this != &o) {
        release();
        m_bus  = o.m_bus;
        m_type = o.m_type;
        m_id   = o.m_id;
        o.m_bus = nullptr;
    }
    return *this;
}

}  // namespace pk
