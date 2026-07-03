# Motor Pokémon — Informe de auditoría (código vs diseño objetivo)

> Auditoría del código real frente a la arquitectura **objetivo** definida en los
> documentos `MotorGrafico_*.md` (Vulkan + **SDL** + **forward rendering**,
> minimalista/YAGNI, RenderGraph como DAG). Fecha: 2026-06-19. Solo informe — no
> se modificó código. Basado en lectura estática (no se compiló ni ejecutó).

---

## 1. Resumen ejecutivo

**El código NO implementa la arquitectura de los documentos de diseño.** Lo que
hay en el repo es el **motor viejo "PokeMotor2" (OpenGL, deferred, avanzado)
portado a Vulkan**, siguiendo el plan de `ROADMAP.md` — no el motor nuevo
**forward minimalista** que describen los `MotorGrafico_*.md`.

La divergencia es estructural, no de detalle:

| Eje | Diseño objetivo (`MotorGrafico_*`) | Código real |
|---|---|---|
| Estrategia de luz | **Forward** (rechaza deferred explícitamente) | **Deferred** (G-buffer + lighting pass) |
| Filosofía | Minimalista, YAGNI | SSR+SVGF, IBL, TAA, fog volumétrico, auto-exposición |
| RenderGraph | DAG (topológico + culling + aliasing + barreras auto) | **Lineal** (orden a mano, solo imágenes) |
| Scripting Lua | Aplazado (YAGNI) | Presente (aunque mínimo en el binario) |
| Plataforma | SDL ✅ | SDL ✅ (único eje ya alineado) |
| Camino 2D (Fase 3) | La "gran validación" | **No existe** |
| Lógica de juego (Fase 4) | Esqueleto jugable | **0%** |

**Veredicto:** el binario actual es un **editor + demo de renderer deferred**
técnicamente competente en su backend Vulkan, pero las dos mitades que el diseño
considera el corazón del proyecto — el **camino 2D** (Fase 3) y la **lógica de
juego** (Fase 4) — están ausentes, y la mitad gráfica está construida sobre la
decisión arquitectónica opuesta a la del diseño.

Además, una parte significativa del código del árbol **ni siquiera se compila**
(es código huérfano: `Core/Events`, `Core/Input`, `Core/Pokemon`,
`Core/Persistence`), lo que hace que el repo "parezca" más completo de lo que el
ejecutable realmente es.

---

## 2. Estado por hito (Fase 0–7)

| Fase | Tema | Estado vs diseño | Veredicto |
|---|---|---|---|
| **0** | Cimientos (Core, EventBus, Input, loop SDL) | ⚠️ Parcial / desconectado | Plataforma SDL OK; **falta módulo Core base**; EventBus/Input **huérfanos** |
| **1** | Backend Vulkan mínimo | ✅ Sólido | vk-bootstrap+VMA, validation, frames-in-flight, sync correcta |
| **2** | Render graph mínimo | ⚠️ Lineal, no DAG | Funciona pero diverge del diseño (sin compile/culling/aliasing) |
| **3** | **Camino 2D (★ validación)** | ❌ Ausente | Sin ortho cam, SpriteBatch, tilemap, upscale nearest |
| **4** | **Lógica de juego (esqueleto)** | ❌ ~0% | Sin mode stack, grid, encuentros, flags, save de partida |
| **5** | 3D / HD-2D (pass, materiales, luces) | ⚠️ Deferred + huecos | Existe pero es deferred; sin sistema de materiales/familias |
| **6** | Post-proceso (el look) | ⚠️ Exceso + hueco crítico | Bloom/tonemap OK; **falta DoF/tilt-shift**; sobran SSR/IBL/TAA/fog |
| **7+** | Contenido | — | Fuera de alcance (depende de 0–6) |

---

## 3. Hallazgos transversales (los que mandan)

### H1 — Crisis de identidad: dos planes incompatibles en el mismo repo
`MotorGrafico_IndiceMaestro.md` (Vulkan, forward, YAGNI, Lua aplazado) y
`ROADMAP.md` (OpenGL deferred, Lua como pilar) describen **motores opuestos**.
El código sigue el segundo. Encima, `CLAUDE.md` describe el motor como **OpenGL
4.6** cuando el binario real es **Vulkan**. Tres fuentes de verdad en conflicto.
→ *Decisión tomada por el usuario: los docs `MotorGrafico` son el objetivo. Este
informe asume esa dirección.*

