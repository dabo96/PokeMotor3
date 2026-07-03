# PokeMotor2 — Motor Roadmap

Este documento describe las fases para completar el **motor**. Todo lo que sea juego (Pokemon, batallas, historia) se construye despues sobre el motor usando Lua + assets.

El objetivo: un desarrollador puede sentarse, escribir Lua, importar assets, y crear un juego completo **sin tocar C++**.

---

## Estado del Motor (Post-Consolidacion C1-C4)

### Sistemas COMPLETOS
- ECS con IDs generacionales, sparse-set pools, transform hierarchy
- Renderer 3D deferred (PBR, IBL, CSM shadows con fade/normal offset, cel-shading)
- Post-processing (TAA+CAS, Bloom, DoF, Volumetric Fog, Color Grading, Outlines)
- Sprite system (billboards, atlas, animation, G-Buffer integration)
- Scene Manager (register, change, transitions con fade)
- Scene Serializer (JSON save/load de entidades y componentes)
- Resource Manager (textures, models, shaders — cached)
- Input System (keyboard + mouse, action mapping)
- Audio Manager (WAV loading, play/stop, volume, loop)
- Lua Script Engine (sol2, 11 modulos, hot-reload, property system, coroutines)
- Editor (hierarchy, inspector con script properties, asset browser, play/stop/pause)
- GameUI system (rects, gradients, elevation, text, images, clipping, buttons)
- Debug Console (color-coded, commands, error badge)
- Day/Night Cycle
- Particle System (compute shader)
- Physics (AABB collision, terrain, triggers)

### Sistemas PARCIALES o AUSENTES en el motor

| Sistema | Estado | Que falta |
|---------|--------|-----------|
| **Persistence/Save** | Ausente | API para que Lua guarde/cargue datos arbitrarios a archivo |
| **2D Rendering** | Parcial | No hay tile map renderer, no hay sprite batching 2D puro |
| **Gamepad** | Ausente | No hay SDL GameController integration |
| **Scene from JSON** | Parcial | Las rutas del juego estan hardcodeadas en C++, no cargadas de JSON |
| **Audio avanzado** | Parcial | No hay crossfade, no hay streaming, no hay audio 3D espacial |
| **Animation** | Parcial | AnimationComponent existe pero no hay state machine ni blend trees |
| **Camera system** | Parcial | Solo third-person. No hay camera collision, fixed camera, o cinematic mode |
| **Screen transitions** | Basico | Solo fade a negro. No wipe, slide, o custom transitions |
| **UI: Font loading** | Ausente | No se puede cargar fuentes custom desde Lua |
| **UI: Text input** | Ausente | No hay text input en GameUI para que Lua haga forms (nombre de jugador) |
| **Config persistence** | Ausente | No se guardan settings de renderer/audio/input entre sesiones |
| **Localization** | Ausente | No hay sistema de i18n / string tables |

---

## FASES DEL MOTOR

### FASE M1 — Persistence Layer
**Objetivo:** Lua puede guardar y cargar datos arbitrarios a disco.

| # | Feature | Descripcion |
|---|---------|-------------|
| M1.1 | **FileSystem API en Lua** | `FileSystem.save(path, jsonTable)` / `FileSystem.load(path) → table`. Escribe/lee JSON. |
| M1.2 | **Save slots** | `FileSystem.saveSlot(slot, data)` / `FileSystem.loadSlot(slot)`. Directorio predefinido. |
| M1.3 | **Config persistence** | `Config.save(key, value)` / `Config.load(key, default)`. Para settings del motor. |

**Prueba:** Un script Lua guarda una tabla con datos, cierra el motor, reabre, carga la tabla, datos intactos.

---

### FASE M2 — Gamepad & Input Polish
**Objetivo:** Input completo para juegos de consola.

| # | Feature | Descripcion |
|---|---------|-------------|
| M2.1 | **SDL GameController** | Detectar gamepads. Joystick → acciones de movimiento. Botones → Interact/Confirm/Cancel. |
| M2.2 | **Input remapping** | `Input.bindAction(action, key/button)` desde Lua. Persistir en config. |
| M2.3 | **Vibration/Rumble** | `Input.rumble(intensity, duration)` para feedback haptico. |

**Prueba:** Conectar un gamepad Xbox, jugar sin teclado. Botones remapeables.

---

### FASE M3 — Scene System Upgrade
**Objetivo:** Escenas 100% data-driven, sin C++ hardcodeado.

| # | Feature | Descripcion |
|---|---------|-------------|
| M3.1 | **Scene loading from JSON** | `Scene.load("Assets/Scenes/Route1.json")` carga entidades, terreno, environment. |
| M3.2 | **Scene metadata** | Nombre, musica de fondo, ambient color, skybox, encounter table ref — todo en JSON. |
| M3.3 | **Scene transitions** | Transiciones configurables: fade, wipe, slide. Lua elige tipo y duracion. |
| M3.4 | **Spawn points** | Entidades marcadas como spawn points. `Scene.change(name, spawnId)`. |

**Prueba:** Crear una escena nueva solo con el editor (sin escribir C++). Cargarla en runtime desde Lua.

---

### FASE M4 — Audio System
**Objetivo:** Audio completo para juegos.

