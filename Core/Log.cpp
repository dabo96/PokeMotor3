// Core/Log.cpp — implementación del log: imprime a la terminal y retiene un buffer
// en memoria (para la consola del editor).
#include "Core/Log.h"

#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

namespace pk {

static const char* levelStr(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

namespace {
std::mutex            g_logMutex;
std::deque<LogEntry>  g_logEntries;       // las últimas líneas (para la UI)
constexpr std::size_t kMaxLog = 500;
}  // namespace

void logMessage(LogLevel lvl, const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Warn/Error a stderr; el resto a stdout. Sin colores por ahora (portable).
    std::FILE* out = (lvl == LogLevel::Error || lvl == LogLevel::Warn) ? stderr : stdout;
    std::fprintf(out, "[%s] %s\n", levelStr(lvl), buf);
    std::fflush(out);

    // Retiene en el buffer (ring de kMaxLog) para la consola del editor.
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_logEntries.push_back({ lvl, buf });
    while (g_logEntries.size() > kMaxLog) g_logEntries.pop_front();
}

void logSnapshot(std::vector<LogEntry>& out) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    out.assign(g_logEntries.begin(), g_logEntries.end());
}

}  // namespace pk
