# Motor Pokémon — Pipeline 2D clásico y cámara

> Cierre del frame-rendering de la mitad gráfica. El pipeline 2D clásico (modo
> Game Boy) y el sistema de cámara. Juntos hacen el camino 2D testeable de punta
> a punta. Backend: Vulkan.

**Estado:** borrador de diseño

---

## Por qué estas dos piezas

- El **2D clásico** es el camino más corto a píxeles en pantalla → banco de
  pruebas de toda la arquitectura antes de tocar el 3D.
- La **cámara** es el punto de vista; sin ella nada de lo renderizado es visible.

Son pequeñas porque el trabajo pesado (render graph, materiales) ya está hecho.

---

## Pipeline 2D clásico

### El grafo más simple

```
Modo 3D / HD-2D:
  [ Shadow ]─►[ Main ]─►[ Bloom ]─►[ DoF ]─►[ Tonemap ]─► swapchain

Modo 2D clásico:
  [ Sprite2DPass (ortho) ]─► lowRes (320×240) ─►[ Upscale nearest ]─► swapchain
```

El contraste valida la arquitectura: cada modo es un grafo distinto, sin tocar
el motor.

### Resolución interna baja + upscale nearest

Para pixel-art auténtico: renderizar a una resolución interna baja y subirla con
filtro **nearest** a la ventana. Píxeles consistentes y crujientes a cualquier
tamaño de pantalla. Es el look retro de verdad.

### El corazón: batching de sprites

Convertir miles de tiles/sprites en pocos draw calls. Una capa de tilemap (un
solo atlas) = un solo draw call.

```cpp
struct Sprite {
    Vec2 position, size;
    Vec4 uvRect;            // x,y,w,h en el atlas (0..1)
    Vec4 color = {1,1,1,1};
    int  layer = 0;         // orden de dibujo
    TextureHandle atlas;
};

class SpriteBatch {
public:
    void begin(const Camera& cam);
    void draw(const Sprite& s);          // solo acumula
    void end(RenderPassContext& ctx);    // ordena por (layer, atlas), flushea
private:
    std::vector<Sprite> m_sprites;
    // buffer de vértices dinámico; cada run de mismo atlas+layer = 1 draw call
};
```

`end()`: ordena por capa y atlas; cada tramo contiguo con mismo atlas+capa se
construye como un buffer de quads y se dibuja de una.

### Conexión con la lógica de juego

El `Sprite2DRenderer` lee el `TileMap` (grilla de `Tile` de la mitad de juego);
cada `Tile.visualId` indexa en el atlas para emitir un sprite al batch.

```cpp
class Sprite2DPass {
public:
    void setup(RenderGraph& graph, GraphTexture target,
               const Camera& cam, const SpriteBatch& batch);
    // ortográfico, escribe target, corre el batch
};
```

Usa la familia de material **UnlitSprite** (pieza 3). El mismo `SpriteBatch` es
además la base de los billboards HD-2D, la UI y las partículas: no es solo para
el modo clásico.

---

## Cámara

### Una interfaz, dos implementaciones

El renderer solo pide `viewProjection()`; no le importa la clase concreta. Mismo
patrón que el pass agnóstico a materiales.

```cpp
class Camera {
public:
    virtual Mat4 view() const = 0;
    virtual Mat4 proj() const = 0;
    Mat4 viewProjection() const { return proj() * view(); }
    virtual ~Camera() = default;
};

class OrthographicCamera : public Camera {   // 2D clásico
    Vec2  m_center;
    float m_zoom = 1.0f;
    Vec2  m_viewport;
public:
    Mat4 view() const override;   // traslación al centro
    Mat4 proj() const override;   // ortho viewport / zoom
};

class PerspectiveCamera : public Camera {    // HD-2D y 3D
    Vec3  m_position, m_target, m_up;
    float m_fov, m_near, m_far, m_aspect;
public:
    Mat4 view() const override;   // lookAt(position, target, up)
    Mat4 proj() const override;   // perspective(fov, aspect, near, far)
};
```

### Separar cámara (matrices) de controlador (comportamiento)

La cámara solo produce matrices. Cómo se mueve (seguir, suavizar, recortar a los
bordes) vive en un controlador aparte → cambiar el comportamiento no toca la
matemática.

```cpp
class FollowCameraController {
public:
    void setTarget(EntityId target);
    void update(Camera& cam, const Scene& scene, float dt);
private:
    Vec3  m_offset;       // en HD-2D: ángulo y altura del tilt
    float m_smoothing;    // lerp para que no sea brusca
    Rect  m_bounds;       // límites del mapa
};
```

### Flujo: la pila de modos configura la cámara

```
ModoActivo ──configura──► Camera (ortho o perspectiva)
                              │ viewProjection()
                              ▼
        FrameUniforms (set 0, 3D)   /   proj del Sprite2DPass (2D)
```

- `OverworldMode` → `FollowCameraController` sobre el jugador.
- `BattleMode` → cámara de combate.
- **HD-2D:** `PerspectiveCamera` con ángulo picado y FOV estrecho (aplana la
  perspectiva). Junto al tilt-shift del post-proceso, da el efecto maqueta. La
  cámara y el DoF trabajan juntos.

---

## El hito que desbloquea

Camino 2D clásico testeable de punta a punta:

```
cámara ortográfica → SpriteBatch → Sprite2DPass → upscale → pantalla
```

Dibujar un tilemap con el sprite del jugador y mover la cámara que lo sigue
prueba toda la pila (ventana, swapchain, render graph, pass, material, cámara)
antes de escribir un solo shader de sombras. El "valida con lo más simple
primero", hecho realidad.

---

## Estructura de carpetas

```
Renderer/
├── 2D/
│   ├── Sprite.h
│   ├── SpriteBatch.h
│   ├── Sprite2DRenderer.h   # TileMap → sprites
│   └── Sprite2DPass.h
├── Camera/
│   ├── Camera.h             # interfaz
│   ├── OrthographicCamera.h
│   ├── PerspectiveCamera.h
│   └── FollowCameraController.h
└── Passes/
    └── UpscalePass.h        # lowRes → swapchain, nearest
```

---

## Decisiones aún abiertas (para revisitar)

- **Resolución interna concreta** (320×240, 480×270…): elección estética.
- **Capas de parallax / tiles animados:** extensiones fáciles sobre el batch.
- **Transiciones de cámara** entre modos (overworld→combate): blend de matrices,
  refinamiento posterior.
- **Camera shake / efectos** (golpes en combate): sobre el controlador.
