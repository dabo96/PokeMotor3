# Brief de implementación — Editor de UI in-engine (UI Builder) · PokeMotor

> **Para Claude Code.** Una herramienta visual para componer la UI del juego
> reusando el editor que ya existe. **Es v2 / productividad** — leer primero la
> sección de disciplina.

---

## Disciplina (leer primero)

- Esto **NO es lo primero.** La librería de widgets + la integración UI↔script
  (brief aparte) es lo load-bearing. Este editor es una herramienta de
  productividad que se añade **cuando autorar UI a mano empiece a doler**.
- **v1 = autorar UI en código o en un `.ui` JSON a mano.** Con eso el juego ya
  tiene menús y diálogos.
- **NO construir UMG.** Nada de solucionadores de layout generales, grafos de
  binding, cascadas de estilos. Enfocado a los pocos widgets de un Pokémon.

---

## Contexto: ya tienes la maquinaria

Un editor de UI es tu **editor actual apuntado a los widgets** en vez de a las
entidades. Lo que ya existe y se reusa:

- **Drag-drop** (arrastrar al viewport) → arrastrar widgets de una paleta al canvas.
- **Inspector** (campos por reflexión) → propiedades del widget seleccionado.
- **Jerarquía** → el árbol de widgets.
- **Serialización de escenas** → guardar el árbol de UI a un archivo.

> **Claude Code:** reusa esos paneles/mecanismos; no los reconstruyas.

---

## Objetivo

Componer una UI visualmente (arrastrar widgets a un canvas, editar props, ordenar)
y guardarla como un **asset de UI** (`main_menu.ui`). El juego lo carga e instancia.

---

## Arquitectura — piezas

### 1. Asset de UI (`.ui`, JSON) — árbol de widgets serializado

Reusa el patrón de serialización de escenas. **Los widgets tienen nombre** (para
que el código/scripts los encuentren).

```json
{
  "root": {
    "type": "Panel", "name": "hud", "rect": [0,0,256,48],
    "children": [
      { "type": "Label", "name": "hp_label", "text": "HP: 100", "font": "main" }
    ]
  }
}
```

### 2. Panel canvas

Muestra la UI a **resolución objetivo**. Es el **drop target** de los widgets y
donde se seleccionan/mueven.

### 3. Paleta de widgets

Lista los tipos (`Panel`, `Label`, `Image`, `Menu`, `DialogueBox`). Arrastrar uno
al canvas lo crea (mismo origen-de-arrastre que los assets).

### 4. Inspector (reusado)

Edita las props del widget seleccionado (texto, sprite, color, rect, nombre). Por
reflexión, como los componentes.

### 5. Jerarquía (reusada)

El árbol de widgets (un `Label` dentro de un `Panel`). Seleccionar ↔ inspector,
como con las entidades (`g_selected`, pero para widgets).

### 6. Loader en runtime

```cpp
std::unique_ptr<Widget> loadUI(const std::string& path);  // .ui → árbol de widgets
// el juego lo muestra con un MenuMode/screen
```

### 7. Widgets con nombre → binding con código/scripts

```lua
local hp = ui:get("hp_label")        -- el widget autorado en el editor
hp:set_text("HP: " .. player.hp)
```

Así lo que dibujas visualmente queda enganchado a la lógica sin cablear a mano.

---

## Por qué la tuya es ligera

UMG / UI Builder pesan por ser **de propósito general**. Tu UI de Pokémon es
simple y estilizada: pocos tipos de widget, layouts casi fijos, navegación por
mando. Enfocar el editor a eso lo hace mucho más ligero. Lo "no engorroso" sale de
ser específico.

---

## Milestones

1. Loader: cargar un `.ui` a mano → instanciar el árbol → mostrarlo. (Sin editor
   aún: valida el formato y el runtime.)
2. Canvas que muestra el árbol cargado a resolución objetivo.
3. Selección de widgets en el canvas + inspector editando sus props.
4. Paleta + drag-drop: añadir widgets al canvas.
5. Jerarquía del árbol de widgets + serialización (guardar `.ui`).
6. Widgets con nombre + `ui:get(name)` para el binding.

> El milestone 1 (loader + autorar a mano) ya te da UI usable. El editor visual
> (2-6) es lo opcional.

---

## Criterios de aceptación

- [ ] Un `.ui` escrito a mano se carga y se muestra correctamente en el juego.
- [ ] En el editor: arrastrar un `Label` al canvas, editar su texto, guardarlo, y
      al recargar persiste.
- [ ] Anidar widgets (Label dentro de Panel) se refleja en la jerarquía.
- [ ] `ui:get("hp_label"):set_text(...)` actualiza el widget en runtime.

---

## Fuera de alcance — NO construir

- **Solucionador de layout general** (flexbox/constraints): posiciones/rects
  simples + anclaje a bordes basta.
- **Grafos de data-binding** estilo UMG.
- **Cascadas de estilos.**
- **Animación de UI** avanzada (más allá de los tweens simples del widget library).
- Cualquier cosa antes de que autorar a mano duela de verdad.

---

## Notas de diseño

- **Es el editor que ya tienes, apuntado a widgets.** Reuso, no sistema nuevo.
- **Asset `.ui` = árbol serializado**, como una escena para widgets.
- **Widgets con nombre** = el puente a código/scripts.
- **v1 es autorar a mano;** el editor visual llega cuando el volumen lo pida.
