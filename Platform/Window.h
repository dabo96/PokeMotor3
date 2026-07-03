// Platform/Window.h — ventana SDL + drenaje de eventos crudos.
// Diseño: MotorGrafico_IndiceMaestro.md (Platform/Window con SDL). Expone el
// SDL_Window* para que Fase 1 cree la superficie Vulkan; el resto del motor solo
// ve RawEvent.
#pragma once

#include "Platform/RawEvent.h"

#include <SDL3/SDL_events.h>   // SDL_Event (eventos crudos para el editor/FluentUI)
#include <vector>

struct SDL_Window;

namespace pk {

class Window {
public:
    // Asume SDL_Init(SDL_INIT_VIDEO) ya llamado por la Application.
    bool create(const char* title, int width, int height);
    void destroy();

    // Drena los eventos pendientes de SDL y los traduce a RawEvent. Actualiza
    // shouldClose() ante un Quit, y width()/height() ante un resize.
    std::vector<RawEvent> drainEvents();

    // Eventos SDL crudos del último drainEvents() (para alimentar a FluentUI).
    const std::vector<SDL_Event>& rawEvents() const { return m_rawEvents; }

    bool shouldClose() const { return m_shouldClose; }
    int  width()  const { return m_width; }
    int  height() const { return m_height; }

    SDL_Window* sdl() const { return m_window; }  // para Fase 1 (superficie Vulkan)

private:
    SDL_Window*            m_window      = nullptr;
    int                    m_width       = 0;
    int                    m_height      = 0;
    bool                   m_shouldClose = false;
    std::vector<SDL_Event> m_rawEvents;
};

}  // namespace pk
