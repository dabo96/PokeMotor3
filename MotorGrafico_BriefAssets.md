# Brief de implementación — Assets: arrastrar al viewport → entidad en escena · PokeMotor

> **Para Claude Code.** Implementar el flujo: arrastrar un asset desde el navegador
> al viewport → cargar el recurso → crear una entidad en la escena → que aparezca
> en la jerarquía, seleccionada y editable en el inspector. **Sin escribir código
> el usuario.**

---

## Contexto del motor

- **PokeMotor**: C++ / Vulkan, RPG estilo Pokémon, 2D.
- **Ya existe** (no reconstruir):
  - **AssetManager** (clase): dueño en runtime de recursos cargados; `loadTexture(path) → TextureHandle`, caché por ruta, pool con generación.
  - Editor: **Jerarquía**, **Inspector** (componentes en colapsables editables),
    navegador de **Assets** (carpeta + miniaturas), **Consola**.
  - Modelo **ECS**: entidades = id + componentes de datos; sistemas.
  - Render 2D: sprites, tilemap, texto MSDF.
- **Lo que falta:** el puente editor que convierte "un asset del navegador" en "una
  entidad con sprite en la escena", reflejada en la jerarquía.

> **Claude Code:** adapta los nombres reales (Scene, Entity, AssetManager,
> Inspector, Console, Camera, el navegador de assets) a los del repo.

---

## Objetivo (v1)

El usuario arrastra `bulbasaur.png` desde el navegador al viewport y, sin tocar
código:
1. El recurso se carga (vía AssetManager, con caché).
2. Se crea una entidad en la posición soltada, con sprite + nombre.
3. La entidad aparece en la **jerarquía**, queda **seleccionada**, y el **inspector**
   muestra sus componentes.

---

## Concepto clave: referencia vs recurso

- **Referencia a asset** (ligera: ruta/id) → lo que el navegador muestra y lo que
  se arrastra. No carga nada.
- **Recurso cargado** (`TextureHandle` + GPU) → lo que el AssetManager posee.
- **Soltar en el viewport = el momento en que una referencia se vuelve recurso
  cargado** y se engancha a una entidad.

---

## Arquitectura — piezas a construir

### 1. `AssetRegistry` — catálogo para el editor (NUEVO)

El AssetManager posee recursos cargados; el editor además necesita *listar* los
assets del proyecto antes de cargarlos. Esa es esta pieza.

```cpp
enum class AssetType { Texture, Model, Script, Font };

struct AssetEntry {
    std::string   path;       // "assets/bulbasaur.png" — id por ahora (ver fase 2: GUIDs)
    AssetType     type;
    TextureHandle preview;    // miniatura (textura ya cargada para el browser)
};

class AssetRegistry {
public:
    void scan(const std::string& root);            // recorre la carpeta, cataloga
    const std::vector<AssetEntry>& entries() const;
    void refresh();                                // re-escanea (botón "Refrescar")
};
```

> El runtime del juego NO necesita el registro; es de cara al editor.

### 2. `NameComponent` — nombre para la jerarquía (NUEVO)

```cpp
struct NameComponent { std::string value; };   // "bulbasaur" en vez de "entidad 7"
```

### 3. Estado de selección del editor (si no existe ya)

```cpp
// En el editor (NO en la Scene — seleccionar es concepto del editor):
Entity g_selected = INVALID_ENTITY;   // compartido por jerarquía, viewport, inspector
```

### 4. Origen de arrastre — miniaturas del navegador

```cpp
// Por cada AssetEntry en el navegador:
ImGui::ImageButton(entry.preview, {64,64});
if (ImGui::BeginDragDropSource()) {
    ImGui::SetDragDropPayload("ASSET_TEXTURE", entry.path.c_str(), entry.path.size()+1);
    ImGui::Image(entry.preview, {48,48});   // preview que sigue al cursor
    ImGui::EndDragDropSource();
}
```

### 5. Destino de arrastre — el viewport

```cpp
// Sobre la imagen/zona del viewport:
if (ImGui::BeginDragDropTarget()) {
    if (auto* p = ImGui::AcceptDragDropPayload("ASSET_TEXTURE")) {
        std::string path((const char*)p->Data);
        Vec2 world = screenToWorld(ImGui::GetMousePos(), camera);
        spawnSpriteEntity(path, world);
    }
    ImGui::EndDragDropTarget();
}
```

`screenToWorld`: convierte el pixel del cursor a coordenadas de mundo usando la
cámara (para 2D ortográfica es un mapeo directo).

### 6. `spawnSpriteEntity` — la bisagra (donde AssetManager y Scene se tocan)

