# Motor Pokémon — Índice maestro y plan de implementación

> Documento puente del diseño al código. Une los ocho documentos de diseño, el
> mapa de dependencias entre piezas, y el orden de implementación recomendado por
> hitos. Léelo de arriba abajo una vez; luego úsalo como checklist.

**Objetivo del motor:** RPG estilo Pokémon en 2D clásico, HD-2D y 3D · PC · Vulkan
**Filosofía:** motor enfocado a un género, simple, sin features de más.

---

## Cómo usar este documento

1. El **mapa de dependencias** te dice por qué el orden es el que es.
2. El **orden de implementación** te dice qué teclear y en qué secuencia.
3. Cada fase termina en un **hito que corre y se ve**. No avances a la siguiente
   sin que la actual funcione.
4. Las piezas marcadas *(aplazado)* son contenido que se monta encima del
   esqueleto — "agregar", no "modificar". No bloquean el arranque.

---

## El corpus de diseño

### Fundamentos (diseñados en las conversaciones; esenciales resumidos aquí)

| Pieza | Qué cubre | Doc |
|---|---|---|
| **Core** | log, assert, math (GLM), `Handle<T>`, `StringID`, tiempo | Arquitectura general |
| **Event Bus** | glue entre subsistemas, suscripción RAII, emit/queue/dispatch | (en discusión) |
| **Input** | snapshots actual/anterior, flancos, `ActionMap` | (en discusión) |

### Arquitectura general

| Doc | Qué cubre |
|---|---|
| `MotorGrafico_DiseñoTecnico.md` | capas, módulos, ciclo de frame, orden de init de Vulkan |

### Mitad de lógica de juego

| Doc | Qué cubre |
|---|---|
| `MotorGrafico_LogicaDeJuego.md` | datos flyweight, pila de modos, combate-intérprete, save, grid, eventos |

### Mitad gráfica (el renderer, en orden)

| Doc | Qué cubre |
|---|---|
| `MotorGrafico_RenderGraph.md` | passes + recursos como DAG, declarar/compilar/ejecutar, barreras auto |
| `MotorGrafico_PassPrincipal.md` | forward shading, pass principal, color HDR, forward→forward+ |
| `MotorGrafico_Materiales.md` | PBR / HD-2D / unlit bajo una interfaz, sets por frecuencia |
| `MotorGrafico_LucesYSombras.md` | buffer de luces, ambiente, CSM, ShadowPass |
| `MotorGrafico_PostProceso.md` | bloom, DoF/tilt-shift, tonemap, patrón fullscreen |
| `MotorGrafico_2DyCamara.md` | pipeline 2D clásico, SpriteBatch, cámara (ortho/persp), controlador |

---

## Mapa de dependencias

Construir de abajo hacia arriba. Cada capa necesita la de debajo.

```
                       ┌──── construir PRIMERO (abajo) ────┐

  Core  (log · math · Handle · StringID · tiempo)        ← no depende de nada
    ▲
  Platform/Window (SDL) ──► Input ──► Main loop (fixed/variable timestep)
    ▲
  VulkanContext  (vk-bootstrap + VMA)
    ▲
  Renderer  (beginFrame/endFrame · sync · frames in flight)
    ▲
  RenderGraph  (declarar · compilar · ejecutar · barreras)
    ▲
    ├──────── camino 2D ────────┐        ├──── camino 3D / HD-2D ────┐
  Camera + Material(unlit)            Camera + Material(PBR/HD2D)
  + SpriteBatch + Sprite2DPass        + Lights + MainPass + ShadowPass + Post
    └──────────────┬─────────────────────────────┬─────────────────┘
                   ▼ (ambos producen draws)       ▼

  Event Bus (glue)
    ▲
  Lógica de juego  (Data · Modes · Battle · Save · Grid · Events)
    ▲
  Application / Game   (cablea lógica + renderer)

                       └──── construir AL FINAL (arriba) ────┘
```

**Relación clave juego ↔ renderer:** la lógica de juego *produce* lo que se
dibuja (DrawItems, sprites) pero el renderer **no depende** de la lógica de
juego. Los une la `Application` en la cima. Por eso puedes construir y probar el
renderer (camino 2D) antes de tener nada de juego.

---

## Orden de implementación por hitos

### Fase 0 — Cimientos

**Construir:** proyecto + CMake + dependencias (SDL, GLM, VMA, vk-bootstrap,
una lib JSON, spdlog opcional). Core: log, assert, aliases de math, `Handle<T>`,
`StringID`, tiempo. Window (SDL) + drenaje de eventos crudos. Bucle principal
con fixed/variable timestep.
**Docs:** Arquitectura general, Core.
**Hito:** se abre una ventana, el bucle corre, el logging funciona. Aún no se
renderiza nada, pero el esqueleto respira.

### Fase 1 — Backend Vulkan mínimo

**Construir:** `VulkanContext` con vk-bootstrap (instance, device, queues,
swapchain) + allocator VMA. Shell del `Renderer` (adquirir imagen, command
buffer, submit, present) con sincronización (frames in flight). Validation layers
ENCENDIDAS.
**Docs:** Arquitectura general (orden de init).
**Hito:** la pantalla se limpia a un color cada frame; luego, un triángulo.
Prueba device, swapchain, submit, sync y present de una.

### Fase 2 — Render graph mínimo

**Construir:** `RenderGraph` (addPass, compile con orden topológico + barreras
del caso simple, execute), handles de recurso, plomería de passes. Empezar
mínimo: un grafo de un pass que limpia/dibuja al swapchain. **Sin** aliasing ni
culling todavía — solo orden + barreras.
**Docs:** Render Graph.
**Hito:** lo mismo de la fase 1, pero ahora pasando por el grafo. Es un hito de
refactor (reenrutar lo que ya funciona), la forma correcta de introducir el grafo.

