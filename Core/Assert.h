// Core/Assert.h — assert que se evapora en release.
// Diseño: MotorGrafico_Core.md (Diagnóstico). En debug caza el error donde
// ocurre; en release no cuesta ni un ciclo.
#pragma once

#include "Core/Log.h"
#include <cstdlib>

#if defined(DEBUG) || !defined(NDEBUG)
    #define ASSERT(cond, msg)                                                  \
        do {                                                                   \
            if (!(cond)) {                                                     \
                ::pk::logMessage(::pk::LogLevel::Error,                        \
                    "Assert fallido (%s:%d): %s | %s",                         \
                    __FILE__, __LINE__, #cond, (msg));                         \
                std::abort();                                                  \
            }                                                                  \
        } while (0)
#else
    #define ASSERT(cond, msg) ((void)0)
#endif
