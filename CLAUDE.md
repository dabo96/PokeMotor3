# CLAUDE.md

Guía para Claude Code (claude.ai/code) al trabajar en este repositorio.

> **Idioma:** responde siempre en **español** (ortografía y acentos correctos). Términos
> técnicos e identificadores de código se mantienen en su forma original.

## Qué es PokeMotor3

Motor de juego **2D** (RPG estilo Pokémon) en **C++20 / Vulkan 1.3 / SDL3**, con un
**editor** integrado en **FluentUI** (modo Vulkan compartido). La visión a largo plazo
es 2D / 3D / HD-2D; **ahora el foco es 2D y el camino 3D está PAUSADO** (su código
existe pero no se ejercita en runtime).

> **Importante:** `legacy/` contiene el motor anterior (**PokeMotor2**, 3D OpenGL con
> pipeline diferida, VXGI, SSR…). **No se compila** y es solo referencia histórica. No
> confundir con el target actual.

## Build

CMake 3.25+, C++20, Windows x64, **Vulkan**. Dependencias vía **vcpkg** + vendored.

```bash
# Configurar (desde la raíz)
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="[vcpkg-root]/scripts/buildsystems/vcpkg.cmake"

# Compilar
cmake --build build --config Debug    # o Release
```

El post-build copia DLLs (SDL3, lua), la carpeta `Assets/` y los shaders al directorio
de salida. El ejecutable es **`PokeMotor.exe`**. El editor se compila bajo el define
**`ENGINE_EDITOR`**. **No hay framework de tests** (el usuario compila y verifica cada
fase manualmente).

vcpkg (ver `vcpkg.json`): SDL3, Vulkan, vk-bootstrap, VMA, **lua + sol2** (scripting),
freetype, etc. `lua` se importa manualmente en CMake (vcpkg no trae config). En **Debug**
se define `POKEMOTOR_SOURCE_DIR` para el hot-reload de scripts (ver Scripting).

## Arquitectura

### Entry point y loop

`main.cpp` → **`Engine`** (`Engine/Engine.{h,cpp}`) — posee ventana, `VulkanContext`,
`Renderer`, `AssetManager`, `SceneManager`, `Input`, `LuaVM`/`ScriptSystem`, editor, y el
bucle principal (**fixed timestep** para la sim + variable para input/render).

### ECS (`Core/ECS/` + `Scene/`)

ECS propio y pragmático (sparse sets):
- **`Entity`** (`Core/ECS/Entity.h`): `id` (20 bits) + `generation` (detecta
  use-after-free). `EntityManager` con free-list.
- **`ComponentPool<T>`** (`Core/ECS/ComponentPool.h`): sparse set, O(1) add/remove/get.
- **`Scene`** (`Scene/Scene.h`): el registry. `createEntity/destroyEntity/add/remove/
  has/get/view/allEntities`. Consultas con `scene.view<Transform, SpriteComponent>()`.
- **`View<Ts...>`** (`Scene/View.h`): iteración type-safe con `.each([](Entity, Ts&...){})`.

> **GOTCHA crítico:** un `Entity` solo es válido dentro de **SU** `Scene` (ids/generación
> se reinician por escena). Al cambiar de escena hay que re-resolver referencias —
> detectar el cambio comparando el **puntero** de `SceneManager::current()`, no por `alive()`.

**Componentes** (`Scene/Components.h`): `Transform` (pos/rot/scale 2D), `SpriteComponent`
(guarda `texturePath`; `tex` se resuelve al cargar), `NameComponent`, `PlayerTag`,
`ScriptComponent` (solo la ruta del `.lua`), `DrawItem` (3D, pausado).

### Escenas (`Game/SceneManager.{h,cpp}`)

`SceneManager` **posee la escena activa** (`current()`); editor y sistemas operan sobre
ella. Serialización JSON (nlohmann) entidades×componentes; cada tipo se registra con
`registerComponent<T>` + `to_json/from_json` (los componentes guardan **rutas** de asset,
no handles). Atajos temporales en el loop: **F2** guarda, **F3** carga, **F4** nueva.

### Render 2D (`Renderer/`)

`Renderer` sobre backend Vulkan (`Renderer/Vulkan/`: instance, device, swapchain, VMA,
buffers, imágenes, `VulkanContext`, `Texture`). 2D en `Renderer/2D/` (`SpriteBatch`,
`Sprite`, `SpriteSheet`, `Animation`, `TextBatch`). Texto **MSDF** (`Renderer/Text/
MsdfFont`, atlas UNORM, `median(rgb)+screenPxRange`). Cámara ortográfica
(`Renderer/Camera/`).

