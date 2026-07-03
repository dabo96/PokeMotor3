# Motor Pokémon — Subsistema de Input

> Pieza fundacional. El primer subsistema que "tickea" en el bucle. Backend:
> C++ (depende de Window y de Core; emite por el Event Bus).

**Estado:** borrador de diseño

---

## La decisión central: dos formas de leer a la vez

Hay input *continuo* — mantener W para avanzar — donde cada frame preguntas "¿está
W pulsada ahora?". Y hay input *discreto* — pulsar espacio para saltar — donde
importa el instante del cambio: "¿se acaba de pulsar este frame?". Una sola tecla
genera ambos tipos de información, y el motor debe responder a los dos.

El truco que lo hace posible: guardar **dos fotos del estado**, la de este frame
y la del anterior.

---

## Detección de flancos

Imagina la señal de una tecla a lo largo de seis frames: suelta en F1–F2, pulsada
en F3–F4–F5, suelta otra vez en F6.

- `isDown` es verdadero **mientras** está pulsada: F3, F4, F5.
- `wasPressed` es verdadero **solo en F3** (flanco de subida): abajo ahora, arriba
  el frame anterior.
- `wasReleased` es verdadero **solo en F6** (flanco de bajada).

La fórmula: `wasPressed = down(ahora) && !down(frame_anterior)`. Sin la foto del
frame anterior no hay forma de saber que hubo un *cambio*, solo el estado.

---

## El código

```cpp
// Input/KeyCode.h  (podría vivir en Platform)
enum class Key { Space, W, A, S, D, Escape, /* ... */ Count };

// Input/Input.h
class Input {
public:
    void init(Window& window, EventBus& bus);
    void update();   // una vez por frame, lo primero tras los eventos del SO

    // --- Polling: estado consultable ---
    bool isKeyDown(Key k)     const;  // ¿pulsada ahora?
    bool wasKeyPressed(Key k)  const; // ¿flanco de subida este frame?
    bool wasKeyReleased(Key k) const; // ¿flanco de bajada este frame?

    Vec2 mousePosition() const;
    Vec2 mouseDelta()    const;       // movimiento desde el frame anterior

private:
    struct State {
        std::array<bool, (size_t)Key::Count> keys{};
        Vec2 mouse{};
    };
    State m_current;
    State m_previous;   // la foto del frame anterior → detecta flancos

    Window*   m_window = nullptr;
    EventBus* m_bus    = nullptr;
};
```

```cpp
void Input::update() {
    m_previous = m_current;                          // 1. el actual pasa a ser pasado

    for (const RawEvent& e : m_window->drainEvents()) {  // 2. drena eventos crudos
        switch (e.type) {
            case RawEvent::KeyDown:  m_current.keys[(size_t)e.key] = true;  break;
            case RawEvent::KeyUp:    m_current.keys[(size_t)e.key] = false; break;
            case RawEvent::MouseMove: m_current.mouse = e.position;         break;
        }
    }

    for (size_t i = 0; i < (size_t)Key::Count; ++i) {    // 3. emite solo en flancos
        if ( m_current.keys[i] && !m_previous.keys[i])
            m_bus->queue(KeyPressedEvent{ (Key)i });
        else if (!m_current.keys[i] && m_previous.keys[i])
            m_bus->queue(KeyReleasedEvent{ (Key)i });
    }
}
```

```cpp
bool Input::isKeyDown(Key k) const { return m_current.keys[(size_t)k]; }

bool Input::wasKeyPressed(Key k) const {
    return m_current.keys[(size_t)k] && !m_previous.keys[(size_t)k];
}
bool Input::wasKeyReleased(Key k) const {
    return !m_current.keys[(size_t)k] && m_previous.keys[(size_t)k];
}
Vec2 Input::mouseDelta() const { return m_current.mouse - m_previous.mouse; }
```

---

## Polling vs bus: cuál usar para qué

Input ofrece dos canales de salida, y no son intercambiables:

- **Polling** para input que la lógica necesita **este mismo frame** (movimiento,
  salto, disparo): la lógica corre justo tras `Input::update()` y consulta con
  cero latencia.
- **Bus** para consumidores **desacoplados** a los que un frame de diferencia no
  importa (sonido de UI al teclear, analítica, pantalla de rebinding).

La razón es el orden del frame: `dispatch()` corre después de la física, así que
un evento encolado al principio se entrega más tarde. **Regla: polling para la
jugabilidad, bus para lo periférico.**

---

## La capa de acciones: tecla vs intención

Que la lógica pregunte por teclas físicas (`Key::Space`) acopla el gameplay al
hardware. La solución es una capa de **acciones** encima del input crudo: dos
inputs físicos distintos (Espacio y botón A del mando) apuntan a una tabla de
bindings que los traduce a una sola acción semántica ("Saltar"), que es lo único
que el juego conoce. Rebindear es editar la tabla; la lógica no cambia.

```cpp
// Input/ActionMap.h
enum class Action { Jump, Fire, MoveLeft, MoveRight, /* ... */ };

class ActionMap {
public:
    void bind(Action a, Key k);   // un Action puede tener varias teclas/botones

    bool isActive(const Input& in, Action a) const;     // ¿activa ahora?
    bool wasTriggered(const Input& in, Action a) const; // ¿flanco este frame?

private:
    std::unordered_map<Action, std::vector<Key>> m_bindings;
};

bool ActionMap::wasTriggered(const Input& in, Action a) const {
    auto it = m_bindings.find(a);
    if (it == m_bindings.end()) return false;
    for (Key k : it->second)
        if (in.wasKeyPressed(k)) return true;   // cualquiera de las teclas vale
    return false;
}
```

El juego queda limpio, sin mencionar teclas físicas:

```cpp
if (actions.wasTriggered(input, Action::Jump))
    player.jump();
```

---

## Notas de diseño

- **Orden en el bucle:** eventos del SO → `Input::update()` → lógica. Nunca al
  revés: la lógica reacciona a un input ya fresco.
- **Mando:** entra en el mismo modelo — añades su estado (ejes, botones) al
  snapshot y lo lees con la librería de ventana (SDL). No leas HID a mano.
- **Contextos:** el siguiente nivel del `ActionMap` — un mapa para "en menú" y
  otro para "en juego", cambiando el activo según el estado. La misma tecla hace
  cosas distintas según dónde estés.

---

## Estructura de carpetas

```
Input/
├── KeyCode.h     # enum Key (podría vivir en Platform)
├── Input.h       # snapshots actual/anterior, flancos, polling
└── ActionMap.h   # capa semántica, rebindable
```

---

## Decisiones aún abiertas (para revisitar)

- **Contextos de acciones:** añadir el cambio de mapa activo cuando haya menús.
- **Mando:** integrar ejes/botones al snapshot cuando se quiera soporte.
- **Rebinding en runtime + persistencia:** guardar los bindings del jugador junto
  al save, cuando exista la UI de opciones.
```