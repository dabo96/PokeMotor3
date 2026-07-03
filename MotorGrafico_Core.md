# Motor Pokémon — Módulo Core

> Pieza fundacional. Utilidades base que no dependen de nada y de las que todo
> depende. Define el vocabulario del motor. Backend: C++ (+ GLM).

**Estado:** borrador de diseño · **Posición:** fondo del grafo de dependencias

---

## Qué es y por qué es la base

El `Core` es distinto a todos los demás subsistemas. Tres razones lo hacen el cimiento:

1. **Está en el fondo del grafo de dependencias.** No depende de ningún otro
   módulo — ni de Vulkan, ni de la ventana. Es C++ puro (más, como mucho, GLM).
   Pero todos dependen de él.
2. **Define el vocabulario del motor.** Un `Vec3`, una `Mat4`, un `Handle`, un
   `StringID` viajan por cada archivo. El resto del motor está escrito en el
   idioma que define el Core. Por eso un error aquí se propaga a todos lados.
3. **No se actualiza.** No tiene `update()` en el bucle. No es una fase del frame:
   es infraestructura pasiva, usada continuamente. Si escribes `core.update(dt)`,
   algo se coló que no debería estar ahí.

---

## Contenido

```
┌───────────────────────────────────────────────────────────┐
│ Core   utilidades base que no dependen de nada más         │
│  ┌────────────┐ ┌────────────┐ ┌────────────────┐          │
│  │Diagnóstico │ │Matemáticas │ │ Memoria        │          │
│  │log + assert│ │vectores GLM│ │ arena/pool/stack│         │
│  └────────────┘ └────────────┘ └────────────────┘          │
│  ┌──────────────────┐ ┌───────────────────────┐            │
│  │ Tiempo           │ │ Tipos y handles       │            │
│  │ reloj·delta·timer│ │ ints·StringID·Handle  │            │
│  └──────────────────┘ └───────────────────────┘            │
└───────────────────────────────────────────────────────────┘
```

---

## Diagnóstico: log y assert

Lo primero que querrás, porque sin esto depurar Vulkan es un infierno.

```cpp
// Core/Log.h
enum class LogLevel { Trace, Info, Warn, Error };
void logMessage(LogLevel lvl, const char* fmt, ...);

#define LOG_INFO(...)  logMessage(LogLevel::Info,  __VA_ARGS__)
#define LOG_WARN(...)  logMessage(LogLevel::Warn,  __VA_ARGS__)
#define LOG_ERROR(...) logMessage(LogLevel::Error, __VA_ARGS__)
```

```cpp
// Core/Assert.h
#ifdef DEBUG
  #define ASSERT(cond, msg) \
      if (!(cond)) { LOG_ERROR("Assert: %s", msg); std::abort(); }
#else
  #define ASSERT(cond, msg) ((void)0)   // se evapora en release
#endif
```

El `ASSERT` desaparece por completo en release (no cuesta ni un ciclo), pero en
debug caza tus errores justo donde ocurren. Llénalo de asserts sin miedo.

---

## Matemáticas: envolver GLM

No se escribe desde cero — se envuelve GLM y se le dan nombres propios al motor.

```cpp
// Core/Math.h
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Mat4 = glm::mat4;
using Quat = glm::quat;
```

Propósito de diseño: si mañana cambias GLM por otra librería, solo tocas este
archivo. El resto del motor habla de `Vec3`, no de `glm::vec3`.

---

## Tipos y handles

Un `Handle` es un ID ligero y tipado. De aquí salen `MeshHandle`, `MaterialHandle`,
etc., que se usan en todo el motor.

```cpp
// Core/Handle.h
template <typename T>
struct Handle {
    uint32_t index      = 0;
    uint32_t generation = 0;   // detecta handles colgados
    bool valid() const { return generation != 0; }
};

using MeshHandle     = Handle<struct MeshTag>;
using MaterialHandle = Handle<struct MaterialTag>;
```

- **`generation`**: cuando un manager libera un slot y lo reutiliza, incrementa el
  contador. Un `Handle` viejo tendrá otra `generation` → detectas un
  use-after-free en vez de leer basura.
- **`struct MeshTag`**: hace que el compilador no deje pasar un `MeshHandle` donde
  se espera un `MaterialHandle`, aunque por dentro ambos sean dos enteros.

El `StringID` compara strings por hash, no por contenido:

```cpp
// Core/StringID.h
struct StringID {
    uint32_t hash;
    constexpr StringID(const char* s) : hash(fnv1a(s)) {}
    bool operator==(StringID o) const { return hash == o.hash; }
};
```

Te deja usar claves legibles como `"player_idle"` con la velocidad de un `int`.
Clave para buscar assets, nombrar eventos, y referenciar contenido en la
`Database` del juego (el flyweight de Pokémon se apoya en esto).

---

## Memoria y tiempo

- **Memoria:** allocators (arena, pool, stack) sobre los que construir sin pedir
  al SO en caliente. Empezar simple (el allocator por defecto) y meter allocators
  custom solo donde el perfilado lo pida.
- **Tiempo:** reloj de alta resolución, `delta` del frame, timers. Lo consume el
  bucle principal para el fixed/variable timestep.

---

## Nota práctica

El Core es casi todo cabeceras (`.h`) con unos pocos `.cpp`, y **no enlaza contra
Vulkan ni contra la ventana**. Mantenerlo puro es la prueba de fuego de que sigue
siendo el cimiento. El día que el Core necesite incluir algo del renderer, sabrás
que algo está mal en el diseño.

---

## Estructura de carpetas

```
Core/
├── Log.h
├── Assert.h
├── Math.h        # aliases de GLM
├── Handle.h      # Handle<T>, MeshHandle, MaterialHandle...
├── StringID.h    # hash-based
├── Memory.h      # allocators (arena/pool/stack)
└── Time.h        # reloj, delta, timers
```

---

## Decisiones aún abiertas (para revisitar)

- **Allocators custom:** introducir solo cuando el perfilado muestre presión de
  memoria; el allocator por defecto basta al inicio.
- **Librería de logging:** spdlog vs propia. spdlog es cómodo; una propia mínima
  evita una dependencia. Cualquiera vale.
- **Hash de StringID:** FNV-1a es simple y suficiente; revisar solo si hay
  colisiones reales (improbable).
```