# Motor Pokémon — Sistema de materiales

> Pieza 3 de la mitad gráfica (tras render graph y pass principal). Define "qué
> muestrea una superficie y cómo recibe luz", unificando materiales PBR (3D),
> sprites HD-2D y sprites planos (clásico/UI). Backend: Vulkan.

**Estado:** borrador de diseño

---

## El problema

Tres tipos de superficie muy distintos bajo un mismo techo, sin `if/else`:

- **PBR** (3D completo): albedo, normales, metal-rugosidad, iluminación física, suave.
- **Sprite HD-2D**: pixel-art que sigue viéndose pixelado pero recibe luz y
  sombras reales del mundo 3D.
- **Sprite unlit** (Game Boy clásico, UI, partículas): plano, sin iluminación.

---

## La idea central: el material es dato, el pass es agnóstico

Un `Material` no es código: es **dato que referencia un Shader** + sus parámetros
y texturas. El pass principal enlaza lo que el material diga y dibuja, sin saber
de qué familia es.

```cpp
// Dentro del MainPass — IDÉNTICO para PBR, HD-2D y unlit
void drawItem(RenderPassContext& ctx, const DrawItem& it) {
    const Material& mat = m_materials.get(it.material);
    const Shader&   sh  = m_shaders.get(mat.shader);

    ctx.bindPipeline(sh.pipeline());     // estado y shaders de SU familia
    ctx.bindSet(1, mat.set1);            // params + texturas del material
    ctx.pushConstants(it.transform);     // transform del objeto
    ctx.draw(m_meshes.get(it.mesh));
}
```

Esas cinco líneas dibujan una roca PBR, un sprite HD-2D o un tile clásico sin
cambiar nada. La diferencia vive dentro de `sh.pipeline()`. **El motor las trata
igual; los shaders las hacen distintas.**

---

## Principio organizador: frecuencia de enlace (descriptor sets)

Organizar los datos por cada cuánto cambian:

| Set | Frecuencia | Contenido | Quién lo enlaza |
|---|---|---|---|
| **0** | por frame | cámara, luces, shadow map | el `MainPass`, una vez |
| **1** | por material | parámetros (UBO) + texturas | al dibujar cada material |
| **2** / push | por objeto | transform | en cada draw |

El **set 0 son las `FrameUniforms` del pass principal** (cámara + buffer de luces
+ sombra), enlazadas una vez. Por eso todo material tiene acceso a las luces
aunque no las pida: los shaders iluminados las usan, el unlit las ignora. El
material nunca toca luces directamente.

---

## Las tres familias

```
   DrawItem ──►┌───────────────────────────────┐
               │  MainPass (forward)           │ set 0: cámara + luces + sombra
               │  — agnóstico a la familia     │
               └───────────────┬───────────────┘
                               │ por objeto: bind shader + set 1 + transform
              ┌────────────────┼────────────────┐
              ▼                ▼                 ▼
        Shader PBR        Shader HD-2D      Shader Unlit
        (lit, BRDF)       (lit, pixel)      (sin luz)
        albedo/normal/    sprite NEAREST    sprite NEAREST
        metal-rough       + recibe luz      paleta, plano
        (linear)          + recibe sombra
              └────────────────┴─────────────────┘
                  todos leen el set 0 · todos escriben sceneColor (HDR)
```

El sprite unlit es además el material del pipeline 2D clásico y el de UI y
partículas en 3D. Una familia, varios usos.

---

## El truco HD-2D: crujiente pero iluminado

Un sprite HD-2D parece pixel-art que vive en el mundo 3D. Sale de combinar en su
shader:

1. **Filtro NEAREST** (punto), nunca bilineal → píxeles duros, sin difuminar.
2. **Alpha-clip** en el borde (no blending) → bordes nítidos y **escribe
   profundidad**, así se ordena con la geometría 3D.
3. **Billboard**: el quad mira a la cámara (lo orienta el vertex shader).
4. **Recibe iluminación**: el fragment shader recorre las luces del set 0 y
   muestrea el shadow map, con difuso suavizado (half-lambert) para que la luz no
   se vea dura sobre el pixel-art.

Resultado: el mismo sprite pixelado se oscurece en sombra, se tiñe de la luz
cercana, y recibe las sombras de la geometría 3D. Crujiente e integrado. Es otra
familia de material, no un sistema aparte.

---

## Clases

```cpp
// Renderer/Material/Shader.h — programa + estado, COMPARTIDO
enum class MaterialFamily { PBR, HD2DSprite, UnlitSprite };

class Shader {
public:
    MaterialFamily        family() const;
    VkPipeline            pipeline() const;
    VkPipelineLayout      layout() const;
    VkDescriptorSetLayout materialSetLayout() const;   // layout del set 1
};
```

```cpp
// Renderer/Material/Material.h — INSTANCIA: referencia un Shader + sus datos
struct Material {
    ShaderHandle shader;                  // PBR / HD-2D / Unlit

    Vec4  baseColor = {1,1,1,1};          // parámetros → UBO del set 1
    float roughness = 1.0f;
    float metallic  = 0.0f;

    TextureHandle albedo, normal, metalRough;   // texturas → bindings del set 1

    SamplerFilter filter = SamplerFilter::Linear; // Nearest = pixel-art crujiente

    VkDescriptorSet set1;                 // construido al crear el material
};
```

Los materiales son **dato cargable de archivo** (shader, texturas, params,
filtro), no hardcodeados — mismo principio data-driven que la `Database` del juego.

---

## Estructura de carpetas

```
Renderer/
├── Material/
│   ├── Shader.h          # programa + pipeline + layout, por familia
│   ├── Material.h        # instancia: shader + params + texturas
│   ├── MaterialManager.h # resuelve MaterialHandle, carga de archivo
│   └── Sampler.h         # Nearest / Linear
└── Shaders/
    ├── pbr.vert / .frag
    ├── hd2d_sprite.vert / .frag    # billboard + nearest + lit
    └── unlit_sprite.vert / .frag
```

---

## Decisiones aún abiertas (para revisitar)

- **Parámetros: struct fijo por familia vs blob genérico** keyed por nombre. El
  struct fijo es simple; el blob es flexible (necesario si vas a un editor de
  materiales). Recomendado: struct fijo por familia al inicio.
- **Variantes de shader** (con/sin normal map, etc.): empezar sin variantes;
  añadir un sistema de permutaciones solo si hace falta.
- **Shader graph nodal:** muy a futuro, probablemente innecesario para este motor.