### H2 — La divergencia central: deferred vs forward (Fase 5)
El código ilumina con un **único shader fullscreen** (`lighting.frag`) que lee un
G-buffer de 5 attachments. El diseño exige **forward**: cada shader de material
recorre las luces y escribe color HDR directo. Converger implica:
- **Reescribir** la estructura de passes (fusionar geometry+lighting en un pass
  forward por-material) y **reorganizar descriptor sets** (mover luces/sombra/
  ambiente del set 2 al **set 0**).
- **Re-cablear** SSR/Hi-Z/fog/TAA, que hoy dependen del G-buffer.
- **Reutilizable (~70%)**: bucle BRDF Cook-Torrance, CSM/PCF/PCSS, IBL, luces
  como dato (SSBO + sync systems), pipeline de sprites. *El trabajo difícil de
  iluminación ya está hecho; lo que cambia es dónde vive.*

> ⚠️ Nota honesta: si los efectos avanzados (SSR, IBL) ya construidos aportan
> valor y se quieren conservar, deferred podría ser la decisión correcta *de
> facto*. Pero entonces serían los **docs** los que habría que actualizar. Dado
> que decidiste que los docs son el objetivo, la recomendación es **converger a
> forward** y recortar lo que sobra (ver H5).

### H3 — Código huérfano: el repo aparenta más de lo que el binario es
Estos módulos están en el árbol pero **fuera de `CMakeLists.txt`** → no se
compilan, no se testean, pueden estar rotos sin que nadie lo note:
- `Core/Events/*` (EventBus, GameEvents) — el "glue" del diseño, **no enlazado**.
- `Core/Input/InputSystem.*` — arquitectura de input excelente (snapshots,
  flancos, ActionMap, gamepad), **no enlazada**. El motor real lee input con
  **SDL crudo** (`SDL_GetKeyboardState` en `src/main.cpp:63-73`).
- `Core/Pokemon/*` (incluido `BattleSystem.cpp`) — datos flyweight + combate,
  **muertos**.
- `Core/Persistence/*` (FileSystemManager, ConfigManager) — la base correcta para
  el save de partida, **muerta**.
- `main.cpp` de raíz (OpenGL/glad, 304 líneas) coexiste con `src/main.cpp` (el
  real). Obsoleto.

### H4 — Falta el módulo Core fundacional
No existen `Core/Log.h`, `Assert.h`, `Math.h` (aliases GLM), `Handle.h`
(`Handle<T>` genérico con generation), `StringID.h` (FNV-1a), `Time.h`,
`Memory.h`. Hoy: logging = `printf`/`fprintf`/`SDL_Log`/`std::cout` dispersos;
sin asserts; handles de renderer como `uint32_t` crudos (sin tipado ni
detección de use-after-free). Es el cimiento del que dependen todas las fases.

### H5 — Exceso de complejidad vs YAGNI (Fase 6)
El post tiene ~9 efectos que el diseño minimalista no pide. Peor: varios
**violan la convención "features desactivadas por defecto"**:
- **TAA**: siempre activo, **sin toggle** (`VulkanContext.cpp:6823`). Además tiende
  a emborronar el pixel-art HD-2D. → dar toggle + apagar, o sustituir por FXAA.
- **CAS sharpen**, **god rays**, **fog temporal**: **ON por defecto**.
- **SSR+SVGF** (Hi-Z, ray sort/prefix/trace, denoiser): el subsistema más pesado
  del motor. Principal candidato a recorte por coste/beneficio en un HD-2D.
- **Auto-exposición por histograma**: 2 compute passes para algo que el diseño
  resuelve con `exp2(exposure)` manual; el propio shader documenta que tiende a
  "crushear" la exposición.
- **IBL, fog volumétrico**: razonables como "3D completo" futuro, no para arrancar.

### H6 — `VulkanContext` god object (7.665 líneas)
Fusiona los tres roles que el diseño separa: `VulkanContext` (device/swapchain),
`Renderer` (sync/present) y `RenderGraph` (passes). `Initialize` encadena ~55
`createX()`; `Shutdown` destruye a mano ~120 handles. Contradice frontalmente la
filosofía minimalista y dificulta todo lo demás.

### H7 — Desacople renderer↔lógica roto
El diseño exige "el renderer NO depende de la lógica; los une la Application". En
el código, la capa de escena/persistencia (`VkSceneManager`, `VkSceneSerializer`)
y los sync-systems viven **dentro** de `src/Rendering/Vulkan/` e incluyen
`VulkanContext.h`. El renderer define la API del dato (`RegisterSprite`,
`RegisterLight`) en vez de recibir una lista neutra de draws.