```cpp
Entity spawnSpriteEntity(const std::string& path, Vec2 world) {
    TextureHandle tex = assets.loadTexture(path);      // AssetManager: carga o caché
    Entity e = scene.createEntity();
    scene.add<Transform>(e, { world });
    scene.add<SpriteComponent>(e, { path, tex, 0 }); // texturePath, tex, layer (ver Contrato)
    scene.add<NameComponent>(e, { filenameStem(path) }); // "bulbasaur"
    g_selected = e;                                      // queda seleccionada
    return e;
}
```

El AssetManager no sabe del editor: recibe ruta, devuelve handle. La Scene tampoco.
El editor orquesta.

### 7. Jerarquía — vista de la Scene + selección

```cpp
// El panel de jerarquía, cada frame:
for (Entity e : scene.allEntities()) {
    const char* name = scene.get<NameComponent>(e).value.c_str();
    if (ImGui::Selectable(name, g_selected == e))
        g_selected = e;                 // clic ↔ selección (bidireccional con el inspector)
}
```

La entidad nueva aparece **sola** porque la jerarquía itera la escena viva. No hay
que notificar nada (modo inmediato).

---

## Flujo completo

```
Navegador (AssetRegistry: ruta, tipo, miniatura)
   │  arrastra (payload = ruta)
   ▼
Viewport (drop target)
   │  screenToWorld(cursor, cámara)
   ▼
AssetManager.loadTexture(ruta) → TextureHandle   (caché: no recarga)
   │
   ▼
Scene.createEntity() + Transform + SpriteComponent + NameComponent
   │
   ▼
g_selected = nueva entidad
   │
   ├──► Jerarquía: la itera y la resalta (seleccionada)
   └──► Inspector: lee g_selected → muestra sus componentes
```

---

## Integración con el motor existente

- **AssetManager**: ya tiene `loadTexture`+caché; no cambia. Solo se le llama desde
  `spawnSpriteEntity`.
- **AssetRegistry**: nuevo; lo alimenta el escaneo de la carpeta. El navegador pasa
  a listar `registry.entries()` (y el botón "Refrescar" llama a `refresh()`).
- **Navegador**: añadir el origen de arrastre a cada miniatura.
- **Viewport**: añadir el destino de arrastre.
- **Jerarquía**: iterar `scene.allEntities()` + `NameComponent` + sincronizar
  `g_selected`.
- **Inspector**: ya muestra la entidad seleccionada; debe leer `g_selected`.

---

## Milestones

1. `AssetRegistry::scan` cataloga la carpeta; el navegador lista desde el registro.
2. Origen de arrastre en las miniaturas (el drag arranca y muestra preview).
3. Destino en el viewport + `screenToWorld` (loguea la ruta y la posición al soltar).
4. `spawnSpriteEntity`: al soltar, se crea la entidad con sprite en la posición.
5. `NameComponent` + jerarquía: la entidad aparece con su nombre.
6. Selección: la entidad nueva queda seleccionada → el inspector la muestra; clicar
   en la jerarquía actualiza el inspector.

---

## Criterios de aceptación

- [ ] El navegador lista los assets desde el `AssetRegistry`; "Refrescar" re-escanea.
- [ ] Arrastrar `bulbasaur.png` al viewport crea una entidad **en la posición soltada**.
- [ ] La entidad aparece en la **jerarquía** con el nombre "bulbasaur".
- [ ] Queda **seleccionada**: el inspector muestra Transform + Sprite.
- [ ] Soltar el mismo asset dos veces NO recarga la textura (caché del AssetManager);
      crea dos entidades que comparten el handle.
- [ ] Clicar una entidad en la jerarquía actualiza el inspector.

---

## Fuera de alcance de v1 — NO construir aún

- **Parenting** (entidades padre-hijo en la jerarquía): requiere transforms
  jerárquicos, que el diseño dejó planos a propósito. Fase 2.
- **GUIDs** de assets (referencias estables al renombrar): por ahora, la ruta es el id.
- **Operaciones de jerarquía**: renombrar, borrar, duplicar, reordenar, multi-selección.
- **Undo/redo** del spawn.
- Arrastrar otros tipos (modelos, etc.): empezar solo con texturas/sprites.

---

## Fase 2 (después)

- La **jerarquía como drop target** también (soltar sobre el panel para crear, sin
  apuntar en el viewport).
- **Parenting** por arrastre en la jerarquía (con transforms padre-hijo).
- **GUIDs** + archivos `.meta` (renombrar/mover sin romper referencias).
- Operaciones de jerarquía (borrar, duplicar, renombrar).

---

## Notas de diseño

- **Selección = estado del editor**, no de la Scene. Es el pegamento entre viewport,
  jerarquía e inspector.
- **AssetManager agnóstico al editor:** recibe ruta, devuelve handle. El registro y
  el pegamento viven en el editor.
- **Jerarquía = vista de la Scene:** se actualiza sola por iterar la escena viva.
- **Disciplina:** v1 = solo texturas, sin parenting ni operaciones. Lo demás cuando
  el uso lo pida.
```