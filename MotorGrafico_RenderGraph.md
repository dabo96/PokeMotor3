# Motor Pokémon — Render Graph (arquitectura del renderer)

> Primera pieza de la mitad gráfica del motor. El render graph es el esqueleto
> del que cuelgan el resto de sistemas de render (materiales, luces, post-proceso).
> Backend: Vulkan.

**Estado:** borrador de diseño · **Cubre:** clásico 2D, HD-2D y 3D completo

---

## Qué es y por qué es el esqueleto

Un render graph describe el frame como un **grafo dirigido acíclico (DAG) de
passes** y los **recursos (texturas/buffers)** que cada pass lee y escribe. En
vez de una función monolítica con el orden de los passes cableado a mano, se
declaran passes y dependencias, y el grafo:

- deduce el orden de ejecución a partir de las dependencias,
- descarta passes cuya salida nadie consume (culling),
- asigna y reutiliza (aliasing) la memoria de las texturas intermedias,
- **inserta automáticamente las barreras y transiciones de layout de Vulkan.**

### Por qué encaja con este proyecto

1. **Tres modos visuales = tres grafos**, sin `if/else` en una función gigante.
2. **Automatiza la sincronización de Vulkan** (barreras, layouts), que es la
   parte más dolorosa y con más bugs para quien empieza.
3. **El post-proceso es por naturaleza una cadena de passes** que leen/escriben
   texturas — el render graph es justo esa estructura.
4. **Centraliza la complejidad** en el compilador del grafo, no esparcida por
   cada pass. Fiel a la filosofía de motor simple.

---

## Las tres fases por frame

```
1. DECLARAR   el código declara passes y qué recursos lee/escribe cada uno.
              Se construye el DAG. Cero trabajo de GPU.
                        │
                        ▼
2. COMPILAR   orden topológico por dependencias · culling de passes inútiles ·
              asignación y aliasing de texturas transitorias · cálculo de barreras.
                        │
                        ▼
3. EJECUTAR   recorre los passes en orden · hace las transiciones de layout ·
              llama al callback execute de cada pass para grabar los draw calls.
```

La fase 2 hace toda la sincronización que no se quiere escribir a mano en Vulkan.

---

## Dónde encaja (refinamiento del diseño previo)

En el boceto inicial, `Pipeline2D`/`Pipeline3D` eran los orquestadores del frame.
Con render graph, el orquestador pasa a ser el `RenderGraph`, y los pipelines
bajan de rango: son el **estado de GPU que un pass enlaza por dentro** para
dibujar. Los `VkPipeline` siguen existiendo, como herramienta dentro de un pass.

```
Renderer              orquesta: declarar → compilar → ejecutar
   │ posee
   ├─ RenderGraph      passes + recursos como DAG
   │     └─ RenderPass · callback execute ──usa──► VkPipeline (2D / 3D / post)
   │ posee
   └─ VulkanContext    device, allocator, swapchain ◄── graba en VkCommandBuffer
```

---

## Clases y API

### Recursos: handles virtuales

Un recurso del grafo es un handle virtual — no es un `VkImage` todavía. El
compilador lo resuelve a una textura real, posiblemente reutilizando memoria de
otra. Es el mismo patrón `Handle` del módulo Core.

```cpp
// Renderer/Graph/Resources.h
struct TextureDesc { uint32_t width, height; VkFormat format; };

struct GraphTexture { uint32_t id = 0; };   // el grafo decide el VkImage real
struct GraphBuffer  { uint32_t id = 0; };
```

### El grafo

```cpp
// Renderer/Graph/RenderGraph.h
class RenderGraph {
public:
    // Fase 1 — el setup declara recursos y RETORNA el callback execute.
    template <typename SetupFn>
    void addPass(const char* name, SetupFn&& setup);

    GraphTexture createTexture(const TextureDesc& desc);     // transitorio
    GraphTexture importTexture(VkImage img, TextureDesc d);  // swapchain, persistentes

    void compile();                      // fase 2
    void execute(VkCommandBuffer cmd);   // fase 3

private:
    struct Pass {
        std::string name;
        std::vector<uint32_t> reads, writes;
        std::function<void(RenderPassContext&)> execute;
    };
    std::vector<Pass> m_passes;
    // tabla de recursos virtuales · plan de barreras
};
```

### Builder (declaración) y contexto (ejecución)

```cpp
// Fase de declaración: declarar dependencias
class RenderGraphBuilder {
public:
    GraphTexture read(GraphTexture t);    // este pass lee t
    GraphTexture write(GraphTexture t);   // este pass escribe t
};

// Fase de ejecución: lo que recibe el callback de cada pass
struct RenderPassContext {
    VkCommandBuffer cmd;
    VkImageView resolve(GraphTexture t);  // handle virtual → vista real, layout listo
};
```

