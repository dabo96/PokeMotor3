// Platform/Window.cpp — implementación SDL3 de la ventana.
#include "Platform/Window.h"

#include "Core/Log.h"

#include <SDL3/SDL.h>

namespace pk {

// Traduce un scancode físico de SDL a nuestra Key (agnóstica de SDL).
static Key keyFromScancode(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
        return static_cast<Key>(static_cast<int>(Key::A) + (sc - SDL_SCANCODE_A));
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
        return static_cast<Key>(static_cast<int>(Key::Num1) + (sc - SDL_SCANCODE_1));
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12)
        return static_cast<Key>(static_cast<int>(Key::F1) + (sc - SDL_SCANCODE_F1));
    switch (sc) {
        case SDL_SCANCODE_0:         return Key::Num0;
        case SDL_SCANCODE_SPACE:     return Key::Space;
        case SDL_SCANCODE_RETURN:    return Key::Enter;
        case SDL_SCANCODE_ESCAPE:    return Key::Escape;
        case SDL_SCANCODE_TAB:       return Key::Tab;
        case SDL_SCANCODE_BACKSPACE: return Key::Backspace;
        case SDL_SCANCODE_DELETE:    return Key::Delete;
        case SDL_SCANCODE_LEFT:      return Key::Left;
        case SDL_SCANCODE_RIGHT:     return Key::Right;
        case SDL_SCANCODE_UP:        return Key::Up;
        case SDL_SCANCODE_DOWN:      return Key::Down;
        case SDL_SCANCODE_LSHIFT:    return Key::LShift;
        case SDL_SCANCODE_RSHIFT:    return Key::RShift;
        case SDL_SCANCODE_LCTRL:     return Key::LCtrl;
        case SDL_SCANCODE_RCTRL:     return Key::RCtrl;
        case SDL_SCANCODE_LALT:      return Key::LAlt;
        case SDL_SCANCODE_RALT:      return Key::RAlt;
        default:                     return Key::Unknown;
    }
}

bool Window::create(const char* title, int width, int height) {
    // SDL_WINDOW_VULKAN deja la ventana lista para crear la superficie en Fase 1.
    // MAXIMIZED = ocupa la resolución nativa del monitor pero conserva titlebar y
    // botón de cerrar (no es fullscreen sin bordes). RESIZABLE permite redimensionar
    // (el renderer recrea la swapchain ante el cambio de tamaño). El width/height
    // pedido es el tamaño "restaurado"; consultamos el real tras maximizar.
    m_window = SDL_CreateWindow(title, width, height,
                               SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);
    if (!m_window) {
        LOG_ERROR("SDL_CreateWindow falló: %s", SDL_GetError());
        return false;
    }
    SDL_GetWindowSize(m_window, &m_width, &m_height);
    m_shouldClose = false;
    return true;
}

void Window::destroy() {
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
}

std::vector<RawEvent> Window::drainEvents() {
    std::vector<RawEvent> out;
    m_rawEvents.clear();
    const SDL_WindowID mainId = SDL_GetWindowID(m_window);
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        m_rawEvents.push_back(ev);   // guarda el evento crudo (para FluentUI / ruteo por ventana)

        // Solo la ventana principal alimenta el input del juego y el tamaño; los
        // eventos de ventanas de herramientas se rutean aparte (no afectan al juego).
        {
            uint32_t wid = 0;
            switch (ev.type) {
                case SDL_EVENT_KEY_DOWN:
                case SDL_EVENT_KEY_UP:            wid = ev.key.windowID;    break;
                case SDL_EVENT_MOUSE_MOTION:      wid = ev.motion.windowID; break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                case SDL_EVENT_MOUSE_BUTTON_UP:   wid = ev.button.windowID; break;
                case SDL_EVENT_MOUSE_WHEEL:       wid = ev.wheel.windowID;  break;
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                case SDL_EVENT_WINDOW_RESIZED:    wid = ev.window.windowID; break;
                default: break;
            }
            if (wid != 0 && wid != mainId) continue;   // evento de otra ventana
        }

        RawEvent re;
        switch (ev.type) {
            case SDL_EVENT_QUIT:
                re.type = RawEvent::Quit;
                m_shouldClose = true;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                // La X de una ventana concreta. Solo la principal termina el motor;
                // las ventanas de herramientas gestionan su propio cierre (sus
                // CLOSE_REQUESTED se rutean por windowID en m_rawEvents). No dependemos
                // del SDL_EVENT_QUIT de "última ventana", que no llega con 2 abiertas.
                if (ev.window.windowID == mainId) {
                    re.type = RawEvent::Quit;
                    m_shouldClose = true;
                    break;
                }
                continue;   // ventana de herramientas: no afecta al motor aquí
            case SDL_EVENT_KEY_DOWN:
                re.type = RawEvent::KeyDown;
                re.key  = keyFromScancode(ev.key.scancode);
                break;
            case SDL_EVENT_KEY_UP:
                re.type = RawEvent::KeyUp;
                re.key  = keyFromScancode(ev.key.scancode);
                break;
            case SDL_EVENT_MOUSE_MOTION:
                re.type     = RawEvent::MouseMove;
                re.position = { ev.motion.x, ev.motion.y };
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                re.type        = RawEvent::MouseButtonDown;
                re.mouseButton = ev.button.button;
                re.position    = { ev.button.x, ev.button.y };
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                re.type        = RawEvent::MouseButtonUp;
                re.mouseButton = ev.button.button;
                re.position    = { ev.button.x, ev.button.y };
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                re.type  = RawEvent::MouseWheel;
                re.wheel = { ev.wheel.x, ev.wheel.y };
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_RESIZED:
                m_width  = ev.window.data1;
                m_height = ev.window.data2;
                continue;  // no es un RawEvent de input
            default:
                continue;
        }
        out.push_back(re);
    }
    return out;
}

}  // namespace pk
