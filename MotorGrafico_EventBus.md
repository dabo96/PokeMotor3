# Motor Pokémon — Bus de eventos

> Pieza fundacional. El "glue" que deja a los subsistemas comunicarse sin
> conocerse. Backend: C++ (depende solo de Core).

**Estado:** borrador de diseño · **Posición:** columna vertebral, sobre Core

---

## Qué resuelve

En vez de que la física llame directamente a audio cuando hay un choque, la
física **emite** un evento al bus, y quien quiera reaccionar lo **escucha**.
Ninguno de los dos sabe que el otro existe.

```
   ┌────────┐  emite CollisionEvent  ┌──────────┐  entrega   ┌──────────┐
   │ Física │ ─────────────────────► │ EventBus │ ─────────► │ Audio    │
   └────────┘                        └────┬─────┘            └──────────┘
                                          │ entrega          ┌──────────┐
                                          └────────────────► │ Gameplay │
                                                             └──────────┘
   La física no conoce a Audio ni a Gameplay — solo emite; el bus entrega.
```

Sin el bus, esas flechas serían punteros directos: `Física` incluiría `Audio` y
`Gameplay`, y en seis meses tendrías un grafo donde tocar un módulo recompila
medio motor. El bus convierte ese grafo en una estrella.

---

## Decisión 1: identidad por tipo

Los eventos son `struct`s planos, y el bus reparte por el **tipo de C++**
(`std::type_index`). Ventaja: seguridad en tiempo de compilación — no puedes
suscribirte a un evento que no existe ni equivocarte en el nombre, porque el
nombre *es* el tipo.

(El `StringID` sigue siendo útil para nombres de depuración, serialización o
eventos que cruzan a scripting; pero para el núcleo del bus, los tipos ganan.)

---

## Decisión 2: inmediato vs diferido

- `emit(evento)` llama a los suscriptores **ahora mismo**, en la misma pila. Útil
  cuando algo tiene que pasar ya; cuidado si un handler muta lo que se recorre.
- `queue(evento)` + `dispatch()` **difiere** la entrega a un punto fijo del frame.
  Por defecto para eventos entre subsistemas: orden determinista, nadie muta
  estado en mitad de la iteración de otro.

---

## El código

```cpp
// Core/EventBus.h
#include <functional>
#include <typeindex>
#include <unordered_map>
#include <vector>
#include <algorithm>

class EventBus {
public:
    // Suscribirse a un tipo de evento. Devuelve un token RAII.
    template <typename E>
    [[nodiscard]] Subscription subscribe(std::function<void(const E&)> handler) {
        auto type   = std::type_index(typeid(E));
        uint64_t id = m_nextId++;
        m_handlers[type].push_back({ id,
            [h = std::move(handler)](const void* e) {
                h(*static_cast<const E*>(e));   // recupera el tipo concreto
            }});
        return Subscription{ this, type, id };
    }

    // Entrega inmediata.
    template <typename E>
    void emit(const E& event) {
        auto it = m_handlers.find(std::type_index(typeid(E)));
        if (it == m_handlers.end()) return;
        for (auto& entry : it->second) entry.fn(&event);
    }

    // Entrega diferida: encola una copia; se reparte en dispatch().
    template <typename E>
    void queue(const E& event) {
        m_queue.push_back([this, event] { emit(event); });
    }

    // Vacía la cola una vez por frame.
    void dispatch() {
        auto pending = std::move(m_queue);   // swap: un handler puede encolar
        m_queue.clear();                     // sin invalidar este bucle
        for (auto& deliver : pending) deliver();
    }

private:
    struct HandlerEntry {
        uint64_t id;
        std::function<void(const void*)> fn;
    };
    std::unordered_map<std::type_index, std::vector<HandlerEntry>> m_handlers;
    std::vector<std::function<void()>> m_queue;
    uint64_t m_nextId = 1;

    void unsubscribe(std::type_index type, uint64_t id) {
        auto it = m_handlers.find(type);
        if (it == m_handlers.end()) return;
        auto& v = it->second;
        v.erase(std::remove_if(v.begin(), v.end(),
            [id](auto& h){ return h.id == id; }), v.end());
    }
    friend class Subscription;
};
```

El token RAII evita el bug más peligroso de cualquier bus: si un subsistema se
destruye pero su callback sigue registrado, el bus llamará a memoria liberada. El
token se desuscribe solo al morir.

```cpp
// Core/Subscription.h
class Subscription {
public:
    Subscription() = default;
    Subscription(EventBus* bus, std::type_index t, uint64_t id)
        : m_bus(bus), m_type(t), m_id(id) {}
    ~Subscription() { release(); }

    // No copiable, sí movible: token único.
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    Subscription(Subscription&& o) noexcept { *this = std::move(o); }
    Subscription& operator=(Subscription&& o) noexcept {
        release();
        m_bus = o.m_bus; m_type = o.m_type; m_id = o.m_id;
        o.m_bus = nullptr;
        return *this;
    }

    void release() {
        if (m_bus) { m_bus->unsubscribe(m_type, m_id); m_bus = nullptr; }
    }
private:
    EventBus*       m_bus = nullptr;
    std::type_index m_type = std::type_index(typeid(void));
    uint64_t        m_id = 0;
};
```

Detalle de compilación: como `release()` llama a `EventBus::unsubscribe`, su
cuerpo debe definirse donde `EventBus` ya está completo (forward-declaration de
`EventBus` arriba, cuerpo donde ambos se conocen).

---

## Uso

```cpp
struct CollisionEvent { EntityId a, b; Vec3 point; };

// En Audio::init — guarda la suscripción como MIEMBRO de la clase:
m_sub = bus.subscribe<CollisionEvent>([this](const CollisionEvent& e) {
    playSound("hit", e.point);
});

// En la física, al detectar el choque:
bus.queue(CollisionEvent{ a, b, contactPoint });

// Una vez por frame, en el bucle (después de física):
bus.dispatch();
```

---

## Relación entre clases

```
EventBus  ──crea──►  Subscription
EventBus  ◄─desuscribe─  Subscription
```

La doble flecha es deliberada: el bus crea el token, y el token llama de vuelta
al bus para limpiarse. Es la única dependencia circular permitida aquí.

---

## Notas de diseño

- **Dónde va `dispatch()`:** una vez por frame, después de física/lógica, antes
  del render. Así todo lo emitido en el frame se reparte de golpe, en orden.
- **Uno o varios buses:** empezar con uno solo, propiedad de la `Application`,
  pasado por referencia. Dividir en canales (gameplay vs motor) solo si el
  perfilado lo pide.
- **Hilos:** diseño de un solo hilo, que es lo que necesitas mientras el bucle
  sea secuencial. Si mueves física o carga de assets a otro hilo, añades un
  candado a la cola o una cola lock-free. Problema futuro.

---

## Estructura de carpetas

```
Core/
├── EventBus.h
└── Subscription.h
```

---

## Decisiones aún abiertas (para revisitar)

- **Canales separados:** dividir el bus solo si hay contención o quieres
  aislamiento entre dominios.
- **Thread-safety:** añadir candado/cola lock-free solo al ir multihilo.
- **Eventos con `StringID`:** capa opcional sobre el bus tipado, para scripting o
  serialización, si hace falta.
```