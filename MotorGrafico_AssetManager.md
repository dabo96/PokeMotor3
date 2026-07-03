# Motor Pokémon — Asset / Resource Manager

> Pieza fundacional: el único dueño de los recursos pesados (mallas, texturas,
> materiales). Resuelve los handles que todo el motor usa. Backend: C++ + VMA.

**Estado:** borrador de diseño · **Posición:** sobre el VulkanContext; lo usan renderer y juego

---

## La idea central: propiedad por handle

El manager es el **único dueño** de los datos pesados (los `VkImage`/`VkBuffer`);
todos los demás llevan handles ligeros (`MeshHandle`, `TextureHandle`,
`MaterialHandle`). Es el gemelo, del lado del renderer, de la `Database` del juego:
mismo patrón "dueño + handle" que el flyweight de Pokémon.

```
   Scene / DrawItem ──MeshHandle──►┌──────────────┐
                                   │ AssetManager │ posee el VkBuffer real
   Material ────────TextureHandle─►│ (único dueño)│ posee el VkImage real
                                   └──────────────┘
```

**El pago de la indirección:** como nadie referencia el recurso directamente sino
a través del handle, puedes **recargar un asset en caliente** (hot-reload)
intercambiando el recurso por debajo sin que ningún poseedor se entere — el handle
sigue válido. Lo mismo que hace trivial el save en la lógica de juego.

---

## Almacenamiento: pool con generación

Un pool genérico de slots. El `generation` del `Handle` (del Core) detecta un slot
liberado y reutilizado → caza use-after-free en vez de leer basura.

```cpp
// Assets/ResourcePool.h
template <typename T>
class ResourcePool {
public:
    Handle<T> add(T&& resource) {
        uint32_t idx;
        if (!m_freeList.empty()) { idx = m_freeList.back(); m_freeList.pop_back(); }
        else { idx = (uint32_t)m_slots.size(); m_slots.push_back({}); }
        m_slots[idx].resource = std::move(resource);
        m_slots[idx].alive = true;
        return { idx, ++m_slots[idx].generation };
    }
    T* get(Handle<T> h) {
        if (h.index >= m_slots.size()) return nullptr;
        Slot& s = m_slots[h.index];
        if (!s.alive || s.generation != h.generation) return nullptr; // colgado
        return &s.resource;
    }
    void remove(Handle<T> h) {
        if (T* r = get(h)) { /* liberar GPU */ m_slots[h.index].alive = false;
                             m_freeList.push_back(h.index); }
    }
private:
    struct Slot { T resource; uint32_t generation = 0; bool alive = false; };
    std::vector<Slot>     m_slots;
    std::vector<uint32_t> m_freeList;
};
```

---

## El manager: un pool por tipo + caché por ruta

```cpp
// Assets/AssetManager.h
class AssetManager {
public:
    void init(VulkanContext& ctx);

    MeshHandle     loadMesh(const std::string& path);     // importa glTF
    TextureHandle  loadTexture(const std::string& path);  // importa PNG/...
    MaterialHandle createMaterial(const Material& m);

    const Mesh&     get(MeshHandle h);
    const Texture&  get(TextureHandle h);
    const Material& get(MaterialHandle h);

private:
    VulkanContext*         m_ctx;
    ResourcePool<Mesh>     m_meshes;
    ResourcePool<Texture>  m_textures;
    ResourcePool<Material> m_materials;
    std::unordered_map<std::string, uint32_t> m_pathCache;  // dedup por ruta
};
```

Flujo de `loadTexture("rock.png")`:

```
1. ¿está en m_pathCache?  → sí: devuelve el handle existente (no re-sube)
2. no: importar  → decodificar PNG → subir a GPU (VMA) → pool.add()
3. cachear ruta → handle
4. devolver handle
```

El caché evita subir dos veces la misma textura. Los **importadores** (glTF, PNG)
son módulos aparte del manager: leen el archivo y producen el recurso en CPU; el
manager lo sube a GPU y lo guarda.

---

## Vidas de los recursos

- **Inicio: propiedad por escena/mapa.** Cargar los assets de un mapa al entrar,
  descargarlos al salir. Simple y suficiente para un Pokémon.
- **Refcount: si hace falta**, cuando un asset lo comparten varios mapas y quieres
  no recargarlo. Añadir solo si el patrón aparece.
- **Streaming asíncrono: futuro.** Encaja con el sistema de jobs (aún aplazado):
  cargar en otro hilo, devolver un handle "pendiente" que se resuelve al terminar.

---

## Estructura de carpetas

```
Assets/
├── AssetManager.h
├── ResourcePool.h        # pool genérico con generación
├── Mesh.h
├── Texture.h
└── Importers/
    ├── GltfImporter.h    # malla → CPU
    └── ImageImporter.h   # PNG/etc → CPU
```

---

## Decisiones aún abiertas (para revisitar)

- **Refcount vs propiedad explícita:** empezar explícito (por mapa), refcount solo
  si hay assets compartidos entre mapas.
- **Carga asíncrona:** depende del sistema de jobs (aplazado). El handle ya permite
  un estado "pendiente".
- **Hot-reload:** vigilar archivos y recargar; pareja natural del editor. La
  indirección por handle ya lo hace posible.
- **Empaquetado para release** (pak/archivo) vs archivos sueltos: sueltos en
  desarrollo, empaquetar al final (pieza del VFS, aún no módulo).
```