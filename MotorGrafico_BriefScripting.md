# Brief de implementación — Sistema de Scripting (Lua) · PokeMotor

> **Para Claude Code.** Implementar un sistema de scripting en Lua que permita
> autorar comportamiento de entidades **sin tocar C++**, descubrible desde el
> editor, con variables expuestas en el inspector y hot-reload resiliente.

---

## Contexto del motor

- **PokeMotor**: motor propio en C++ / Vulkan, RPG estilo Pokémon, modo 2D.
- **Ya existe** (no reconstruir):
  - Editor: panel de **Jerarquía**, **Inspector** (renderiza componentes en
    secciones colapsables con campos editables), navegador de **Assets** (con una
    carpeta `Scripts/`), y **Consola**.
  - Modelo **ECS pragmático**: entidades = id + componentes de datos; **sistemas**
    que recorren entidades por componente.
  - Render 2D: sprites, tilemap, **texto MSDF**.
  - El **movimiento del jugador YA funciona** como un sistema en C++.
- **Problema que este sistema resuelve:** el comportamiento en C++ es opaco — un
  tercero no sabe de dónde sale ni cómo crear el suyo. El scripting lo hace
  autorable y descubrible.

> **Claude Code:** adapta los nombres (Scene, Entity, Inspector, Console,
> ActionMap, etc.) a los del código real del repo. Este brief define las *formas*
> e *interfaces*; intégralas con lo que ya existe.

---

## Objetivo (v1)

Que un usuario pueda, **sin tocar C++**:
1. Escribir un `.lua` en `Scripts/`.
2. Asignarlo a una entidad desde el Inspector.
3. Ver sus variables exportadas como campos editables (reusando el inspector).
4. Que corra (`on_update` cada frame).
5. Editarlo en un editor externo (VS Code) y, al guardar, **recargar en caliente**
   sin reiniciar el motor.

Meta de paridad: reproducir el movimiento actual del jugador **desde un script Lua**.

---

## Dependencia

- **sol2** (binding C++/Lua, header-only): https://github.com/ThePhD/sol2
- Lua 5.4 (o LuaJIT si se prefiere; sol2 soporta ambos).

No escribir el binding a mano.

---

## Arquitectura — piezas a construir

### 1. `LuaVM` — wrapper del estado de Lua

```cpp
class LuaVM {
public:
    void init();                                  // crea sol::state, registra el Script API
    sol::table loadScript(const std::string& path); // carga el chunk; en error: loguea y devuelve tabla inválida
    bool reload(const std::string& path);         // recarga; false + log si error (NUNCA crashea)
    sol::state& state();
private:
    sol::state m_lua;
};
```

### 2. `ScriptComponent` — componente de datos

```cpp
struct ScriptComponent {
    std::string path;                 // ruta del script en Scripts/
    sol::table  self;                 // estado POR ENTIDAD (persiste entre frames)
    sol::table  module;              // el chunk cargado (exports + on_start + on_update)
    bool        started = false;     // ¿ya se llamó on_start?
    // los valores editados de exports viven dentro de `self` (ver modelo de estado)
};
```

### 3. `ScriptSystem` — corre los scripts

```cpp
void ScriptSystem(Scene& scene, LuaVM& vm, float dt) {
    for (Entity e : scene.view<ScriptComponent>()) {
        auto& sc = scene.get<ScriptComponent>(e);
        if (!sc.module.valid()) continue;

        if (!sc.started) {
            callHook(sc, "on_start");   // protegido: error → consola, no crash
            sc.started = true;
        }
        callHook(sc, "on_update", dt);  // idem
    }
}
```

`callHook` usa `sol::protected_function`: si el script lanza, captura el error,
lo manda a la **consola en rojo**, y el motor sigue. Las demás entidades no se ven
afectadas.

### 4. Script API — la superficie descubrible (DELIBERADA y ESTABLE)