### H8 — Docs de diseño faltantes
`MotorGrafico_IndiceMaestro.md` referencia `MotorGrafico_DiseñoTecnico.md`
(orden de init Vulkan, ciclo de frame) y `MotorGrafico_LogicaDeJuego.md` (datos,
modos, combate, save, grid, eventos) — **ninguno de los dos existe**. La Fase 4
carece de su especificación de referencia.

---

## 4. Detalle por fase

### Fase 0 — Fundamentos + Plataforma  ⚠️
- ✅ SDL3 + Vulkan, init en orden razonable; GLM como math.
- ✅ `InputSystem` (en disco) implementa el diseño casi exacto: doble snapshot,
  flancos, ActionMap, gamepad, rebinding — **más completo que el doc**. Pero **no
  se compila ni se usa** (H3).
- ✅ `EventBus` (en disco): tipado, emit/queue/dispatch. Pero diverge del diseño:
  es **singleton global** (el doc lo quiere poseído por Application), devuelve
  `ListenerID` crudo en vez de **token RAII `Subscription`** (no existe
  `Subscription.h`), y `Unsubscribe` recorre todos los tipos. Y **no se compila**.
- ❌ Falta módulo Core (H4). ❌ Loop solo variable timestep (sin fixed/acumulador,
  pedido en Fase 0). ❌ `EventBus::dispatch()` nunca se llama en el frame.

### Fase 1–2 — Backend Vulkan + RenderGraph  ✅ / ⚠️
- ✅ **Backend sólido**: vk-bootstrap (instance/device/swapchain) + VMA, Vulkan 1.3
  (dynamic rendering + synchronization2), validation + sync-validation + best
  practices en debug, `kFramesInFlight=2`, `renderFinished` semaphore
  **per-swapchain-image** (evita el bug VUID clásico de present). Buen trabajo.
- ⚠️ **RenderGraph lineal, no DAG** (reconocido en `RenderGraph.h:38-45`): orden a
  mano de ~38 passes en `recordCommandBuffer`, sin compile/topológico, sin
  culling (solo `if` imperativos), sin recursos transitorios ni aliasing, solo
  rastrea **imágenes** (las barreras de buffers de SSR/partículas/histograma se
  hacen **a mano** dentro de los `execute`, ~40 llamadas).

### Fase 3 — Camino 2D + Cámara  ❌
- ❌ Búsqueda global de `SpriteBatch|OrthographicCamera|PerspectiveCamera|
  FollowCameraController|TileMap|Sprite2DPass|UpscalePass|UnlitSprite` = **0 en
  código**. No existe la interfaz `Camera` abstracta (la clase `Camera` es
  concreta y 3D free-fly), ni `OrthographicCamera`, ni SpriteBatch (batching por
  layer+atlas), ni Sprite2DPass, ni target lowRes + upscale nearest, ni tilemap.
- Los "sprites" actuales son **billboards 3D que escriben al G-buffer deferred**
  (Fase 5), no quads 2D ortográficos. La "gran validación" del diseño está sin
  construir.

### Fase 5 — Pass principal + Materiales + Luces/Sombras  ⚠️
- Pipeline **deferred** confirmado (ver H2). ✅ Color HDR, ✅ buffer de luces SSBO,
  ✅ 3 tipos de luz + area, ✅ CSM (3 cascadas) con PCF/PCSS/normal-offset bias/
  cross-fade, ✅ IBL.
- ❌ **No hay sistema de materiales data-driven**: no existe `Material`/`Shader`/
  `MaterialFamily`. Los params PBR están **hardcodeados como push constants** por
  mesh; el pass tiene bucles separados a mano para mallas vs sprites. No hay
  familia **Unlit**.
- ⚠️ **Sprites HD-2D incompletos**: ✅ alpha-clip, escribe depth, recibe/proyecta
  sombra; ❌ **no NEAREST** (muestrean el sampler LINEAR global → pixel-art
  borroso), ❌ **no billboard** (no orientan a cámara), ❌ sin half-lambert.
- ❌ Sin bucket opacos/transparentes (la debilidad de deferred que el doc citaba).

### Fase 6 — Post-proceso  ⚠️
- ✅ **Bloom** (pirámide HDR, prefilter Karis, down 13-tap anti-firefly, up tent)
  — implementación de alta calidad. ✅ **Tonemap** ACES (correctamente el último
  eslabón, sin toggle).
