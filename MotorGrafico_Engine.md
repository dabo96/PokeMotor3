# Motor Pokémon — Application / Engine, bucle principal y tiempo

> Pieza fundacional: la raíz que posee todos los subsistemas, gestiona el orden
> de arranque/apagado, y corre el bucle principal con timestep fijo y variable.

**Estado:** borrador de diseño · **Posición:** la cima del grafo de dependencias (lo posee todo)

---

## Qué es

El módulo raíz. Tres responsabilidades:

1. **Poseer y cablear los subsistemas** (Window, VulkanContext, Renderer, Input,
   EventBus, AssetManager, Scene, GameStack, EditorUI...).
2. **Gestionar el orden de arranque y apagado** (init en orden de dependencias,
   shutdown en orden inverso).
3. **Correr el bucle principal** con timestep fijo (física/lógica determinista) y
   variable (animación/cámara), más el render interpolado.

Mantiene la separación **Engine (framework) / Game (contenido)**: el Engine corre
el bucle genérico; el Game (la pila de modos) es lo que va encima. El Engine no
sabe de Pokémon.

---

## El bucle: timestep fijo + variable

Patrón "Fix Your Timestep": acumular tiempo real, correr la sim determinista en
pasos fijos, lo demás una vez por frame, render interpolado.

```cpp
// Engine/Engine.h
class Engine {
public:
    void run();
private:
    void init();                    // arranque en orden de dependencias
    void shutdown();                // apagado en orden inverso
    void fixedUpdate(float dt);     // física, lógica determinista
    void variableUpdate(float dt);  // animación, cámara
    void render(float alpha);       // interpolado

    Window        m_window;
    VulkanContext m_vulkan;
    Renderer      m_renderer;
    EventBus      m_bus;
    Input         m_input;
    AssetManager  m_assets;
    Scene         m_scene;
    GameStack     m_game;
    bool          m_running = true;
};
```

```cpp
void Engine::run() {
    init();
    const float FIXED_DT = 1.0f / 60.0f;   // paso fijo de la sim
    float  accumulator = 0.0f;
    double current = Clock::now();

    while (m_running) {
        double newTime = Clock::now();
        float  frameTime = float(newTime - current);
        current = newTime;
        frameTime = std::min(frameTime, 0.25f);   // anti spiral-of-death
        accumulator += frameTime;

        m_window.pollEvents();        // 1. eventos del SO
        m_input.update();             // 2. snapshots / flancos

        while (accumulator >= FIXED_DT) {
            fixedUpdate(FIXED_DT);    // 3. física, lógica determinista (N veces)
            accumulator -= FIXED_DT;
        }

        float alpha = accumulator / FIXED_DT;
        variableUpdate(frameTime);    // 4. animación, cámara
        m_bus.dispatch();             // 5. entrega los eventos del frame
        render(alpha);                // 6. interpolado para suavidad
    }
    shutdown();
}
```

### Por qué este bucle amarra todo lo diseñado

- **`m_input.update()`** al inicio → el subsistema de Input, con su orden correcto.
- **Paso fijo** → determinismo del combate (el RNG con semilla explícita del
  `BattleState` depende de pasos reproducibles).
- **`m_bus.dispatch()`** antes del render → exactamente donde lo situamos en el
  diseño del bus.
- **`render(alpha)`** → interpola entre los dos últimos estados fijos para que el
  movimiento se vea suave aunque la sim corra a 60 pasos/s.

---

## Orden de arranque y apagado

Los subsistemas dependen unos de otros: el orden importa.

```cpp
void Engine::init() {
    m_window.init();                  // 1
    m_vulkan.init(m_window);          // 2 (necesita surface de la ventana)
    m_renderer.init(m_vulkan);        // 3
    m_input.init(m_window, m_bus);    // 4
    m_assets.init(m_vulkan);          // 5
    // game systems...                // 6
}
// shutdown() hace lo inverso: game → assets → input → renderer → vulkan → window
```

Construir en orden de dependencias (forward), destruir en orden inverso. Es la
contraparte del "orden de init de Vulkan" del documento de arquitectura general.

---

## Tiempo

- El reloj de alta resolución vive en Core (`Time.h`).
- El **acumulador y el paso fijo** viven aquí, en el bucle.
- Timers de gameplay (cooldowns, etc.) se construyen sobre el `delta` que el bucle
  reparte.

---

## Estructura de carpetas

```
Engine/
├── Engine.h          # raíz: posee subsistemas, corre el bucle
├── Application.h      # punto de entrada (main), config de arranque
└── (usa Core/Time.h para el reloj)
```

---

## Decisiones aún abiertas (para revisitar)

- **Interfaz de subsistema** (init/shutdown común) vs construcción explícita
  ordenada: la explícita es más simple y clara al inicio.
- **Frecuencia del paso fijo** (60 Hz): ajustable; afecta a la sensación de la
  física y el combate.
- **Modo headless** (sin ventana, para tests): aislar el bucle del render lo
  facilita; útil para tests de lógica/combate.
- **Pausa / time scale** (cámara lenta, pausar el mundo en menús): un multiplicador
  sobre `frameTime`, fácil de añadir.
```