### Ejemplo: declarar dos passes con dependencia

```cpp
GraphTexture shadowMap  = graph.createTexture({2048, 2048, VK_FORMAT_D32_SFLOAT});
GraphTexture sceneColor = graph.createTexture({w, h, VK_FORMAT_R16G16B16A16_SFLOAT});

graph.addPass("shadow", [&](RenderGraphBuilder& b) {
    b.write(shadowMap);                   // produce shadowMap
    return [=](RenderPassContext& ctx) {
        /* dibujar la escena desde la luz */
    };
});

graph.addPass("main", [&](RenderGraphBuilder& b) {
    b.read(shadowMap);                    // depende de "shadow"
    b.write(sceneColor);                  // → el grafo lo ordena después, solo
    return [=](RenderPassContext& ctx) {
        /* dibujar la escena iluminada usando shadowMap */
    };
});
```

Nunca se dijo "shadow antes que main". El grafo lo dedujo de que `main` lee lo
que `shadow` escribe. Esa inferencia es el corazón del patrón.

---

## Qué hace `compile()`

1. Construir el DAG: un pass que lee R depende del último que escribió R.
2. Orden topológico → secuencia de ejecución.
3. Culling: descartar passes cuya salida nadie consume (salvo el swapchain final).
4. Asignar texturas transitorias; aliasar memoria entre recursos cuyas vidas no
   se solapan.
5. Por cada recurso, entre uso y uso, calcular la barrera y la transición de
   layout necesaria.

Los pasos 4 y 5 son justo lo que no se quiere llevar a mano en Vulkan; aquí
viven en un único sitio.

---

## Los tres modos visuales como grafos

### Clásico Game Boy — grafo trivial de un pass

```
[ tiles + sprites (ortográfico) ] ──► swapchain
```

### HD-2D y 3D completo — el mismo grafo, con toggles

```
[ shadow ] ─► [ escena principal ] ─► [ bloom ] ─► [ DoF / tilt-shift ] ─► [ tonemap ] ─► swapchain
                     ▲                                   ▲
           billboards (HD-2D)                  fuerte en HD-2D (efecto diorama)
           o modelos (3D)                      suave o off en 3D completo
```

La estructura del grafo es idéntica para HD-2D y 3D. Lo que cambia:

- **Contenido del pass principal:** billboards con textura pixel-art (HD-2D) vs.
  modelos con materiales PBR (3D).
- **Parámetros del pass de DoF/tilt-shift:** fuerte para el look de maqueta de
  HD-2D, suave o desactivado en 3D.

Es decir: **un solo pipeline 3D sirve a los dos looks.**

---

## Por qué sirve a "nuevo en Vulkan" y "motor simple"

- La sincronización (barreras, layouts) —lo más bug-prone de Vulkan— se calcula
  una vez en el compilador, no se repite en cada pass.
- Cada pass queda simple: declara lo que lee/escribe y graba sus draws. No sabe
  de barreras ni de orden global.
- Añadir un efecto nuevo = añadir un pass. Quitarlo = quitar un pass. La
  estructura del frame es legible de un vistazo.

---

## Estructura de carpetas (render graph)

```
Renderer/
├── Graph/
│   ├── RenderGraph.h        # declarar / compilar / ejecutar
│   ├── RenderGraphBuilder.h # API de declaración de dependencias
│   ├── RenderPassContext.h  # contexto de ejecución
│   ├── Resources.h          # GraphTexture, GraphBuffer, TextureDesc
│   └── Compiler.h           # orden topológico, aliasing, barreras
├── Passes/
│   ├── ShadowPass.h
│   ├── MainPass.h
│   ├── BloomPass.h
│   ├── DepthOfFieldPass.h   # el tilt-shift de HD-2D
│   └── TonemapPass.h
└── (VulkanContext, Renderer, VkPipeline...)
```

---

## Decisiones aún abiertas (para revisitar)

- **¿Grafo reconstruido cada frame o cacheado?** Reconstruir cada frame es más
  simple y flexible; cachear es más rápido. Recomendado: reconstruir al inicio,
  optimizar solo si el coste de CPU aparece en el perfilado.
- **Granularidad de los passes:** ¿un pass por efecto de post, o un "uber-pass"?
  Recomendado: un pass por efecto al inicio (legible), fusionar después si hace
  falta.
- **Escena principal: forward, forward+ o deferred.** Afecta a cómo se estructura
  el pass principal y el de iluminación. A decidir al diseñar luces/materiales.
