// Input/Input.cpp — aplicación de eventos + detección de flancos.
#include "Input/Input.h"

#include "Core/EventBus.h"

namespace pk {

void Input::update(const std::vector<RawEvent>& events) {
    m_previous = m_current;  // 1) el actual pasa a ser pasado

    for (const RawEvent& e : events) {  // 2) aplica los eventos crudos
        switch (e.type) {
            case RawEvent::KeyDown:
                if (e.key != Key::Unknown) m_current.keys[(std::size_t)e.key] = true;
                break;
            case RawEvent::KeyUp:
                if (e.key != Key::Unknown) m_current.keys[(std::size_t)e.key] = false;
                break;
            case RawEvent::MouseMove:
                m_current.mouse = e.position;
                break;
            case RawEvent::MouseButtonDown:
                if (e.mouseButton >= 0 && e.mouseButton < 8) m_current.mouseBtn[e.mouseButton] = true;
                m_current.mouse = e.position;
                break;
            case RawEvent::MouseButtonUp:
                if (e.mouseButton >= 0 && e.mouseButton < 8) m_current.mouseBtn[e.mouseButton] = false;
                m_current.mouse = e.position;
                break;
            case RawEvent::Quit:
                m_quit = true;
                break;
            default:
                break;
        }
    }

    if (m_bus) {  // 3) emite solo en los flancos
        for (std::size_t i = 0; i < (std::size_t)Key::Count; ++i) {
            if (m_current.keys[i] && !m_previous.keys[i])
                m_bus->queue(KeyPressedEvent{ (Key)i });
            else if (!m_current.keys[i] && m_previous.keys[i])
                m_bus->queue(KeyReleasedEvent{ (Key)i });
        }
    }
}

}  // namespace pk
