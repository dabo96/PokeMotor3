// Input/Input.h — dos fotos del estado (actual/anterior) → polling + flancos.
// Diseño: MotorGrafico_Input.md. Polling para la jugabilidad (cero latencia);
// el bus para consumidores desacoplados (sonido de UI, analítica, rebinding).
#pragma once

#include "Core/Math.h"
#include "Input/KeyCode.h"
#include "Platform/RawEvent.h"

#include <array>
#include <cstddef>
#include <vector>

namespace pk {

class EventBus;

// Eventos emitidos en los flancos (entrega diferida por el bus).
struct KeyPressedEvent  { Key key; };
struct KeyReleasedEvent { Key key; };

class Input {
public:
    void init(EventBus* bus) { m_bus = bus; }

    // Una vez por frame, lo primero tras drenar la ventana. Aplica los eventos,
    // calcula flancos y emite KeyPressed/Released por el bus.
    void update(const std::vector<RawEvent>& events);

    // --- Polling ---
    bool isKeyDown(Key k)      const { return m_current.keys[(std::size_t)k]; }
    bool wasKeyPressed(Key k)  const { return m_current.keys[(std::size_t)k] && !m_previous.keys[(std::size_t)k]; }
    bool wasKeyReleased(Key k) const { return !m_current.keys[(std::size_t)k] && m_previous.keys[(std::size_t)k]; }

    Vec2 mousePosition() const { return m_current.mouse; }
    Vec2 mouseDelta()    const { return m_current.mouse - m_previous.mouse; }

    // Botones de ratón (1=izq, 2=medio, 3=der), igual que SDL.
    bool isMouseDown(int button)      const { return inRange(button) && m_current.mouseBtn[button]; }
    bool wasMousePressed(int button)  const { return inRange(button) &&  m_current.mouseBtn[button] && !m_previous.mouseBtn[button]; }
    bool wasMouseReleased(int button) const { return inRange(button) && !m_current.mouseBtn[button] &&  m_previous.mouseBtn[button]; }

    bool quitRequested() const { return m_quit; }

private:
    static constexpr int kMaxMouseButtons = 8;
    static bool inRange(int b) { return b >= 0 && b < kMaxMouseButtons; }
    struct State {
        std::array<bool, (std::size_t)Key::Count> keys{};
        std::array<bool, kMaxMouseButtons>        mouseBtn{};
        Vec2 mouse{ 0.0f, 0.0f };
    };
    State     m_current;
    State     m_previous;   // la foto del frame anterior → detecta flancos
    EventBus* m_bus  = nullptr;
    bool      m_quit = false;
};

}  // namespace pk
