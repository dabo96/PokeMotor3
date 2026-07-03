# Motor Pokémon — Cadena de post-proceso

> Pieza 5 (última) de la mitad gráfica. Convierte la imagen HDR de la escena en
> la imagen final de pantalla, y aloja el look HD-2D (tilt-shift + bloom).
> Backend: Vulkan.

**Estado:** borrador de diseño · cierra el diseño del renderer

---

## La idea

El post-proceso es una **cadena de passes fullscreen que leen una textura y
escriben otra** — exactamente para lo que se diseñó el render graph. Parte de
`sceneColor` (HDR) y termina en el swapchain.

---

## El patrón común: fullscreen pass + shader

Casi todo efecto de post = dibujar un triángulo fullscreen + un fragment shader
que muestrea la entrada. Código uniforme, shaders distintos.

```cpp
// Renderer/Passes/FullscreenPass.h
GraphTexture addFullscreenPass(RenderGraph& graph, const char* name,
                               ShaderHandle shader,
                               GraphTexture input, TextureDesc outDesc);
// declara read(input), write(output), dibuja triángulo fullscreen con 'shader'
```

---

## El orden y su razón

```
sceneColor(HDR) ──► [ Bloom ] ──► [ DoF / tilt-shift ] ──► [ Tonemap ] ──► swapchain
                                        ▲
                                  lee depth (del MainPass)
```

- **Bloom y DoF en HDR lineal:** necesitan los valores reales de brillo (bloom
  umbraliza > 1.0; el desenfoque conserva el brillo de lo difuminado).
- **Tonemap al final:** comprime HDR → pantalla. Lo que venga después (UI, texto)
  opera ya en espacio de pantalla.

---

## Los tres efectos

### Bloom

No es un pass: es una pirámide. Extrae las zonas brillantes, reduce a la mitad
varias veces (difumina), y vuelve a subir sumando. El grafo gestiona las texturas
intermedias.

```
bright ─► ½ ─► ¼ ─► ⅛        (downsample: extrae y difumina)
   ▲       ╲    ╲    │
   └── combina ◄── ⬆ ◄── ⬆    (upsample: suma de vuelta, glow suave)
```

### Profundidad de campo / tilt-shift  ← corazón del look HD-2D

Lee color + **depth** (otra dependencia que el render graph predijo). Calcula
cuánto desenfocar cada píxel según su distancia al plano de foco. Para el efecto
diorama: banda de foco estrecha, todo lo demás muy desenfocado → el mundo parece
una miniatura. Es el ingrediente que separa "sprites con luz" de "HD-2D".

### Tonemapping

Convierte el HDR crudo en imagen final: exposición + curva (ACES filmic, estándar,
gratis) + grading opcional, y escribe al swapchain. Último eslabón.

---

## Opcionales (aparcados)

- **SSAO** (oclusión ambiental): va antes, cerca de la iluminación, no en esta
  cadena final.
- **Antialiasing:** ya resuelto vía MSAA por elegir forward; o un FXAA como pass
  post. No necesario para arrancar.

---

## La cadena en código

```cpp
// En el Renderer — la cadena de post completa
void buildPostChain(RenderGraph& graph,
                    GraphTexture sceneColor, GraphTexture depth,
                    GraphTexture swapchain)
{
    GraphTexture bloomed = m_bloom.setup(graph, sceneColor);     // pirámide
    GraphTexture focused = m_dof.setup(graph, bloomed, depth);   // tilt-shift (lee depth)
    m_tonemap.setup(graph, focused, swapchain);                  // HDR→LDR→pantalla
}

// El bloom por dentro: sub-cadena de fullscreen passes
GraphTexture BloomPass::setup(RenderGraph& graph, GraphTexture scene) {
    GraphTexture bright = addFullscreenPass(graph, "bloom_threshold", m_threshold, scene, halfRes);
    GraphTexture cur = bright;
    for (int i = 0; i < MIPS; ++i)                               // downsample
        cur = addFullscreenPass(graph, "bloom_down", m_down, cur, halfOf(cur));
    for (int i = MIPS-1; i >= 0; --i)                            // upsample + suma
        cur = addFullscreenPass(graph, "bloom_up", m_up, cur, doubleOf(cur));
    return addFullscreenPass(graph, "bloom_combine", m_combine, scene, fullRes);
}
```

---

## El frame completo (piezas 2–5 conectadas)

```
[ Shadow ]─shadowMap─┐
                     ▼
[ Main (forward) ]──► sceneColor(HDR) ──► [ Bloom ] ──► [ DoF/tilt-shift ] ──► [ Tonemap ] ──► swapchain
       │                                                      ▲
       └──────────────────► depth ─────────────────────────────┘
```

Una sola cadena sirve a las dos modalidades 3D: **HD-2D** con el DoF fuerte (el
diorama); **3D completo** con el DoF suave o apagado. Misma maquinaria, dos looks.

---

## Estructura de carpetas

```
Renderer/
└── Passes/
    ├── FullscreenPass.h    # patrón común de post
    ├── BloomPass.h         # pirámide down/up
    ├── DepthOfFieldPass.h  # tilt-shift, lee depth
    └── TonemapPass.h       # ACES, HDR→LDR→swapchain
└── Shaders/
    ├── bloom_threshold/down/up/combine.frag
    ├── dof.frag
    └── tonemap.frag
```

---

## Decisiones aún abiertas (para revisitar)

- **Nº de niveles del bloom** y umbral: tuning visual.
- **DoF: depth-based puro vs gradiente de pantalla** (tilt-shift literal). El
  gradiente da el look de maqueta más marcado; el depth-based es más físico.
  Probablemente una mezcla configurable.
- **Tonemapper concreto:** ACES para empezar; LUT de grading después para dar
  identidad de color a cada zona (pueblo cálido, cueva fría).
- **Color grading por escena:** vía LUT, mejora futura.
