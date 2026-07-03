// Input/KeyCode.h — enum de teclas físicas, agnóstico de SDL.
// Diseño: MotorGrafico_Input.md. El mapeo desde scancodes de SDL vive en
// Platform/Window.cpp, así el resto del motor no incluye SDL.
#pragma once

namespace pk {

enum class Key {
    Unknown = 0,
    // Letras (contiguas A..Z)
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    // Dígitos (contiguos Num1..Num9; Num0 aparte)
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    // Especiales
    Space, Enter, Escape, Tab, Backspace, Delete,
    Left, Right, Up, Down,
    LShift, RShift, LCtrl, RCtrl, LAlt, RAlt,
    // Función (contiguas F1..F12)
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Count
};

}  // namespace pk
