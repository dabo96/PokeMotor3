# Brief de implementación — SpriteRenderSystem (render-feed ECS → SpriteBatch) · PokeMotor

> **Para Claude Code.** El puente que hace **visibles** a las entidades del ECS:
> cada frame, `view<Transform, SpriteComponent>()` → emite al `SpriteBatch`
> existente. Pieza pequeña pero foundational. **Va junto al ECS** (cierra la base:
> "ver una entidad" llega con la fundación).

---

## El gap que cierra

El render 2D actual (OverworldMode) **no lee del ECS**. Sin este sistema:
- El **SceneManager** carga entidades → invisibles.
- **Assets** suelta sprites → invisibles.
- Sus criterios ("las entidades vuelven con su sprite", "el sprite soltado se ve")
  no se cumplen.

Este sistema conecta los datos del ECS con la maquinaria de render que **ya
existe** (SpriteBatch, Sprite2DPass, cámara, target lowRes). **No** crea render
nuevo; es el feed.

> Realiza la frontera de diseño "la Scene produce renderables, el renderer los
> consume", concretada para 2D.

---

## Objetivo

Una entidad con `Transform` + `SpriteComponent` se **dibuja** en pantalla, en su
posición, con su textura, capa, tinte y opacidad.

---

## El sistema

```cpp
void SpriteRenderSystem(Scene& scene, SpriteBatch& batch) {
    for (Entity e : scene.view<Transform, SpriteComponent>()) {
        auto& tr = scene.get<Transform>(e);
        auto& sp = scene.get<SpriteComponent>(e);
        Sprite s;
        s.texture  = sp.tex;
        s.position = tr.position;     // o tr.visualPos si hay tween de rejilla
        s.layer    = sp.layer;        // Capa
        s.tint     = sp.tint;         // Apariencia: Tinte
        s.opacity  = sp.opacity;      // Apariencia: Opacidad
        batch.submit(s);              // adapta al nombre real del método del batch
    }
}
```

> **Apariencia:** `tint`/`opacity`/`layer` son los campos del panel "Apariencia"
> que ya muestra tu inspector. Si en tu motor viven en un componente aparte
> (p. ej. `Appearance`) en vez de dentro de `SpriteComponent`, leer de ese — el
> sistema solo necesita textura + posición + capa + tinte + opacidad.

---

## Integración con el render existente

- **Cablear en el camino de render 2D:** donde hoy se dibuja la escena
  (OverworldMode / el renderer), llamar a `SpriteRenderSystem(scene, batch)` antes
  de que el `Sprite2DPass` vacíe el batch. Así las entidades del ECS se dibujan.
- **Capas / orden:** el `SpriteBatch` ordena por `layer`. Los sprites de entidades
  y los del **tilemap** van al mismo batch → se intercalan correctamente (el
  jugador entre las capas below-player y above-player del mapa). Respetar el campo
  `layer`.
- **Cámara:** la aplica el pass existente, no el feed. El feed emite en espacio de
  mundo.
- **Culling (v2):** saltar sprites fuera de la vista de la cámara, como hace el
  `TileMapRenderer`. v1 emite todos.

---

## Milestones

1. `SpriteRenderSystem` itera `view<Transform, SpriteComponent>()` y hace `submit`
   al batch.
2. Cablearlo en el camino de render → las entidades del ECS aparecen en pantalla.
3. Respetar `layer` (entidades ordenadas con las capas del tilemap).
4. `tint` + `opacity` desde los campos de Apariencia.
5. (v2) culling por cámara.

---

## Criterios de aceptación

- [ ] Una entidad creada en código con `Transform` + `SpriteComponent` **se ve** en
      pantalla, en su posición.
- [ ] **Assets:** el sprite soltado (brief de Assets) queda **visible** (cierra ese
      criterio).
- [ ] **SceneManager:** guardar + recargar → las entidades vuelven **visibles** con
      su sprite (cierra ese criterio).
- [ ] Un sprite en una capa above-player se dibuja **por encima** del jugador.
- [ ] Tinte y opacidad editados en el inspector se reflejan en el render en vivo.

---

## Fuera de alcance — NO construir

- **Animación de sprites** (hojas, clips): sistema aparte, después.
- **Render del tilemap:** ya existe; este sistema solo añade las entidades.
- **Maquinaria de render nueva** (SpriteBatch, pass, cámara): ya existe; esto es el
  feed.
- **3D / HD-2D.**

---

## Notas de diseño

- Es un **puente, no render nuevo.** Lee ECS → alimenta el batch existente.
- Es un **sistema** (corre en el bucle de render), no un componente.
- Va **junto al ECS**: sin él, el ECS produce entidades invisibles y Assets /
  SceneManager no cumplen sus criterios.
- Mantiene el **desacople** Scene↔renderer: el renderer no conoce el ECS; recibe
  sprites por el batch.