Las funciones que Lua puede llamar. **Esta es la cara pública del motor para
contenido** — los scripts llaman a esta API, NO a las tripas del motor, para que
refactorizar el motor no rompa los scripts. Registrar en `LuaVM::init`.

Superficie mínima de v1 (ejemplos; ampliar con criterio):

```
-- entrada
input.is_down("Up"|"Down"|"Left"|"Right"|"Confirm"|"Cancel") -> bool
input.was_pressed(action) -> bool

-- la entidad (vía self)
self:try_step("Up"|...)        -- intenta un paso en la rejilla (respeta isWalkable)
self:set_move_speed(v)         -- ajusta la velocidad del tween
self:position() -> x, y
self:facing() -> dir

-- mundo / estado
flags.get(name) -> value
flags.set(name, value)
log(msg)                       -- a la consola del editor

-- random
random_direction() -> dir
```

Mantener esta lista documentada (un archivo `Scripts/API.md` generado o a mano)
para que sea descubrible.

### 5. Exports → Inspector

El script declara sus variables editables en una tabla `exports`. El inspector
**reusa el renderizado de campos existente**, leyendo `exports` en vez de la
reflexión de C++.

```cpp
// Pseudocódigo: sección del inspector para ScriptComponent
sol::table exp = sc.module["exports"];
for (auto& [k, meta] : exp) {
    std::string name = k.as<std::string>();
    std::string type = meta["type"];
    if (type == "float") {
        float mn = meta.get_or("min", 0.0f), mx = meta.get_or("max", 1.0f);
        drawFloatField(name, sc.self[name], mn, mx);   // reusa tu widget
    } else if (type == "bool") {
        drawBoolField(name, sc.self[name]);
    } else if (type == "enum") {
        drawEnumField(name, sc.self[name], meta["options"]); // dropdown
    }
}
```

Soportar metadatos: `min`, `max` (slider acotado), `options` (enum → dropdown),
`tooltip`. El valor editado se escribe en `sc.self[name]` (por entidad).

### 6. Hot-reload (BIEN HECHO — requisito, no opcional)

- Vigilar la carpeta `Scripts/` (file watch, o un chequeo de mtime por frame).
- Al cambiar un `.lua`: re-ejecutar el chunk en un entorno limpio.
- **Si hay error:** loguear a la consola en rojo, **mantener la versión anterior**
  corriendo. NUNCA crashear.
- **Si va bien:** intercambiar `module`; por cada entidad que usa el script,
  **preservar los valores de `self`** (re-aplicar los exports editados; para
  exports nuevos, usar su `default`).
- Resultado: el usuario edita, guarda, y ve el cambio en vivo; si se equivoca, lo
  ve en rojo en vez de un crash. Iteración sin miedo.

---

## Modelo de estado e instanciación (importante)

- Un archivo `.lua` = **una definición de comportamiento**.
- Cada entidad que lo usa tiene su propio `self` (tabla que persiste entre frames).
- **El estado por entidad va en `self`**, NO en variables locales del módulo (que
  se compartirían entre entidades). Documentar esto para los usuarios.
- Los valores de `exports` editados en el inspector viven en `self[name]`.
- Inicializar `self` al asignar el script: copiar los `default` de `exports`.

---

## Integración con el motor existente

- Registrar `ScriptSystem` en el bucle **después del input, antes del
  movimiento/tween** (para que el script decida y el tween lo aplique ese frame).
- `ScriptComponent` se suma al modelo de componentes; el Inspector gana una
  sección "Script" (elegir script de `Scripts/` + render de exports).
- La carpeta `Scripts/` del navegador de Assets es el origen; el Inspector permite
  **elegir un script** de ahí (un combo/picker simple basta en v1).
- Errores de Lua (carga y runtime) → la **Consola** existente, en rojo.

---

## Orden de implementación (milestones)

1. **sol2 embebido** + `LuaVM` carga y ejecuta un script de prueba; `log()` sale en
   la consola.
