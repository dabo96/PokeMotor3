# Motor Pokémon — Plan del renderer 2D

> Qué necesita un juego 2D del renderer, qué ya está diseñado, qué falta, y en qué
> orden construirlo. Enfocado a la demo 2D.

**Estado:** plan de implementación · **Posición:** zoom-in del camino 2D (fases 3-4 del índice)

---

## Qué tiene que mostrar un juego 2D → pieza del motor

| Lo que muestra | Pieza | Estado | Doc |
|---|---|---|---|
| Sprites (jugador, NPCs, objetos) | Sprite + SpriteBatch + material UnlitSprite + nearest | ✓ | 2DyCamara, Materiales |
| El mundo | Sistema de tiles + TileMapRenderer | ✓ | Tiles |
| Orden / capas | TileLayer order + above-player | ✓ | Tiles |
| Cámara que sigue | OrthographicCamera + FollowCameraController | ✓ | 2DyCamara |
| Píxeles nítidos | Target lowRes + upscale nearest | ✓ | 2DyCamara |
| Rendimiento (muchos sprites) | Batching | ✓ | 2DyCamara |
| **Animación de sprites** | Sistema de animación 2D | ✗ falta | — |
| **Texto** | Renderizado de fuentes | ✗ falta | — |
| **Cajas de UI / HUD** | Sprites screen-space + 9-slice | ✗ parcial | — |
| **Transiciones** | Quad fullscreen con efecto | ✗ menor | — |

**Lectura:** el núcleo del renderer 2D ya está diseñado. Faltan, por orden de
importancia: **animación de sprites**, **texto**, **cajas de UI** (+ transiciones,
menor).

---

## El plan, secuenciado

Cada paso deja algo en pantalla. Parte de lo ya diseñado.

### Paso 1 — Sprites estáticos + tilemap
No falta nada: ensamblar lo diseñado. SpriteBatch dibujando el mapa + un sprite
del jugador quieto, cámara ortográfica siguiéndolo, lowRes + upscale.
**Resultado:** el hito "tile en pantalla".

### Paso 2 — El jugador animado  ← primera pieza que falta
Sistema de animación 2D: hojas de sprites, clips (idle/walk por dirección), un
componente que avanza frames con el tiempo.
**Resultado:** de un muñeco quieto a uno que camina — "parece un juego".

### Paso 3 — Texto
Renderizado de fuentes (atlas de glifos: bitmap o BMFont). Imprescindible para
diálogos y menús.
**Resultado:** se puede mostrar texto en pantalla.

### Paso 4 — Cajas de UI
Sprites en espacio de pantalla + 9-slice (caja que estira sin deformar esquinas)
para diálogo y marcos de menú. Extensión del sistema de sprites.
**Resultado:** caja de diálogo y marcos de menú.

### Paso 5 — Transiciones
Fade a negro (entrar a edificio, iniciar combate). Quad fullscreen con efecto.
**Resultado:** pulido en los cambios de escena.

---

## Aclaración: renderer vs lógica

Esto es el lado del **renderer** (la capacidad de dibujar). La **lógica** que lo
usa —movimiento del jugador, lógica de diálogo, de menú— crece en paralelo del
lado de la lógica de juego (modos, eventos), ya diseñada. El renderer da las
herramientas; el juego las usa.

---

## Las tres piezas que faltan (a diseñar cuando toquen)

1. **Animación de sprites 2D** — hojas de sprites, clips, estados, un componente
   de animación. La más urgente (jugador caminando).
2. **Renderizado de texto/fuentes** — atlas de glifos, layout de texto, BMFont.
3. **UI en espacio de pantalla + 9-slice** — sprites en coordenadas de pantalla,
   cajas estirables. Extensión del sprite system.

(Transiciones es un quad fullscreen, no necesita diseño aparte.)

---

## Decisiones aún abiertas (para revisitar)

- **Formato de animación:** clips definidos en datos (JSON) vs en código. Datos,
  para iterar sin recompilar.
- **Fuentes: bitmap vs SDF.** Bitmap (BMFont) es perfecto para pixel-art y más
  simple; SDF escala mejor pero es overkill para un look retro.
- **Y-sorting de entidades:** si NPCs y objetos necesitan ordenarse por Y entre sí;
  refinamiento sobre las capas.
```