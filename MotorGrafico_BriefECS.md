# Brief de implementación — ECS robusto (base de todo) · PokeMotor

> **Para Claude Code.** Implementar un ECS sólido y completo (sparse sets + handles
> con generación) que sea la base de los briefs de **Assets** y **Scripting**.
> **Implementar ESTE primero.** Los otros dos usan su API tal cual.

---

## Contexto y orden

- **PokeMotor**: C++ / Vulkan, RPG estilo Pokémon, 2D.
- Existe un ECS **a medio implementar**. Este brief lo reemplaza por uno robusto.
- **Dependencia:** los briefs de Assets y Scripting usan `createEntity`, `add<T>`,
  `get<T>`, `has<T>`, `view<...>`, `allEntities`, `destroyEntity` y el tipo
  `Entity`. Este ECS debe exponerlos exactamente. **Construir este antes.**

> **Claude Code:** integra con el código real (la clase Scene/Registry actual, los
> componentes ya existentes como Transform/Sprite/Name). Migra lo que haya al
> nuevo almacenamiento.

---

## Qué significa "robusto" aquí

- **NO** es un ECS de arquetipos (overkill para esta escala).
- **SÍ** es el modelo pragmático (entidades + componentes + sistemas) implementado
  bien: **sparse sets** (denso, cache-friendly, O(1)) + **handles con generación**
  (detectan entidades muertas).

Alternativa de librería (si no se quiere mantener propio): **EnTT**. Este brief es
para el ECS propio.

---

## Objetivo

Un almacén de componentes correcto y completo, con esta API exacta:

```cpp
Entity e = scene.createEntity();
scene.add<Transform>(e, { ... });
scene.add<SpriteComponent>(e, { tex });
bool b = scene.has<Transform>(e);
Transform& t = scene.get<Transform>(e);
scene.remove<SpriteComponent>(e);
scene.destroyEntity(e);                       // limpia TODOS sus componentes
for (Entity x : scene.view<Transform, SpriteComponent>()) { ... }
auto all = scene.allEntities();               // para la jerarquía
```

Cualquier `struct` plano puede ser un componente (Transform, Sprite, Name, Script,
GridMover...). El ECS no los conoce de forma especial.

---

## Arquitectura — piezas a construir

### 1. `Entity` — handle con generación

```cpp
struct Entity {
    uint32_t id = 0;
    uint32_t generation = 0;
    bool valid() const { return generation != 0; }
    bool operator==(Entity o) const { return id == o.id && generation == o.generation; }
    bool operator!=(Entity o) const { return !(*this == o); }
};
// (Opcional: empaquetar id+generation en un uint64 para compacidad.)
```

### 2. `EntityManager` — crea/destruye con free list + generaciones

```cpp
class EntityManager {
public:
    Entity create() {
        uint32_t id;
        if (!m_free.empty()) {
            id = m_free.back(); m_free.pop_back();
            return { id, m_generations[id] };       // generación ya subida en destroy
        }
        id = (uint32_t)m_generations.size();
        m_generations.push_back(1);                 // primera generación válida (≠0)
        return { id, 1 };
    }
    void destroy(Entity e) {
        if (!alive(e)) return;
        m_generations[e.id]++;                      // invalida handles viejos
        m_free.push_back(e.id);
    }
    bool alive(Entity e) const {
        return e.id < m_generations.size() && m_generations[e.id] == e.generation;
    }
    template<typename Fn> void forEachAlive(Fn fn) const; // ids con generación viva
private:
    std::vector<uint32_t> m_generations;
    std::vector<uint32_t> m_free;
};
```

### 3. `IComponentPool` — base type-erased (para limpiar en destroy)

```cpp
struct IComponentPool {
    virtual ~IComponentPool() = default;
    virtual void remove(Entity e) = 0;
    virtual bool has(Entity e) const = 0;
};
```

### 4. `ComponentPool<T>` — sparse set (el corazón)

