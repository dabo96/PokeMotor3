# Brief de implementación — Mejoras de calidad del renderer 2D (sprites HD) · PokeMotor

> **Para Claude Code.** Hacer que los sprites HD (arte detallado, anti-aliased) se
> rendericen nítidos: filtrado lineal + mipmaps + camino full-res + alpha
> premultiplicado. Conservar nearest+lowRes como un **modo pixel-art por asset**.

---

## Contexto y problema

- **PokeMotor**: C++ / Vulkan, 2D. Existe `SpriteBatch`, `Sprite2DPass`,
  `AssetManager` (loadTexture), el `SpriteRenderSystem` (render-feed), y
  posiblemente un **target interno de baja resolución + upscale nearest** (look
  retro).
- **Problema:** los sprites de Pokémon son arte detallado con sombreado suave y
  bordes anti-aliased — **no son pixel art**. Cargados con **nearest** (filtro de
  pixel art) pierden calidad. Y si pasan por un target de baja resolución, se
  reducen antes de llegar a pantalla y ningún filtro lo recupera.

> El render-feed decide **QUÉ** se dibuja; este brief decide **CÓMO se ve**.

> **Claude Code:** adapta a los nombres reales (Texture, AssetManager, el pipeline
> de sprites, el batch).

---

## Objetivo

Los sprites HD se ven limpios: suaves al escalar, sin shimmer al achicar, sin halos
en los bordes. Los tiles pixel-art siguen nítidos con nearest. **El filtrado es por
asset, no global.**

---

## Piezas a construir

### 1. Dos samplers (no uno global)

```cpp
enum class FilterMode { Pixel, Smooth };

VkSampler makeSampler(VkDevice device, FilterMode m) {
    VkSamplerCreateInfo s{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    if (m == FilterMode::Pixel) {                 // pixel art / tiles
        s.magFilter = s.minFilter = VK_FILTER_NEAREST;
        s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        s.maxLod = 0.0f;                          // sin mips
    } else {                                      // sprites HD
        s.magFilter = s.minFilter = VK_FILTER_LINEAR;
        s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;  // trilinear
        s.maxLod = VK_LOD_CLAMP_NONE;             // toda la cadena de mips
        // s.anisotropyEnable = VK_TRUE; s.maxAnisotropy = 8.0f; // si la feature está
    }
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler out; vkCreateSampler(device, &s, nullptr, &out); return out;
}
// Crear UNA vez: g_pixelSampler y g_smoothSampler. Reusar.
```

### 2. Filtrado por asset

```cpp
struct Texture {
    VkImage     image;
    VkImageView view;
    uint32_t    mipLevels = 1;
    FilterMode  filter = FilterMode::Smooth;   // por defecto suave (HD)
};

// AssetManager: loadTexture(path, FilterMode = Smooth).
//   tiles → Pixel ; sprites de Pokémon → Smooth.
```

El pipeline de sprites **bindea el sampler que corresponde** al `filter` de la
textura. Si el batch ya agrupa por textura, el sampler queda determinado por grupo;
si un batch mezcla modos, separar en dos draws por sampler.

### 3. Generación de mipmaps (para texturas Smooth)

Lo más importante cuando el sprite se muestra **más pequeño que su tamaño nativo**:
sin mipmaps hay shimmer/aliasing aunque uses lineal.

```cpp
// Imagen creada con: mipLevels = floor(log2(max(w,h))) + 1
//                    usage = TRANSFER_SRC | TRANSFER_DST | SAMPLED
// 1. Subir el mip 0.
// 2. Para i = 1 .. mipLevels-1:
//      - barrera: nivel i-1 → TRANSFER_SRC_OPTIMAL
//      - vkCmdBlitImage(i-1 → i) con VK_FILTER_LINEAR (mitad de tamaño cada vez)
// 3. Barrera final: TODOS los niveles → SHADER_READ_ONLY_OPTIMAL
```

> **GOTCHA:** comprobar que el formato soporta blit lineal
> (`vkGetPhysicalDeviceFormatProperties` →
> `VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT`). Si no, generar mips en CPU
> o elegir otro formato.

### 4. Camino full-res para HD (NO el target de baja resolución)

Si existe un target interno lowRes (look retro): los sprites HD que pasen por ahí
se **reducen antes de la pantalla** y se pierde el detalle. Opciones:

- **Recomendado:** renderizar el contenido HD a **resolución completa** (no al
  target lowRes).
- Si quieres conservar el look retro a la vez: dos targets (capas pixel-art →
  lowRes → upscale; capas HD → full-res) y componer. Más complejo; solo si de
  verdad quieres ambos looks simultáneos.

> Verificar esto primero: suele ser la mayor fuente de pérdida.

### 5. Alpha premultiplicado (bordes limpios)

Los sprites con bordes suaves + filtrado lineal + alpha "directo" sacan **halos
oscuros**. Solución:

```cpp
// Al cargar (CPU), por pixel 8-bit: r=r*a/255, g=g*a/255, b=b*a/255.

// Estado de blend PREMULTIPLICADO en el pipeline de sprites:
blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
// Premultiplicar Y blend premultiplicado van JUNTOS (no mezclar con straight alpha).
```

### 6. (v2, opcional) Correcto en sRGB

Cargar texturas de color como `..._SRGB` (se linealizan al muestrear), blend en
lineal, salida a sRGB. Da colores correctos. Sutileza: el premultiplicado ideal es
en espacio lineal. Dejar para después; primero lo de arriba.

---

## Milestones

1. Dos samplers creados; el pass de sprites bindea el **smooth** → los HD se ven
   más suaves de inmediato.
2. Mipmaps para texturas Smooth → sin shimmer al mostrarlos pequeños.
3. `FilterMode` por asset (tiles=Pixel, sprites=Smooth); bindear el sampler que toca.
4. Alpha premultiplicado (carga + blend) → bordes limpios, sin halo.
5. Verificar que los HD se renderizan **full-res** (no por el target lowRes).
6. (v2) sRGB correcto.

---

## Criterios de aceptación

- [ ] El Bulbasaur HD se renderiza **suave** (sin dientes de sierra) a tamaño nativo.
- [ ] Mostrado más pequeño que su nativo → **sin shimmer/aliasing** (mipmaps).
- [ ] Los bordes suaves **no tienen halo oscuro** (premultiplicado).
- [ ] Los tiles pixel-art siguen **nítidos** (sampler Pixel, nearest).
- [ ] (Si hay target lowRes) los sprites HD **no pasan** por él / van full-res.

---

## Fuera de alcance — NO construir

- **Animación de sprites** (hojas, clips): aparte.
- **El look retro lowRes en sí:** se conserva como modo por asset, no se elimina.
- **3D / HD-2D.**

---

## Notas de diseño

- **Filtrado y target son por asset, no globales:** dos samplers, la textura elige.
- **nearest = pixel art; lineal+mips = arte HD.** Usar el correcto para cada uno.
- **Mipmaps** son el arreglo clave al **achicar**; lineal solo, no basta.
- **El target lowRes destruye el detalle HD** antes de cualquier filtro: revisarlo
  primero.
- **Premultiplicado** limpia los bordes suaves; carga y blend deben ir juntos.
