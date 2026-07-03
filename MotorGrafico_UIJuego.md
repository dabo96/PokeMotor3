# Motor Pokémon — Librería de UI del juego

> La UI para el jugador: cajas de diálogo, menús, HUD. Retenida, estilizada,
> navegable con mando. NO es la UI del motor (ImGui).

**Estado:** borrador de diseño · **Posición:** sobre las primitivas de render 2D (texto, 9-slice)

---

## UI del juego ≠ UI del motor

| | UI del juego | UI del motor (editor) |
|---|---|---|
| Para quién | el jugador | el desarrollador |
| Modo | **retenido** (árbol persistente) | immediate (ImGui) |
| Estilo | artístico, animado, estética Pokémon | funcional, denso |
| Navegación | acciones (mando/teclado), foco | ratón/teclado |
| En release | enviado con el juego | compilado fuera |

Por qué retenido: animación (typewriter, cursor que parpadea, caja que desliza),
foco/navegación y estilo persisten mejor en un árbol que construyes una vez.

---

## Dependencia: primitivas de render 2D

Esta librería se apoya en piezas del **plan 2D** marcadas como pendientes:

- **Texto** (atlas de glifos / BMFont),
- **9-slice** y **sprites en espacio de pantalla**.

La UI es la capa de arriba; esas primitivas son el suelo. Se construyen primero.

---

## Modelo de widgets

```cpp
// UI/Widget.h — nodo del árbol retenido
class Widget {
public:
    virtual ~Widget() = default;
    virtual void update(float dt) {}
    virtual void draw(UIRenderer& r) = 0;
    virtual bool handleInput(const UIInput& in) { return false; }  // ¿consumió?

    Rect rect;                                      // posición/tamaño en pantalla
    std::vector<std::unique_ptr<Widget>> children;
};
```

Widgets concretos (los que la demo necesita):

```cpp
class Panel : public Widget {        // caja 9-slice (diálogo, marco de menú)
    NineSlice background;
};

class Label : public Widget {        // texto
    std::string text;
    FontHandle  font;
    bool        typewriter = false;  // máquina de escribir
};

class Image : public Widget {        // icono, retrato, cursor
    TextureHandle texture;
    Vec4          uv;
};

class Menu : public Widget {         // lista navegable con cursor (núcleo interactivo)
    std::vector<std::string> options;
    int selected = 0;
    std::function<void(int)> onConfirm;
    std::function<void()>    onCancel;
    // handleInput: Up/Down mueve selected; Confirm → onConfirm; Cancel → onCancel
};
```

---

## La caja de diálogo (de primera clase)

El elemento más importante de un Pokémon. Teclea letra a letra, avanza al pulsar,
pagina, y puede ofrecer una elección. Conecta con `ShowText` del `EventRunner`.

```cpp
// UI/DialogueBox.h
class DialogueBox : public Widget {
public:
    void show(const std::string& text);
    void showChoice(const std::string& text, std::vector<std::string> opts,
                    std::function<void(int)> onPick);
    bool isFinished() const;

    void update(float dt) override;                // avanza el typewriter
    bool handleInput(const UIInput& in) override;  // Confirm avanza/cierra
    void draw(UIRenderer& r) override;
private:
    Panel       m_box;
    std::string m_full;
    size_t      m_revealed = 0;                    // chars mostrados
    float       m_timer = 0;
};
```

Integración con el `EventRunner`: `ShowText` empuja un `DialogueMode` con un
`DialogueBox`; al terminar, el modo hace `pop` y el runner reanuda.

---

## Navegación por acciones, no por ratón

La UI del juego se navega con **acciones semánticas** vía el `ActionMap` de Input;
nunca toca teclas físicas. Igual con teclado y con mando.

```cpp
struct UIInput { bool up, down, left, right, confirm, cancel; };  // flancos
```

---

## Render: el UIRenderer

Dibuja en espacio de pantalla emitiendo al `SpriteBatch`.

```cpp
// UI/UIRenderer.h
class UIRenderer {
public:
    void drawNineSlice(const NineSlice&, Rect);          // caja estirable
    void drawText(const std::string&, Vec2, FontHandle, Color);
    void drawSprite(TextureHandle, Rect, Vec4 uv);       // icono, retrato, cursor
    // por dentro: emite al SpriteBatch en coordenadas de pantalla
};
```

En el frame: **mundo → UI del juego → UI del editor → swapchain**.

---

## Integración con la pila de modos

```cpp
class MenuMode : public GameMode {
    std::unique_ptr<Widget> m_root;
    void update(float dt) override    { m_root->update(dt); /* + input → handleInput */ }
    void render(Renderer& r) override { /* m_root->draw vía UIRenderer */ }
    bool blocksUpdateBelow() const override { return true;  }  // congela el mundo
    bool blocksRenderBelow() const override { return false; }  // se ve detrás
};
```

Los flags `blocksUpdate/RenderBelow` (de la pila de modos) hacen el layering: el
menú congela el overworld pero lo deja verse detrás.

---

## Tema / estilo

Un `UITheme` mantiene el look consistente y swappable: el sprite de la caja
(9-slice), la fuente, el sprite del cursor, los colores. Data-driven donde tenga
sentido.

```cpp
struct UITheme {
    NineSlice boxFrame;
    FontHandle font;
    TextureHandle cursor;
    Color textColor, highlightColor;
};
```

---

## Estructura de carpetas

```
UI/                       # UI del JUEGO (distinta de Editor/)
├── Widget.h
├── Widgets.h             # Panel, Label, Image, Menu
├── DialogueBox.h
├── UIRenderer.h          # dibuja vía SpriteBatch en screen-space
├── UIInput.h             # acciones semánticas
└── UITheme.h
```

---

## Decisiones aún abiertas (para revisitar)

- **Layout:** posiciones fijas + VBox/HBox simples al inicio; nada de constraint
  solver. Anclaje a bordes de pantalla (diálogo abajo, HUD en esquina) es útil.
- **Animación:** typewriter, parpadeo de cursor, deslizado de caja — unos pocos
  tweens; no un sistema de animación completo.
- **Pantallas complejas** (resumen de Pokémon, mochila con pestañas): se arman con
  los mismos widgets cuando el juego las pida.
- **Y-sort / focus entre múltiples menús:** un menú activo a la vez + pila de
  pantallas basta para empezar.
```