```cpp
template <typename T>
class ComponentPool : public IComponentPool {
    static constexpr uint32_t NIL = UINT32_MAX;
public:
    T& add(Entity e, T comp) {
        if (e.id >= m_sparse.size()) m_sparse.resize(e.id + 1, NIL);
        ASSERT(m_sparse[e.id] == NIL, "componente duplicado");
        m_sparse[e.id] = (uint32_t)m_dense.size();
        m_dense.push_back(e);
        m_components.push_back(std::move(comp));
        return m_components.back();
    }
    bool has(Entity e) const override {
        return e.id < m_sparse.size() && m_sparse[e.id] != NIL
            && m_dense[m_sparse[e.id]] == e;        // compara owner → valida generación
    }
    T& get(Entity e) {
        ASSERT(has(e), "la entidad no tiene este componente");
        return m_components[m_sparse[e.id]];
    }
    void remove(Entity e) override {
        if (!has(e)) return;
        uint32_t idx  = m_sparse[e.id];
        uint32_t last = (uint32_t)m_dense.size() - 1;
        m_dense[idx]      = m_dense[last];          // swap-and-pop: mantiene denso
        m_components[idx] = std::move(m_components[last]);
        m_sparse[m_dense[idx].id] = idx;            // reapunta el que se movió
        m_dense.pop_back(); m_components.pop_back();
        m_sparse[e.id] = NIL;
    }
    const std::vector<Entity>& entities()   const { return m_dense; }       // iteración densa
    std::vector<T>&            components()        { return m_components; }
    size_t size() const { return m_dense.size(); }
private:
    std::vector<uint32_t> m_sparse;       // entity.id → índice en denso, o NIL
    std::vector<Entity>   m_dense;         // qué entidad posee components[i]
    std::vector<T>        m_components;     // paralelo a m_dense
};
```

Claves de robustez aquí:
- `has` compara `m_dense[...] == e` → un handle viejo (id reusado, otra generación)
  da `false`. **Detecta use-after-free.**
- `remove` hace swap-and-pop → el array denso nunca tiene huecos (iteración rápida).

### 5. `Scene` / `Registry` — posee entidades + pools, da la API

```cpp
class Scene {
public:
    Entity createEntity() { return m_entities.create(); }
    void destroyEntity(Entity e) {
        if (!m_entities.alive(e)) return;
        for (auto& [type, pool] : m_pools) pool->remove(e);  // limpia TODOS los pools
        m_entities.destroy(e);
    }
    bool alive(Entity e) const { return m_entities.alive(e); }

    template<typename T> T&   add(Entity e, T c) { return pool<T>().add(e, std::move(c)); }
    template<typename T> void remove(Entity e)    { if (auto* p = poolPtr<T>()) p->remove(e); }
    template<typename T> bool has(Entity e) const { auto* p = poolPtr<T>(); return p && p->has(e); }
    template<typename T> T&   get(Entity e)       { return pool<T>().get(e); }

    template<typename... Ts> View<Ts...> view()   { return View<Ts...>(*this); }
    std::vector<Entity> allEntities() const;       // para la jerarquía

    template<typename T> ComponentPool<T>& pool(); // crea el pool si no existe
    template<typename T> ComponentPool<T>* poolPtr() const; // nullptr si no existe
private:
    EntityManager m_entities;
    std::unordered_map<std::type_index, std::unique_ptr<IComponentPool>> m_pools;
};
```

### 6. `View<Ts...>` — la query

Contrato: itera **el pool más pequeño** entre `Ts`, y por cada entidad filtra que
tenga el resto. Debe soportar:
- `.each([](Entity e, Ts&...){ })` (referencia), y
- `for (Entity e : view)` rindiendo cada `Entity` que tiene todos los `Ts` (para el
  estilo que usan los otros briefs).

```cpp
template<typename... Ts>
class View {
public:
    View(Scene& s) : m_scene(s) {}
    template<typename Fn> void each(Fn fn) {
        const auto& base = smallestPoolEntities();   // pool más pequeño entre Ts
        for (Entity e : base)
            if ((m_scene.has<Ts>(e) && ...))
                fn(e, m_scene.get<Ts>(e)...);
    }
    // begin()/end(): iterador que salta entidades que no tienen todos los Ts,
    //                rindiendo Entity. (boilerplate estándar)
private:
    Scene& m_scene;
};
```

