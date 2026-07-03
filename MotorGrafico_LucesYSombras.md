# Motor Pokémon — Luces y sombras

> Pieza 4 de la mitad gráfica. Llena el buffer de luces que el set 0 expone y
> genera el shadow map que PBR y HD-2D muestrean. Backend: Vulkan.

**Estado:** borrador de diseño · **Decisión clave:** CSM para el sol; sombras de luces locales aplazadas

---

## Dos sub-sistemas, ambos alimentan el set 0

1. **Luces** → recolectadas en un buffer de GPU (set 0).
2. **Sombras** → generadas por un `ShadowPass` que escribe el shadow map, leído
   por el `MainPass` (set 0). Es el ejemplo shadow→main del render graph, ya real.

---

## Luces

Papeles para un juego estilo Pokémon:

- **Direccional**: sol/luna. Una por escena, la luz clave, proyecta la sombra principal.
- **Puntual**: antorchas, faroles, cristales de cueva, VFX de combate. Varias por escena.
- **Spot**: raras (faro, linterna). Opcionales.

Las luces son **dato de la escena** (una antorcha es una luz en los datos del mapa).
Cada frame se recolectan en un buffer de GPU:

```cpp
// Renderer/Lighting/LightBuffer.h
class LightBuffer {
public:
    void     collect(const Scene& scene);  // luces activas
    VkBuffer gpuBuffer() const;             // se enlaza en el set 0
    uint32_t count() const;
private:
    std::vector<Light> m_lights;            // tope p.ej. 32 (forward+ lo quita)
};
```

### No olvidar el ambiente

Sin término ambiente, lo que está en sombra queda negro puro. Lo mínimo decente:
**ambiente hemisférico** (color de cielo arriba, de suelo abajo), barato y bueno
en exteriores. IBL para PBR de verdad = mejora futura. El ambiente va en el set 0.

---

## Sombras: el problema del mundo abierto

Shadow mapping básico: dibujar la profundidad de la escena desde la luz en una
textura; en el pass principal, cada fragmento se compara contra ella.

**Problema:** un solo shadow map no cubre un overworld grande con buena
resolución. Las sombras cercanas salen pixeladas.

**Solución: Cascaded Shadow Maps (CSM).** Partir el rango de profundidad de la
cámara en franjas (cascadas), cada una con su shadow map: cercanas con mucho
detalle, lejanas con menos.

```
        cámara
          │
          ▼
   ┌──┬─────┬──────────┬─────────────────────┐
   │C0│ C1  │   C2     │         C3           │
   └──┴─────┴──────────┴─────────────────────┘
   cerca                                  lejos
```

En el render graph:

```
[ ShadowPass ]──shadowMap (array de N cascadas)──►[ MainPass ]──► sceneColor
   dibuja SOLO profundidad                          PCF + elige cascada por profundidad
   (mallas + billboards HD-2D)
```

### Decisión de alcance

**CSM para el sol = esencial. Sombras de luces locales = aplazadas.** Las
puntuales necesitan cube maps (caros, complejos) y la mayoría de antorchas se ven
bien sin proyectar sombra. Clavar una cosa antes que dos a medias.

---

## El pass de sombras

Depth-only (sin shading ni color), barato por draw. Dibuja la escena N veces (una
por cascada):

```cpp
// Renderer/Passes/ShadowPass.h
class ShadowPass {
public:
    GraphTexture setup(RenderGraph& graph, const Scene& scene,
                       const Camera& cam, const Light& sun)
    {
        computeCascades(cam, sun);   // splits + matriz luz-espacio por cascada

        GraphTexture shadowMap = graph.createTexture(
            {2048, 2048, VK_FORMAT_D32_SFLOAT /* array de N capas */});

        graph.addPass("shadow", [=](RenderGraphBuilder& b) {
            b.write(shadowMap);
            return [=](RenderPassContext& ctx) {
                for (int c = 0; c < m_cascadeCount; ++c) {
                    beginCascade(ctx, c);                  // capa c, matriz c
                    for (const DrawItem& it : m_casters)   // mallas Y billboards HD-2D
                        drawDepthOnly(ctx, it);
                }
            };
        });
        return shadowMap;
    }
private:
    Mat4  m_cascadeViewProj[4];
    float m_cascadeSplits[4];
    int   m_cascadeCount = 4;
    std::vector<DrawItem> m_casters;
};
```

Cálculo de cascadas (no detallado): partir el rango de profundidad con esquema
mixto lineal-logarítmico, y ajustar una caja ortográfica en espacio de luz a cada
franja → una `cascadeViewProj` por cascada.

### Set 0 extendido

```cpp
struct FrameUniforms {
    Mat4 view, proj;
    Vec3 cameraPos;
    Vec3 ambient;                 // hemisférico
    uint32_t lightCount;          // + LightBuffer aparte

    Mat4  cascadeViewProj[4];     // matriz luz-espacio por cascada
    float cascadeSplits[4];       // corte de cada cascada
    uint32_t cascadeCount;
};
```

### Muestreo en el fragment shader

1. Elegir la cascada según la profundidad del fragmento.
2. Transformarlo a ese espacio de luz.
3. **PCF** (varias muestras promediadas) → borde suave, no escalón pixelado.
4. Devuelve un factor [0,1] que multiplica la contribución de la luz.

Más un **bias** para evitar shadow acne (auto-sombreado). Es tuning, no arquitectura.

---

## Detalle HD-2D: los sprites proyectan sombra

En el pass de sombras, `m_casters` incluye **mallas y billboards HD-2D**. En
materiales el sprite *recibe* sombra; aquí también la *proyecta*. Un Pokémon que
arroja su sombra sobre el suelo 3D es lo que lo ancla al mundo.

Arruga técnica: el billboard mira a la cámara, pero para la sombra se dibuja
desde la luz → se resuelve con orientación fija para el casting o aceptando la
aproximación. Detalle, no rediseño.

---

## Estructura de carpetas

```
Renderer/
├── Lighting/
│   ├── Light.h
│   ├── LightBuffer.h     # recolecta luces de la escena → GPU
│   └── Ambient.h         # hemisférico (IBL futuro)
└── Passes/
    └── ShadowPass.h      # CSM, depth-only
```

---

## Decisiones aún abiertas (para revisitar)

- **Nº de cascadas y resolución:** empezar con 4 cascadas a 2048². Ajustar por
  perfilado y por cómo se vea el overworld.
- **Sombras de luces locales:** cube maps para puntuales, solo si un escenario
  (p.ej. cueva con antorchas) lo pide de verdad.
- **Ambiente IBL:** environment map para PBR de verdad, mejora futura sobre el
  hemisférico.
- **Filtrado avanzado** (PCSS para penumbras suaves): muy a futuro.
