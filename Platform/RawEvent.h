// Platform/RawEvent.h — evento crudo de ventana, traducido desde SDL.
// Diseño: MotorGrafico_Input.md (Window::drainEvents). Desacopla Input de SDL:
// la ventana traduce los SDL_Event a esta forma neutra.
#pragma once

#include "Core/Math.h"
#include "Input/KeyCode.h"

namespace pk {

struct RawEvent {
    enum Type {
        None,
        Quit,
        KeyDown,
        KeyUp,
        MouseMove,
        MouseButtonDown,
        MouseButtonUp,
        MouseWheel
    } type = None;

    Key  key         = Key::Unknown;  // KeyDown / KeyUp
    int  mouseButton = 0;             // MouseButton* (1=izq, 2=medio, 3=der)
    Vec2 position{ 0.0f, 0.0f };       // MouseMove / MouseButton*
    Vec2 wheel{ 0.0f, 0.0f };          // MouseWheel
};

}  // namespace pk
