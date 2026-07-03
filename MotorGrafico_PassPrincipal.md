# Motor Pokémon — Pass principal y estrategia de iluminación

> Pieza 2 de la mitad gráfica (tras el render graph). Define cómo se dibuja la
> escena 3D dentro del grafo y qué estrategia de iluminación se usa. Backend: Vulkan.

**Estado:** borrador de diseño · **Decisión:** forward shading, diseñado para crecer a forward+

---

## Orden de diseño de la mitad gráfica

```
1. Pass principal + estrategia de iluminación   ◄── ESTE DOCUMENTO
2. Sistema de materiales (PBR + sprites HD-2D)
3. Luces y sombras
4. Cadena de post-proceso (bloom, tilt-shift)
   (+ pipeline 2D clásico — pieza pequeña, independiente)
```

Cada pieza depende de la anterior: la estrategia de iluminación dicta la
estructura del pass principal y qué deben aportar los materiales; los materiales
definen los datos de superficie; luces y sombras se construyen sobre ambos; el
post-proceso opera sobre la imagen ya iluminada.

---

## La decisión: forward vs forward+ vs deferred

| Estrategia | Transparencia | Muchas luces | Complejidad | MSAA | Memoria |
|---|---|---|---|---|---|
| **Forward**  | natural             | costoso    | baja  | fácil   | baja        |
| **Forward+** | natural             | eficiente  | media | fácil   | baja-media  |
| **Deferred** | mala (pass aparte)  | excelente  | alta  | difícil | alta (G-buffer) |

### Veredicto: forward, diseñado para crecer a forward+

**Deferred queda descartado** por dos razones que pegan justo en este proyecto:

1. **Transparencia.** HD-2D son sprites billboard con bordes alpha, follaje y
   partículas. Deferred maneja la transparencia mal y obliga a un pass forward
   aparte solo para transparentes.
2. **Luces.** El superpoder de deferred (miles de luces baratas) es lo único que
   un juego estilo Pokémon no necesita: sol + un puñado de luces locales por escena.

**Forward plano para empezar:** mucho más simple de entender e implementar (sin
layout de G-buffer, sin afinar ancho de banda, sin pass de transparencia
separado), maneja HD-2D de forma natural, y MSAA trivial.

**Nota sobre el listón visual:** para "verse mejor que Switch", forward no es el
cuello de botella. El techo lo ponen materiales, sombras y post-proceso, no la
estrategia de iluminación.

---

## El pass principal dentro del grafo

```
[ shadow ] ──shadowMap──►┌─────────────────┐──► sceneColor (HDR)
                         │  PASS PRINCIPAL  │
[ luces (buffer) ]──────►│    (forward)     │──► depth
                         └─────────────────┘
                                  │
                                  ▼  (lo consume el post-proceso)
```

### Decisiones del pass

- **Color HDR** (`R16G16B16A16_SFLOAT`), no el formato del swapchain. El bloom y
  el tonemapping necesitan rango más allá de 1.0. El tonemap se hace en post.
- **Orden de dibujo:** opacos primero (depth test, cualquier orden), luego
  transparentes ordenados de atrás hacia delante con alpha blending. Natural en
  forward; necesario para los sprites HD-2D.
- **Depth prepass:** opcional, futuro. Reduce overdraw en la escena pesada. Con
  render graph es solo otro pass — añadirlo después es trivial.

---

## Clases

```cpp
// Renderer/Lighting/Light.h
enum class LightType { Directional, Point, Spot };

struct Light {
    LightType type;
    Vec3  position;    // point / spot
    Vec3  direction;   // directional / spot
    Vec3  color;
    float intensity;
    float range;       // point / spot
};

// Datos por frame
struct FrameUniforms {
    Mat4 view, proj;
    Vec3 cameraPos;
    uint32_t lightCount;     // + buffer de luces enlazado aparte
};

// Objeto a dibujar (viene de Scene::renderables → DrawItem)
struct DrawItem {
    MeshHandle     mesh;
    MaterialHandle material;
    Mat4           transform;
    bool           transparent;   // decide el bucket
};
```

```cpp
// Renderer/Passes/MainPass.h
class MainPass {
public:
    void setup(RenderGraph& graph,
               GraphTexture shadowMap,      // entrada
               GraphTexture sceneColorOut,  // salida (HDR)
               GraphTexture depthOut)       // salida
    {
        graph.addPass("main", [=](RenderGraphBuilder& b) {
            b.read(shadowMap);
            b.write(sceneColorOut);
            b.write(depthOut);
            return [=](RenderPassContext& ctx) {
                bindFrameUniforms(ctx, m_view);     // cámara + buffer de luces
                bindShadowMap(ctx, shadowMap);

                for (const DrawItem& it : m_opaque)        // 1) opacos
                    drawItem(ctx, it);

                sortBackToFront(m_transparent, m_camera);  // 2) transparentes
                for (const DrawItem& it : m_transparent)
                    drawItem(ctx, it);                     //    alpha blending
            };
        });
    }
private:
    std::vector<DrawItem> m_opaque, m_transparent;
    RenderView m_view;
    Camera     m_camera;
};
```

La iluminación real ocurre en el fragment shader de cada material: muestrea la
superficie (albedo, normal, rugosidad, metalicidad), recorre las luces
acumulando contribución PBR, muestrea la sombra, y emite color HDR. Ese "qué
muestrea la superficie" es el sistema de materiales (pieza 2).

---

## Previsión: forward → forward+ (aditivo)

```
Forward (hoy):
   [ shadow ] ─► [ main: por fragmento recorre TODAS las luces ] ─► sceneColor

Forward+ (mañana):
   [ shadow ] ─► [ light cull ]─clusters─► [ main: recorre solo las del cluster ] ─► sceneColor
                       ▲
               pass nuevo; el resto del grafo no cambia
```

Para que el salto sea aditivo y no una reescritura, dos disciplinas ya en el
diseño:

1. Mantener las luces en un **buffer** (no hardcodeadas en el shader).
2. Mantener el **bucle de iluminación aislado** en el shader.

Con eso, forward+ = un `LightCullPass` nuevo (ordenado solo por el render graph)
+ un cambio en sobre qué luces itera el shader. El `MainPass` en C++ casi no
cambia.

---

## Estructura de carpetas

```
Renderer/
├── Passes/
│   └── MainPass.h
├── Lighting/
│   ├── Light.h
│   └── LightBuffer.h        # las luces en un buffer de GPU
└── (Graph/, VulkanContext, Renderer...)
```

---

## Decisiones aún abiertas (para revisitar)

- **Depth prepass:** añadirlo cuando el overdraw aparezca en el perfilado.
- **Sombras:** cuántas cascadas para el sol, resolución del shadow map → se
  decide en la pieza 3 (luces y sombras).
- **Tonemapper concreto** (ACES, Reinhard, etc.) → se decide en la pieza 4 (post).
