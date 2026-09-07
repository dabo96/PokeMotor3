// Editor/ToolWindow.h — ventana OS secundaria (p.ej. el editor de tiles).
// Crea su propio SDL_Window y un 2º contexto FluentUI en modo "device compartido +
// swapchain propio" (CreateStandaloneContext(shareFrom) con ownSwapchain): reusa el
// VkDevice/instance/queue y el resource-pool (atlas de fuentes) del editor principal,
// pero posee su surface/swapchain/present. La librería hace acquire/record/submit/
// present internamente (BeginFrame/EndFrame); aquí ya NO hay Vulkan a mano.
#pragma once

#include <SDL3/SDL_events.h>

#include <cstdint>
#include <functional>

struct SDL_Window;

namespace pk {

// windowID del evento SDL (0 si no aplica) — para rutear eventos por ventana.
inline uint32_t sdlEventWindowId(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_MOUSE_MOTION:      return e.motion.windowID;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:   return e.button.windowID;
        case SDL_EVENT_MOUSE_WHEEL:       return e.wheel.windowID;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:            return e.key.windowID;
        case SDL_EVENT_TEXT_INPUT:        return e.text.windowID;
        case SDL_EVENT_TEXT_EDITING:      return e.edit.windowID;
        default:
            if (e.type >= SDL_EVENT_WINDOW_FIRST && e.type <= SDL_EVENT_WINDOW_LAST)
                return e.window.windowID;
            return 0;
    }
}

class ToolWindow {
public:
    // shareFromCtx = FluentUI::UIContext* del editor principal (EditorUI::uiContext()):
    // de ahí se toman prestados device/instance/queue + el resource-pool compartido.
    bool open(void* shareFromCtx, const char* title, int width, int height);
    void close();                              // destruye contexto/ventana
    bool isOpen() const { return m_open; }
    bool wantsClose() const { return m_shouldClose; }
    uint32_t windowId() const { return m_windowId; }
    SDL_Window* window() const { return m_window; }   // para parentar diálogos nativos

    void beginInputFrame();                    // SetCurrentContext + input.Update
    void processEvent(const SDL_Event& e);     // rutea/alimenta el input de su contexto
    // Construye la UI (callback con w,h del cliente) y la presenta en esta ventana.
    void renderFrame(float dt, const std::function<void(int w, int h)>& buildUI);

private:
    SDL_Window* m_window   = nullptr;
    uint32_t    m_windowId = 0;
    void*       m_uictx    = nullptr;     // FluentUI::UIContext* (propio de esta ventana)
    void*       m_backend  = nullptr;     // FluentUI::RenderBackend* (propio)

    bool m_open        = false;
    bool m_shouldClose = false;
};

}  // namespace pk