### Fase 3 — Camino 2D de punta a punta  ★ LA GRAN VALIDACIÓN

**Construir:** `OrthographicCamera`. Material mínimo (familia UnlitSprite + sets
0/1). Carga de texturas (imagen → VkImage vía VMA) + sampler nearest.
`SpriteBatch` + `Sprite2DPass` + target lowRes + pass de upscale. Render de
tilemap (leer un `TileMap`, emitir sprites). Enganchar Input → mover cámara/sprite.
**Docs:** 2D y Cámara, Materiales (parte unlit), Render Graph.
**Hito:** un **tilemap en pantalla con el sprite del jugador que mueves, cámara
siguiéndolo.** Prueba la pila ENTERA (ventana → swapchain → grafo → pass →
material → cámara → input) antes de un solo shader de sombras.

### Fase 4 — Mitad de lógica de juego (esqueleto)

**Construir:** Event Bus. Pila de modos + `OverworldMode` (mover el tilemap +
jugador desde un modo real). Capa de datos (`Database`, cargar especies/
movimientos/objetos de JSON, instancia `Pokemon` flyweight). Movimiento de grid +
colisión + encuentros (pisar hierba). Save/load. Flags + `EventRunner` básico.
**Docs:** Lógica de Juego, Event Bus.
**Hito:** overworld jugable — caminas por la grid, saltas un encuentro (aunque
solo logee "¡apareció un Pokémon salvaje!"), guardas y cargas.

### Fase 5 — Camino 3D / HD-2D

**Construir:** `PerspectiveCamera`. Carga de mallas/modelos (glTF) + pipeline de
geometría. Familia de material PBR + `MainPass` (forward) con el bucle de
iluminación. Luces (`LightBuffer`) + ambiente. `ShadowPass` (CSM). Familia de
material HD-2D (billboards iluminados).
**Docs:** Pass Principal, Materiales, Luces y Sombras.
**Hito:** una escena 3D/HD-2D iluminada con sol + sombras. Empieza el camino
"mejor que Switch".

### Fase 6 — Post-proceso (el look)

**Construir:** plomería de fullscreen pass (ya la tienes del upscale), target
HDR. Tonemap (primero — para mostrar el HDR bien). Bloom (pirámide). DoF/
tilt-shift (el diorama HD-2D).
**Docs:** Post-proceso.
**Hito:** el look HD-2D en pantalla — sprites pixelados iluminados en un mundo 3D
con el desenfoque de maqueta. La visión, hecha imagen.

### Fase 7+ — Sistemas de contenido *(aplazado)*

Se montan encima del esqueleto, en el orden que el juego pida:

- **UI / menús** y **sistema de texto/diálogo** (los más urgentes para un Pokémon)
- **Animación** (esqueletal 3D + hoja de sprites HD-2D)
- **Audio** (música por zona, transiciones, SFX por evento)
- **Reglamento de combate** completo + IA de combate
- **NPCs/entrenadores**, **gestión de mundo/mapas**, **inventario/economía**
- **Progresión** (Pokédex, evoluciones, captura, insignias)
- **Partículas**, **materiales especiales** (agua), **día-noche**

---

## Principios de implementación

- **De abajo hacia arriba**, pero en **rebanadas verticales**: cada fase produce
  un build que corre, no una capa aislada sin probar.
- **Valida con lo simple primero** (fase 3, el 2D) antes del 3D. Si la
  arquitectura aguanta el 2D limpio, el 3D solo añade implementación.
- **Apóyate en librerías** para el boilerplate: vk-bootstrap (init), VMA
  (memoria), SDL (ventana + input), GLM (math), una lib JSON (datos). Tu valor está en
  el diseño, no en reescribir lo resuelto.
- **El juego antes que el reglamento:** overworld jugable (fase 4) antes que las
  reglas profundas de combate (fase 7).
- **YAGNI:** no construyas forward+, sombras de luces locales, networking ni
  scripting Lua hasta que el juego los pida. El diseño ya los deja como extensión.
- **Validation layers siempre encendidas** en debug. En Vulkan son tu red.

---

## Arranque concreto (primera semana)

1. CMake + SDL + GLM + vk-bootstrap + VMA compilando juntos.
2. Ventana abierta + bucle principal (fase 0).
3. `VulkanContext` con vk-bootstrap, swapchain, limpiar pantalla a un color (fase 1).
4. Objetivo de la semana: **un triángulo en pantalla**, a ser posible ya por el
   render graph mínimo. A partir de ahí, el camino a un tile (fase 3) es corto.

---

## Estado del diseño

- **Esqueleto arquitectónico:** cerrado (fundamentos + lógica + gráficos, 8 docs).
- **Frame-rendering:** completo (los tres caminos existen y son visibles).
- **Pendiente:** sistemas de contenido (fase 7+), que son "agregar" sobre base firme.
```

---

## Checklist de hitos

```
[ ] Fase 0 — ventana + bucle + Core
[ ] Fase 1 — pantalla limpia / triángulo (Vulkan vivo)
[ ] Fase 2 — el triángulo, pero por el render graph
[ ] Fase 3 — tilemap + jugador + cámara (★ pila entera validada)
[ ] Fase 4 — overworld jugable + save
[ ] Fase 5 — escena 3D/HD-2D con sol y sombras
[ ] Fase 6 — look HD-2D completo (bloom + tilt-shift)
[ ] Fase 7+ — contenido (UI, animación, audio, combate, ...)
```
