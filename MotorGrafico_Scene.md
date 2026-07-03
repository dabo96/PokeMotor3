# Motor Pokémon — Scene (el mundo)

> Pieza fundacional: la estructura que sostiene el mundo (entidades, transforms,
> render, luces, cámara). Puente entre la lógica de juego y el render.

**Estado:** borrador de diseño · **Posición:** entre lógica de juego y renderer

---

## La decisión: modelo de componentes pragmático, NO full ECS

Fiel a la regla de no sobre-diseñar la lógica: para los recuentos de un Pokémon
(decenas de NPCs, no miles de balas; los tiles los lleva el `TileMap` aparte), un
ECS completo es sobre-ingeniería.

La elección es un **modelo de componentes pragmático**: entidades como IDs, un
puñado de arrays de componentes en la Scene, y queries tipadas. Da el beneficio
data-oriented donde importa (iterar todos los renderables, todas las luces) sin el
framework completo de un ECS.

> Migrar a ECS puro queda como opción futura, solo si el perfilado lo justifica.

---

## El papel: el mundo compartido

La Scene es lo que la lógica manipula y lo que el render consume:

```
  Modos de juego (Overworld, Battle...) ──manipulan──► Scene
                                                         │ produce
                          ┌──────────────────────────────┼─────────────────┐
                          ▼                               ▼                 ▼
                   renderables() → DrawItems      lights() → LightBuffer  camera()
                          └───────────── lo consume el Renderer ──────────┘
```

El renderer **no depende** de la Scene: recibe listas (DrawItems, luces, matrices).
Ese desacople es el que permite probar el render sin lógica de juego.

**División clara:** la Scene tiene lo **dinámico** (jugador, NPCs, objetos, luces);
el `TileMap` (lógica de juego) tiene la grilla **estática** del overworld. No se
mezclan.

---

## Componentes (datos planos)

```cpp
// Scene/Components.h
using EntityId = Handle<struct EntityTag>;

struct Transform {
    Vec3 position{};
    Quat rotation{};
    Vec3 scale{1,1,1};
    Mat4 matrix() const;
};

struct RenderComponent {
    MeshHandle     mesh;
    MaterialHandle material;
    bool           transparent = false;   // a qué bucket del pass principal
};

struct LightComponent { Light light; };
```

---

## La Scene

```cpp
// Scene/Scene.h
class Scene {
public:
    EntityId createEntity();
    void     destroyEntity(EntityId e);

    Transform&       transform(EntityId e);
    RenderComponent& addRender(EntityId e, MeshHandle, MaterialHandle);
    LightComponent&  addLight(EntityId e, const Light&);

    // lo que el renderer consume:
    std::vector<DrawItem> renderables() const;  // entidades con RenderComponent
    std::vector<Light>    lights() const;        // entidades con LightComponent
    Camera&               camera();

private:
    // almacenamiento por componente (data-oriented, pragmático)
    std::vector<Transform>       m_transforms;
    std::vector<RenderComponent> m_renders;
    std::vector<LightComponent>  m_lights;
    // mapeo EntityId → índices en cada array
};
```

`renderables()` recorre las entidades con `RenderComponent`, combina su
`Transform` y produce los `DrawItem` que el `MainPass` (o el `Sprite2DPass`)
dibuja. `lights()` alimenta el `LightBuffer` del set 0.

---

## Conexión con todo lo demás

- **Game logic:** `OverworldMode` spawnea al jugador y NPCs como entidades, coloca
  luces (la antorcha del pueblo es una entidad con `LightComponent`).
- **Assets:** los componentes guardan `MeshHandle`/`MaterialHandle` → se resuelven
  contra el `AssetManager`. La Scene no posee recursos pesados, solo handles.
- **Renderer:** consume `renderables()`, `lights()`, `camera()`. No conoce la
  Scene por dentro.
- **Cámara:** la Scene expone la cámara activa; el modo de juego la configura (con
  el `FollowCameraController`).

---

## Estructura de carpetas

```
Scene/
├── Scene.h
├── Entity.h          # EntityId = Handle<EntityTag>
├── Components.h      # Transform, RenderComponent, LightComponent
└── (Camera vive en Renderer/Camera/)
```

---

## Decisiones aún abiertas (para revisitar)

- **Jerarquía de transforms** (padre-hijo, p.ej. un objeto pegado a la mano):
  empezar plano (todo en espacio mundo); añadir parenting si un caso lo pide.
- **Migración a ECS puro:** solo si el perfilado muestra que iterar componentes es
  un cuello de botella (improbable a esta escala).
- **Spatial partitioning** (quadtree/grid para culling): innecesario al inicio;
  el overworld por mapas ya acota lo visible.
- **Componentes nuevos** (script, animación, colisión de grid): se añaden como más
  arrays cuando los sistemas que los usan existan.
```