2. **`ScriptComponent` + `ScriptSystem`**: una entidad con script corre `on_update`.
3. **Script API mínima** (input + `try_step` + `set_move_speed`): el sprite se mueve
   **desde un script Lua** (paridad con el C++ actual).
4. **Exports → Inspector**: `speed` editable en vivo desde el panel.
5. **Hot-reload resiliente**: editar el `.lua`, guardar → recarga; un error sale en
   rojo sin tumbar el motor; los valores por entidad se preservan.
6. **Scripts de ejemplo** + un par de funciones más del API (`flags`, `facing`).

Cada milestone deja algo demostrable.

---

## Criterios de aceptación

- [ ] Asignar `player_movement.lua` a la entidad del jugador desde el Inspector →
      se mueve con las flechas, igual que el C++ actual.
- [ ] Cambiar `speed` en el Inspector → cambia el movimiento **en vivo**.
- [ ] Editar `player_movement.lua` en VS Code y guardar → **recarga sin reiniciar**;
      el cambio se ve.
- [ ] Meter un error de sintaxis en el script → aparece en la **consola en rojo**,
      el motor **sigue corriendo**, las demás entidades siguen.
- [ ] Asignar `npc_wander.lua` a otra entidad → deambula, con su propio temporizador
      (estado por entidad, independiente).

---

## Fuera de alcance de v1 — NO construir todavía

- **Corrutinas / secuenciado** (`show_text` → `wait` → `start_battle`): fase 2.
- **EventRunner de comandos** data-driven para diálogos/cutscenes: relacionado pero
  aparte.
- **Editor de código dentro del motor:** se edita en VS Code. No reinventar un peor.
- **Undo/redo, prefabs, selectores de assets avanzados, visual scripting.**

Mantener v1 mínimo y completo. La pulidez se añade cuando el dolor lo pida.

---

## Fase 2 (después, NO ahora)

- **Corrutinas de Lua** para secuencias: `wait`, `show_text` que ceden el control y
  reanudan (scripts que se leen como un guion).
- **Triggers** (`interact` / `step` / `enter`) que disparan scripts.
- Integración con una **caja de diálogo** usando el texto MSDF existente.

---

## Notas de diseño (las recomendaciones, en firme)

- **API deliberada y estable:** los scripts llaman a la API, no a las tripas del
  motor. Aísla los scripts de los refactors internos.
- **Hot-reload resiliente:** un script roto jamás tumba el motor. Es lo que hace la
  iteración sin miedo.
- **Exports con metadatos:** `min`/`max`/`options`/`tooltip` → mejores widgets.
- **Disciplina:** v1 mínimo; nada de gold-plating antes de que el contenido lo pida.

---

## Scripts de ejemplo (entregar como plantillas en `Scripts/`)

`Scripts/player_movement.lua`:

```lua
-- Mueve a la entidad con las teclas. Plantilla base para movimiento por rejilla.
exports = {
    speed = { type = "float", default = 4.0, min = 0.0, max = 10.0,
              tooltip = "Velocidad del tween entre celdas" },
}

function on_start(self)
    self:set_move_speed(self.speed)
end

function on_update(self, dt)
    self:set_move_speed(self.speed)       -- por si se editó en el inspector
    if input.is_down("Up") then
        self:try_step("Up")
    elseif input.is_down("Down") then
        self:try_step("Down")
    elseif input.is_down("Left") then
        self:try_step("Left")
    elseif input.is_down("Right") then
        self:try_step("Right")
    end
end
```

`Scripts/npc_wander.lua`:

```lua
-- Hace que un NPC deambule. El estado (timer) vive en self → por entidad.
exports = {
    interval = { type = "float", default = 2.0, min = 0.5, max = 5.0,
                 tooltip = "Segundos entre pasos" },
}

function on_start(self)
    self.timer = self.interval
end

function on_update(self, dt)
    self.timer = self.timer - dt
    if self.timer <= 0 then
        self.timer = self.interval
        self:try_step(random_direction())
    end
end
```
