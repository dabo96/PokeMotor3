// Core/Log.h — diagnóstico: log con niveles.
// Diseño: MotorGrafico_Core.md (Diagnóstico). Cimiento puro, sin dependencias
// de Vulkan ni ventana.
#pragma once

#include <string>
#include <vector>

namespace pk {

enum class LogLevel { Trace, Info, Warn, Error };

// printf-style. La implementación vive en Log.cpp. Además de imprimir a stdout/stderr,
// retiene el mensaje en un buffer en memoria que la consola del editor muestra.
void logMessage(LogLevel lvl, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

// Una línea del log retenida (para la consola del editor).
struct LogEntry { LogLevel level; std::string text; };

// Copia (thread-safe) las entradas recientes del log para mostrarlas en la UI.
void logSnapshot(std::vector<LogEntry>& out);

// Errores y avisos acumulados desde el arranque. Es un contador, no una copia del buffer:
// la UI lo consulta cada frame (p.ej. el aviso de la consola plegada del editor) sin pagar
// el snapshot completo.
struct LogCounts { unsigned warn = 0, error = 0; };
LogCounts logCounts();

}  // namespace pk

#define LOG_TRACE(...) ::pk::logMessage(::pk::LogLevel::Trace, __VA_ARGS__)
#define LOG_INFO(...)  ::pk::logMessage(::pk::LogLevel::Info,  __VA_ARGS__)
#define LOG_WARN(...)  ::pk::logMessage(::pk::LogLevel::Warn,  __VA_ARGS__)
#define LOG_ERROR(...) ::pk::logMessage(::pk::LogLevel::Error, __VA_ARGS__)