---

## Requisitos de robustez (lo que lo separa de la versión a medias)

1. **Handles con generación:** destruir una entidad incrementa su generación; los
   handles viejos quedan inválidos (`alive`/`has` dan false). Sin use-after-free.
2. **`destroyEntity` limpia todos los pools:** recorre los pools y hace `remove`.
   Sin componentes huérfanos.
3. **Iteración densa:** los componentes viven en arrays densos (sparse set), no en
   un `unordered_map` por entidad. Cache-friendly.
4. **Cambios estructurales durante la iteración:** añadir/quitar/destruir DENTRO de
   un `view` puede invalidarlo. Regla simple: no hacer cambios estructurales en
   medio de un recorrido; si hace falta, **recoger y aplicar después** (o una cola
   de destrucción diferida al final del frame). Documentar.
5. **Sin asignaciones en el hot path:** los pools crecen y se reutilizan; no se
   asigna por frame en la iteración.
6. **Type-safety:** los componentes son tipos; `add<T>`/`get<T>` son chequeados.

---

## Integración / migración

- Reemplazar el almacenamiento del ECS actual por `ComponentPool<T>` + `Scene`.
- Los componentes existentes (Transform, Sprite, Name...) se vuelven structs planos
  guardados en pools. No necesitan herencia ni nada especial.
- El editor:
  - **Jerarquía** → `scene.allEntities()` + `NameComponent`.
  - **Inspector** → recorre los componentes de la entidad seleccionada.
  - Los **sistemas** (movimiento, render-feed) → `scene.view<...>()`.
- Tras migrar, los briefs de **Assets** y **Scripting** se montan sin cambios de API.

---

## Milestones

1. `Entity` + `EntityManager`: crear/destruir, `alive`, free list + generaciones.
2. `ComponentPool<T>` (sparse set): add/has/get/remove con tests básicos.
3. `Scene`: add/get/has/remove/destroyEntity (con limpieza de pools) + allEntities.
4. `View<Ts...>`: `.each` y range-for; iterar el pool más pequeño + filtro.
5. Migrar los componentes y sistemas actuales al nuevo ECS; el editor sigue
   funcionando (jerarquía/inspector/render).
6. Test de estrés mínimo: crear/destruir muchas entidades, verificar que los
   handles viejos quedan inválidos y no hay fugas.

---

## Criterios de aceptación

- [ ] Crear una entidad, añadir Transform + Sprite, `get` los devuelve; `has` acierta.
- [ ] Destruir la entidad → `alive` da false; un handle viejo a ese id (nueva
      generación) da `has`/`get` inválido (no lee basura).
- [ ] `destroyEntity` deja sus componentes fuera de todos los pools (sin huérfanos).
- [ ] `view<Transform, SpriteComponent>()` recorre solo entidades con ambos.
- [ ] El editor (jerarquía + inspector + render) funciona sobre el nuevo ECS.
- [ ] Crear/destruir 10.000 entidades en bucle no fuga memoria ni rompe índices.

---

## Fuera de alcance — NO construir

- **Arquetipos** / agrupación por combinación de componentes (overkill).
- **Relaciones** (padre-hijo) — eso es transforms jerárquicos, otro tema (fase 2 de
  la jerarquía).
- **Signals/observers** más allá de lo básico, **grupos**, **multi-threading**.
- Cualquier optimización avanzada antes de tener un perfilado que la pida.

---

## Notas de diseño

- **Sparse set, no `map`:** el salto de robustez/rendimiento más grande viene de
  aquí.
- **Generación en el handle:** barato, y mata toda una clase de bugs.
- **Destrucción diferida** (cola que se vacía al final del frame) si los cambios en
  medio de iteraciones dan problemas — es el patrón estándar.
- **EnTT** sigue siendo la salida si mantener esto cansa; la API de arriba es
  conceptualmente la misma, así que migrar sería acotado.
```