| # | Feature | Descripcion |
|---|---------|-------------|
| M4.1 | **Music player** | `Audio.PlayMusic(path, loop, fadeIn)` / `Audio.StopMusic(fadeOut)`. Solo 1 track a la vez. |
| M4.2 | **Music crossfade** | Al cambiar de zona, crossfade entre tracks. `Audio.CrossfadeMusic(newPath, duration)`. |
| M4.3 | **SFX channels** | Multiples SFX simultaneos. `Audio.PlaySFX(path, volume)`. Fire-and-forget. |
| M4.4 | **Audio format** | Soporte OGG Vorbis ademas de WAV (WAV es muy grande para musica). |

**Prueba:** Caminar entre zonas con musica diferente. Crossfade suave. SFX de menu sobre la musica.

---

### FASE M5 — Camera System
**Objetivo:** Camaras flexibles para diferentes situaciones de juego.

| # | Feature | Descripcion |
|---|---------|-------------|
| M5.1 | **Camera collision** | Raycast contra geometria. Camara no atraviesa paredes. |
| M5.2 | **Camera modes** | ThirdPerson (actual), Fixed (posicion fija), Follow (sigue entidad sin input), Free (editor). Switchable desde Lua. |
| M5.3 | **Camera scripting** | `Camera.setTarget(entity)`, `Camera.setOffset(vec3)`, `Camera.shake(intensity, duration)`. |
| M5.4 | **Cinematic camera** | Spline paths para cutscenes. `Camera.playPath(waypoints, duration)`. |

**Prueba:** Interior de un edificio con camara fija. Exterior con third-person + collision. Cutscene con path.

---

### FASE M6 — UI System Expansion
**Objetivo:** UI suficiente para crear cualquier menu de juego desde Lua.

| # | Feature | Descripcion |
|---|---------|-------------|
| M6.1 | **Text Input widget** | `GameUI.textInput(x,y,w, buffer, placeholder)` para nombre de jugador, busqueda, etc. |
| M6.2 | **Font loading** | `GameUI.loadFont(name, path, size)`. Usar fuentes custom en el juego. |
| M6.3 | **ScrollView** | `GameUI.beginScroll(x,y,w,h, contentH)` / `endScroll()` para listas largas (pokedex, inventario). |
| M6.4 | **9-slice panels** | `GameUI.nineSlice(x,y,w,h, textureHandle, border)` para bordes de dialogo estilo RPG. |
| M6.5 | **Sprite animation en UI** | `GameUI.animatedImage(x,y,w,h, handle, cols,rows, fps)` para iconos animados. |

**Prueba:** Crear un menu scrollable con fuente custom, text input, y panel con bordes decorativos, todo desde Lua.

---

### FASE M7 — Animation System
**Objetivo:** Animaciones de personajes y objetos.

| # | Feature | Descripcion |
|---|---------|-------------|
| M7.1 | **Animation state machine** | Estados (idle, walk, run, attack) con transiciones. Configurable desde Lua. |
| M7.2 | **Animation blending** | Crossfade entre clips (0.2s blend de idle→walk). |
| M7.3 | **Tween system** | `Tween.to(entity, {position={0,5,0}}, 1.0, "easeOutQuad")` para animaciones procedurales. |
| M7.4 | **Sprite animation events** | Frame callbacks: `OnFrame(5, function)` para sincronizar SFX con animacion. |

**Prueba:** Personaje con idle→walk→run blend. Tween de UI element. SFX en frame especifico.

---

### FASE M8 — 2D Rendering (opcional)
**Objetivo:** Soporte para juegos 2D puros o vistas 2D (batalla, menus).

| # | Feature | Descripcion |
|---|---------|-------------|
| M8.1 | **Sprite batching** | Renderer 2D optimizado. Z-order sorting. Miles de sprites sin issue. |
| M8.2 | **Tile map renderer** | Cargar tile maps (Tiled JSON format). Multiples capas, colision auto. |
| M8.3 | **2D camera** | Scroll, zoom, bounds. `Camera2D.follow(entity)`, `Camera2D.setBounds(rect)`. |
| M8.4 | **2D physics** | AABB 2D simple, tilemap collision, trigger zones. |

**Prueba:** Overworld 2D estilo Pokemon clásico funcionando con tile map + sprites.

---

## Dependencias

```
M1 (Persistence) ← independiente, critico
M2 (Gamepad)     ← independiente
M3 (Scenes)      ← independiente
M4 (Audio)       ← independiente
M5 (Camera)      ← independiente
M6 (UI)          ← independiente
M7 (Animation)   ← independiente
M8 (2D)          ← independiente, opcional
```

Todas las fases son **independientes** entre si. Se pueden hacer en cualquier orden.

**Orden recomendado por impacto:**
```
M1 (Persistence) → M3 (Scenes) → M2 (Gamepad) → M4 (Audio) → M6 (UI) → M5 (Camera) → M7 (Animation) → M8 (2D)
```

---

## Convenciones del Motor

| Regla | Descripcion |
|-------|-------------|
| Engine vs Game | C++ = motor generico. El motor no sabe que es un Pokemon. |
| Nuevos features | Disabled by default, toggle en RendererUI/EditorUI. |
| API publica | Todo lo que el game dev necesite debe ser accesible desde Lua. |
| Compilacion | El usuario compila. Claude no ejecuta cmake build. |
| Datos del juego | JSON + Lua. El motor provee FileSystem, el juego decide la estructura. |
| Escenas | JSON. El editor las crea, el motor las carga. |
