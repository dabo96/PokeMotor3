# Brief de implementación — Integración UI del juego ↔ Scripting · PokeMotor

> **Para Claude Code.** El puente entre los scripts y la UI del juego: un script
> **presenta** UI y **espera** el resultado (corrutinas + pila de modos). Concreta
> la fase 2 del brief de Scripting. **Prioridad: implementar este primero** (de los
> dos de UI).

---

## Contexto

- **PokeMotor**: C++ / Vulkan, RPG estilo Pokémon, 2D.
- Este brief **conecta** dos sistemas:
  - **UI del juego** (widgets retenidos: `DialogueBox`, `Menu`, `UIRenderer`) — ver
    diseño `UIJuego`.
  - **Scripting** (Lua vía sol2; ver brief de Scripting) + la **pila de modos**
    (`GameMode` con `blocksUpdateBelow`/`blocksRenderBelow`).
- **Idea central:** la UI es algo que un script puede **esperar** ("await"). El
  script presenta una UI, se pausa, y se reanuda cuando el jugador termina con ella.

> **Claude Code:** adapta a los nombres reales (DialogueBox, Menu, GameMode/stack,
> el LuaVM del brief de Scripting).

---

## Prerrequisitos (deben existir o construirse antes)

- **Widgets:** `DialogueBox` (`show(text)`, avanzar, `isFinished()`), `Menu`
  (`options`, `onConfirm(idx)`, navegación por `ActionMap`). Del diseño `UIJuego`.
- **Pila de modos:** `DialogueMode` / `MenuMode` que poseen un widget y bloquean el
  mundo de abajo (`blocksUpdateBelow = true`).
- **Lua + corrutinas:** el `LuaVM` (sol2) y poder correr una función de script como
  **corrutina**.

---

## Objetivo

Que un script de evento se lea como un guion, presentando UI y esperando:

```lua
function on_interact(self)
    show_text("¡Hola! ¿Curo tu equipo?")     -- empuja DialogueBox, YIELD hasta avanzar
    local pick = show_choice({"Sí", "No"})     -- empuja Menu, YIELD hasta elegir
    if pick == 1 then
        heal_party()
        show_text("¡Listo!")
    else
        show_text("Vuelve cuando quieras.")
    end
end
```

---

## Arquitectura

### 1. Los scripts de evento corren como corrutinas

```cpp
struct RunningScript {
    sol::coroutine co;       // la función del script (on_interact, una cutscene...)
    bool        waiting = false;  // ¿esperando que una UI/acción termine?
    sol::object result;      // valor a devolver al reanudar (p.ej. índice del choice)
};
// g_runningScripts: las corrutinas de evento activas (normalmente 0 o 1).
```

### 2. Funciones de UI en el API (wrappers Lua que ceden)

Provistas por el motor en el preludio de los scripts de evento:

```lua
function show_text(text)
    __push_dialogue(text)      -- C++: empuja DialogueMode
    coroutine.yield()          -- pausa el script hasta que el diálogo termine
end

function show_choice(options)
    __push_choice(options)     -- C++: empuja un Menu
    return coroutine.yield()   -- pausa; al reanudar, devuelve el índice elegido
end
```

### 3. Las funciones C++ que empujan UI y marcan la espera

```cpp
// __push_dialogue (expuesto a Lua):
void push_dialogue(RunningScript& rs, const std::string& text) {
    auto box = std::make_shared<DialogueBox>();
    box->show(text);
    g_modes.push(std::make_unique<DialogueMode>(box, /*onDone=*/[&rs]{
        rs.waiting = false;                  // el diálogo terminó → reanudable
    }));
    rs.waiting = true;
}

// __push_choice:
void push_choice(RunningScript& rs, std::vector<std::string> opts) {
    auto menu = std::make_shared<Menu>(opts);
    menu->onConfirm = [&rs](int idx) {
        rs.result  = sol::make_object(lua, idx);   // el índice vuelve al script
        rs.waiting = false;
    };
    g_modes.push(std::make_unique<MenuMode>(menu));
    rs.waiting = true;
}
```

### 4. El scheduler que reanuda las corrutinas

```cpp
void ScriptCoroutineScheduler() {     // una vez por frame
    for (auto& rs : g_runningScripts) {
        if (rs.waiting) continue;            // sigue esperando una UI/acción
        if (!rs.co.runnable()) { /* terminó → quitar de la lista */ continue; }
        rs.co(rs.result);                    // reanuda (pasa el resultado al yield)
        rs.result = sol::nil;
    }
}
```

Flujo: trigger dispara el script → corre hasta el primer `show_text` (yield) →
`DialogueMode` arriba, mundo congelado → jugador avanza → `onDone` pone
`waiting=false` → el scheduler reanuda → el script sigue al siguiente comando.

---

## Generaliza más allá de la UI

El mismo patrón await sirve para todo lo secuenciado:

```lua
move_npc(npc, "Up", 3)   -- mueve y YIELD hasta que llegue
wait(1.0)                -- YIELD 1 segundo
start_battle("rival")    -- YIELD hasta que el combate termine
```

Cada uno: empuja/arranca una acción, marca `waiting`, cede; al terminar la acción,
`waiting=false` y el scheduler reanuda. La UI (`show_text`/`show_choice`) es el caso
más visible, pero el mecanismo es el mismo.

---

## Los tres tipos de UI (contexto, para no confundir)

- **Dirigida por script** (diálogos, elecciones): el script la presenta y la
  espera. **← este brief.**
- **Dirigida por el jugador** (menú, mochila): el jugador la abre; navega por
  `ActionMap`; las selecciones llaman a lógica del juego (no necesariamente un
  script).
- **Ligada a datos** (HUD): la UI lee el modelo (PS, ítems) para mostrarse.

Este brief cubre el primero. Los otros usan la misma librería de widgets.

---

## Milestones

1. Correr una función de script como corrutina; un `RunningScript` + el scheduler
   que la reanuda.
2. `show_text` (wrapper Lua + `__push_dialogue` + `DialogueMode`): un script muestra
   texto y se reanuda al avanzar.
3. `show_choice`: muestra opciones, el script recibe el índice y ramifica.
4. `wait(seconds)`: cede N segundos.
5. Un script de evento real (saludo + elección + ramas) corriendo de punta a punta.

---

## Criterios de aceptación

- [ ] Un trigger `interact` corre un script que hace `show_text` → aparece la caja,
      el mundo se congela, al avanzar el script continúa.
- [ ] `show_choice` devuelve el índice elegido; el script ramifica con `if`.
- [ ] Durante la UI, el overworld no se actualiza (la pila de modos lo bloquea).
- [ ] Encadenar `show_text` → `show_choice` → `show_text` funciona en secuencia.
- [ ] Un error en el script no tumba el motor (corrutina protegida → consola).

---

## Fuera de alcance — NO construir

- **La librería de widgets completa** (es prerrequisito; ver `UIJuego`).
- **El editor visual de UI** (brief aparte).
- **Triggers nuevos** más allá de los que ya existen (interact/step/enter).
- **Combate / mover NPC reales** si aún no existen: stubear `start_battle`/`move_npc`
  para probar el mecanismo await primero.

---

## Notas de diseño

- **La UI es algo que un script "espera".** El pegamento es corrutina + pila de
  modos; nadie maneja el árbol de widgets desde el script.
- **El resultado fluye de vuelta** por el valor de `coroutine.yield`
  (`show_choice` → índice).
- **Corrutinas protegidas:** un script roto va a la consola, no al crash.
- **Generaliza:** `wait`, `move_npc`, `start_battle` usan el mismo await.
