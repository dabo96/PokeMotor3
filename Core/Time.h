// Core/Time.h — reloj de alta resolución, delta y timers.
// Diseño: MotorGrafico_Core.md (Memoria y tiempo). Lo consume el bucle
// principal para el fixed/variable timestep.
#pragma once

#include <chrono>

namespace pk {

// Reloj del frame. tick() devuelve los segundos transcurridos desde el tick
// anterior; elapsed() los segundos desde la creación.
class Clock {
public:
    Clock() : m_start(now()), m_last(m_start) {}

    float tick() {
        TP t = now();
        float dt = std::chrono::duration<float>(t - m_last).count();
        m_last = t;
        return dt;
    }

    float elapsed() const {
        return std::chrono::duration<float>(now() - m_start).count();
    }

private:
    using TP = std::chrono::high_resolution_clock::time_point;
    static TP now() { return std::chrono::high_resolution_clock::now(); }
    TP m_start;
    TP m_last;
};

// Temporizador de cuenta atrás. tick(dt) devuelve true el frame en que termina.
class Timer {
public:
    Timer() = default;
    explicit Timer(float duration) : m_remaining(duration), m_duration(duration) {}

    void set(float duration) { m_duration = duration; m_remaining = duration; }
    void reset() { m_remaining = m_duration; }

    bool tick(float dt) {
        if (m_remaining <= 0.0f) return false;
        m_remaining -= dt;
        return m_remaining <= 0.0f;
    }

    bool finished() const { return m_remaining <= 0.0f; }
    float remaining() const { return m_remaining; }

private:
    float m_remaining = 0.0f;
    float m_duration  = 0.0f;
};

}  // namespace pk