- ❌ **DoF / tilt-shift: NO EXISTE** — y el doc lo llama "el corazón del look
  HD-2D". Único hueco del diseño objetivo en esta fase, y el más importante. Toda
  la infraestructura para implementarlo ya existe (depth disponible, patrón
  fullscreen pass).
- ⚠️ Exceso vs YAGNI con defaults mal puestos (H5).

### Fase 4 — Lógica de juego  ❌
- ✅ **ECS sólido y alineado**: IDs generacionales 20+12, sparse-set O(1),
  `View<Ts...>`, jerarquía de transform con reparenting. ✅ Física AABB 3D, Audio.
- ❌ **Nada del esqueleto de Fase 4**: sin pila de modos / `OverworldMode`
  (solo `EditorMode{Editor,Play}` de cámara), sin movimiento por grid, sin
  encuentros (pisar hierba), sin flags + `EventRunner`. Combate (`BattleSystem`)
  existe pero muerto.
- ⚠️ **Save de alcance equivocado**: `VkSceneSerializer` guarda *escena de editor*
  (entidades + componentes de render), no *partida* (equipo, flags, posición). El
  `FileSystemManager` correcto está sin compilar.
- ❌ Capa de datos Pokemon flyweight correcta de diseño pero **muerta**;
  `species.json`/`moves.json` huérfanos.

---

## 5. Bugs concretos confirmados (independientes de la migración)

| # | Severidad | Bug | Ubicación |
|---|---|---|---|
| B1 | **Alto** | **Última cascada CSM nunca proyecta sombra**: `pickCascade` devuelve hasta 3, pero `sampleShadow` hace `if (cascade>=3) return 1.0` con `kCascadeCount=3` → la franja lejana queda sin sombra (off-by-one). | `lighting.frag:97-102,145` |
| B2 | **Alto (sospecha)** | `recreateSwapchain` hace `destroyPerImageSemaphores()` pero **no se ve la recreación** correspondiente → posible crash/UB al redimensionar si cambia el nº de imágenes. *Requiere confirmar en `VulkanContext.cpp:5532-5854`.* | `VulkanContext.cpp:5456` |
| B3 | Medio | EventBus sin token RAII → un suscriptor destruido sin `Unsubscribe` deja un `std::function` apuntando a `this` liberado; el siguiente `Emit` llama memoria muerta. | `EventBus.h:22-38` |
| B4 | Medio | Sprites muestrean atlas con sampler **LINEAR** → HD-2D borroso (debería ser NEAREST por material). | `VulkanContext.cpp:4730` |
| B5 | Bajo | `recreateSwapchain` ignora su valor de retorno y no maneja el caso "minimizado sostenido" (reintento ciego cada frame). | `VulkanContext.cpp:7556-7558` |
| B6 | Bajo | `signalSem.stageMask = ALL_GRAPHICS_BIT` para render-finished; debería ser `ALL_COMMANDS_BIT` (no cubre TRANSFER de un blit). | `VulkanContext.cpp:7626` |
| B7 | Bajo | `gEmissive` y AO siempre 0/1: el material no expone emisión ni occlusion aunque el G-buffer los transporta. | `geometry.frag:49,69` |
| B8 | Latente | Shaders declaran `lightSpaceMatrix[3]` literal y `>=3` hardcodeado; subir a 4 cascadas (como pide el doc) los desincroniza en silencio. | `geometry.vert:11`, `lighting.frag:9` |

---

## 6. Recomendaciones priorizadas (globales)

### P0 — Desbloqueantes y honestidad del estado
1. **Alinear las fuentes de verdad** (H1): adoptados los `MotorGrafico` como
   objetivo, actualizar/archivar `ROADMAP.md` y corregir `CLAUDE.md` (dice OpenGL;
   es Vulkan). Escribir los docs faltantes `DiseñoTecnico.md` y `LogicaDeJuego.md`,
   o quitar sus referencias (H8).
2. **Sanear el código huérfano** (H3): decidir por cada módulo no compilado
   (`Core/Events`, `Core/Input`, `Core/Pokemon`, `Core/Persistence`, `main.cpp`
   raíz) si se enlaza, se archiva o se borra. Hoy el repo miente sobre sí mismo.
3. **Crear el módulo Core fundacional** (H4): `Log/Assert/Math/Handle/StringID/
   Time` header-only puros. Cimiento de todo lo demás.
4. **Arreglar B1** (última cascada sin sombra) y **confirmar/arreglar B2**
   (semáforos en recreateSwapchain). Son corrección, no refactor.