**Render-feed ECS → sprites** (`Scene/SpriteRenderSystem.cpp`): `collectEntitySprites`
recorre `view<Transform, SpriteComponent>` y emite `Sprite`s; `Renderer::appendSprites`
los intercala por capa con los que pone el modo (tiles, jugador). Mantiene desacoplados
`Scene` ↔ `Renderer`.

> **3D PAUSADO:** `Renderer/Vulkan/MeshVk`, `GltfLoader`, `Assets/ModelLoader` (comentado
> en CMake), `Renderer/Lighting/Light`, `PerspectiveCamera`, `Renderer/Graph/RenderGraph`
> existen pero **no se ejercitan**. No tocarlos salvo que se reactive el 3D.

### Scripting (`Core/Scripting/`)

- **`LuaVM`** (sol2): `sol::state`; `init()` abre librerías + registra el Script API;
  `loadModule(path)` ejecuta cada `.lua` en su propio `sol::environment`. Errores → consola
  (`Core/Log`), **nunca** crashea.
- **`ScriptSystem`** (PIMPL — sol2 oculto en el `.cpp` para no propagarlo): recorre
  `view<ScriptComponent>`, llama `on_start` una vez y `on_update(self, dt)` cada frame,
  protegido. **Hot-reload**: en Debug carga desde el árbol de fuentes (`POKEMOTOR_SOURCE_DIR`)
  y recarga al cambiar el `mtime`. **Exports**: la tabla `exports` del script se muestra
  editable en el inspector (`exportsOf`/`setExport`).
- **Script API** (v1): `self` = la entidad (`x/y/set_position/move`), `move_axis()` (input).
  Pendiente: API de grid `try_step` + colisión (gameplay).

### Modos de juego (`Game/`)

`GameStack` (pila de modos) + `GameMode` (interfaz). `OverworldMode` (mapa de tiles +
jugador-**entidad**), `BattleMode` (combate). `GameContext` pasa a los modos: `scene`,
`assets`, `renderer`, `bus`, `input`, `actions`, `db`, `stack`, `selection`, `screenW/H`,
`uiCapturesMouse`. `TileMap`/`Tile` (mapa + colisión `walkable`), `Database` (especies).

### Editor (`Editor/`, bajo `ENGINE_EDITOR`)

`EditorUI` (FluentUI) dibuja como último pass sobre la swapchain:
- **Jerarquía** = lista viva de `current().allEntities()`; **Inspector** edita los
  componentes de la entidad seleccionada (Transform, Apariencia, exports del Script).
- **Selección** = `Entity` (`Game/Selection.h`), compartida con el picking del viewport.
- **Navegador de assets** (`AssetBrowser`): escanea `Assets/` en disco (árbol + grid);
  arrastrar un sprite al viewport → crea una entidad (`Transform`+`Sprite`+`Name`).
- **Editor de tiles** en 2ª ventana OS (`ToolWindow` + `TileEditorUI`).

### Otros core

- **Eventos** (`Core/EventBus.h`): `EventBus` type-safe (inmediato + diferido).
  `Game/GameEvents.h`: `MapSavedEvent`, `AssetDroppedEvent`, etc.
- **Input** (`Input/`): polling + flancos (`isKeyDown`/`wasKeyPressed`), `ActionMap`,
  emite `KeyPressed/Released` por el bus.
- **Assets** (`Assets/AssetManager`): caché de texturas (resuelve `texturePath` → handle).
- **Core/**: `Log` (con buffer en anillo que alimenta la consola del editor), `Math` (glm),
  `Handle`, `Time`.

## Convenciones

- **Includes con ruta desde la raíz** del repo: `#include "Core/Log.h"`,
  `#include "Scene/Components.h"` (el target añade la raíz como include dir). El backend
  Vulkan (`Renderer/Vulkan/`) usa includes same-dir entre sus archivos.
- **Estilo de trabajo:** cambios mínimos, arreglos de raíz, por fases (el usuario compila
  cada fase). Comentarios en español, densidad acorde al código vecino.
- Nuevas features de render: **desactivadas por defecto**, con toggle en la UI del editor.
- `sol2` es pesado de compilar: NO incluirlo en headers de uso amplio (usar PIMPL como
  `ScriptSystem`).
- Hay **entidades de prueba temporales** en `Engine::init` (p.ej. `ScriptMover`) — limpiar
  cuando dejen de hacer falta.