### P1 — Convergencia arquitectónica con el diseño
5. **Decidir y ejecutar deferred→forward** (H2). Si se mantiene deferred, actualizar
   los docs en su lugar — pero no dejar la contradicción abierta.
6. **Conectar el Input** real (enlazar `InputSystem` al build y al loop, o decidir
   reemplazarlo) y eliminar la lectura SDL cruda duplicada.
7. **EventBus según diseño**: no-singleton, `subscribe()` → `Subscription` RAII,
   `dispatch()` una vez por frame. Compilarlo y darle un consumidor real.
8. **Construir el camino 2D (Fase 3)** — la validación que falta: interfaz
   `Camera` + `OrthographicCamera`/`PerspectiveCamera`, `SpriteBatch` (orden por
   layer+atlas), familia **Unlit**, `Sprite2DPass` a lowRes + **UpscalePass
   nearest**, tilemap → sprites, input moviendo cámara/jugador.
9. **Implementar DoF/tilt-shift** (Fase 6) — el hueco más importante del look.
10. **Recortar el exceso de post** (H5): toggle+off a TAA/CAS/godRays/fogTemporal;
    decidir recorte de SSR+SVGF y auto-exposición.

### P2 — Refactor estructural y pulido
11. **Descomponer `VulkanContext`** (H6): extraer `Renderer` (sync/present) y mover
    cada efecto a su propio `Pass`/módulo.
12. **RenderGraph → DAG**: `compile()` topológico, culling derivado, recursos
    transitorios + aliasing, tracking de buffers.
13. **Desacoplar renderer↔lógica** (H7): mover glue/escena/serializer a una capa
    `Application`/`Game` fuera de `Rendering/Vulkan/`.
14. **Sistema de materiales con familias** (PBR/HD2D/Unlit) data-driven; sprites
    NEAREST + billboard real (B4); 4 cascadas con splits lineal-log (B8).
15. **`Handle<T>` tipado** en las APIs del renderer; logging unificado del Core;
    fixed timestep para la lógica.

### Construir el esqueleto de Fase 4 (cuando 0–3 estén firmes)
16. Pila de modos + `OverworldMode`; grid + colisión por casillas + encuentros;
    flags + `EventRunner`; save de **partida** (sobre `FileSystemManager`);
    cablear la capa de datos Pokemon flyweight (`species.json`/`moves.json`).

---

## 7. Decisiones que requieren tu input

1. **deferred vs forward**: ¿converger el código a forward (refactor medio-alto,
   ~70% reutilizable) o conservar deferred y actualizar los docs? *(Elegiste docs
   = objetivo → por defecto, converger a forward.)*
2. **Scripting Lua**: el diseño lo aplaza (YAGNI); el ScriptEngine compilado es
   mínimo y las `.lua` de juego están huérfanas. ¿Aplazar/archivar o adoptar?
3. **Efectos avanzados** (SSR, IBL, fog, auto-exposición): ¿cortar para cumplir
   YAGNI, o aparcar tras toggles como "3D completo futuro"?
4. **Código huérfano**: ¿enlazar, archivar o borrar cada módulo no compilado?

---

## 8. Apéndice — Migración forward: qué reutilizar vs reescribir

**Reutilizar (~70%):** BRDF Cook-Torrance GGX, `sampleShadow`/PCF/PCSS,
`pickCascade`, IBL split-sum, atenuación de luces (todo de `lighting.frag` → a un
header GLSL `#include`); CSM completo (`shadow.*`, cálculo de cascadas,
`m_shadowMap`); luces como dato (SSBO, `RegisterLight`, `VkLightSyncSystem`);
bindless de texturas de material (set 1); SSBO/instanciación de sprites.

**Reescribir:** `geometry.frag` → `pbr.frag` (lit, recorre luces, escribe HDR);
eliminar el lighting pass fullscreen y el G-buffer (salvo lo que SSR/fog/TAA
necesite, p.ej. depth prepass + normales); **mover luces/sombra/ambiente del set
2 al set 0**; re-cablear SSR/Hi-Z/fog/TAA.

---

*Límites de esta auditoría: análisis estático; no se compiló ni ejecutó el motor.
Las afirmaciones "no compilado" se basan en ausencia en `CMakeLists.txt` y en
greps de inclusión. B2 es sospecha fuerte pendiente de confirmar en un tramo de
`VulkanContext.cpp` no leído. Los "funciona" de shaders se basan en lectura
GLSL, no en runtime.*
