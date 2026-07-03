# Brief de implementación — SceneManager (cargar / guardar / cambiar escenas) · PokeMotor

> **Para Claude Code.** Implementar el gestor de **archivos de escena**: cargar,
> guardar y cambiar la escena activa, con la serialización mínima que eso requiere.
> Hace persistente el trabajo del editor. **Depende del ECS; va después de él.**

---

## Contexto y las cuatro capas (no confundir)

- **Scene (registro)** — almacén en memoria de entidades + componentes de UN mundo.
  Es el `Scene` del brief del ECS. *La estructura.*
- **SceneManager** — carga/guarda/cambia **archivos de escena** (contenido autorado).
  *Este brief.*
- **GameStack / modos** — estados de jugabilidad (overworld, combate). *Aparte.*
- **Sistema de save** — progreso del jugador (equipo, banderas). *Aparte.*

> **Archivo de escena** (contenido autorado: el layout de un mapa) ≠ **save del
> juego** (la partida del jugador). Este brief es lo primero. El save del jugador
> es otro sistema, **fuera de alcance aquí**.

> **Claude Code:** adapta a los nombres reales (Scene, Entity, AssetManager,
> componentes existentes, el menú Archivo del editor).

---

## Objetivo (v1)

- **Guardar** la escena activa a un archivo (todas las entidades + sus componentes).
- **Cargar** un archivo de escena → poblar una escena nueva y hacerla activa.
- **Nueva** escena vacía.
- En el editor: menú **Archivo → Nuevo / Abrir / Guardar / Guardar como**.
- Resultado: lo que construyes con el drag-drop de assets **persiste** entre sesiones.

---

## Cambio de propiedad

La escena activa pasa a ser **propiedad del SceneManager** (para poder
intercambiarla al cargar). El Engine posee el SceneManager; los sistemas operan
sobre `sceneManager.current()`.

```
Engine → SceneManager → Scene activa (registro ECS)
```

---

## Arquitectura — piezas a construir

### 1. `SceneManager`

```cpp
class SceneManager {
public:
    Scene& current();                              // la escena activa

    void   newScene();                             // vacía, activa
    Scene& load(const std::string& path);          // deserializa → activa
    void   save(const std::string& path);          // serializa la activa
    void   switchTo(const std::string& path);      // (juego) descarga actual, carga otra

    const std::string& currentPath() const;        // para "Guardar" sin "como"
private:
    std::unique_ptr<Scene> m_current;
    std::string            m_currentPath;
    ComponentSerializer    m_serializer;           // ver abajo
    AssetManager*          m_assets = nullptr;     // para resolver rutas → handles
};
```

### 2. Serialización de componentes (la dependencia)

Para guardar/cargar hace falta saber, por entidad, **qué componentes tiene** y
**cómo (de)serializar cada uno**. Dos partes:

**a) Hooks en el pool** (extender `IComponentPool` del ECS):

```cpp
struct IComponentPool {
    virtual ~IComponentPool() = default;
    virtual void remove(Entity) = 0;
    virtual bool has(Entity) const = 0;
    // NUEVO para escenas:
    virtual const char* typeName() const = 0;            // "Transform"
    virtual void serialize(Entity, Json& out) const = 0; // un componente → JSON
    virtual void deserialize(Entity, const Json& in) = 0;// JSON → componente
};
```

`ComponentPool<T>` implementa `serialize`/`deserialize` para `T`. Cómo serializa
los campos de `T`:
- **Si existe reflexión** (`REFLECT(T, ...)`): genérico, recorre los campos.
- **Si no**: una función registrada por tipo (`to_json`/`from_json` de T). Empezar
  así si la reflexión aún no está; migrar a reflexión después.

**b) Registro de tipos** (qué componentes existen):

```cpp
// Registrar cada tipo de componente serializable al arrancar:
sceneManager.registerComponent<Transform>("Transform");
sceneManager.registerComponent<SpriteComponent>("Sprite");
sceneManager.registerComponent<NameComponent>("Name");
sceneManager.registerComponent<ScriptComponent>("Script");
// el registro mapea "Transform" → cómo crear/poblar ese pool al cargar
```

### 3. Formato del archivo de escena (JSON)

Legible en desarrollo. Lista de entidades; cada una, sus componentes:

```json
{
  "scene": "pueblo_paleta",
  "entities": [
    {
      "components": {
        "Name":      { "value": "Player" },
        "Transform": { "x": 5, "y": 5 },
        "Sprite":    { "texture": "assets/bulbasaur.png", "layer": 1 }
      }
    }
  ]
}
```

`save`: recorre `current().allEntities()`; por cada una, recorre los pools que la
tienen y serializa cada componente. `load`: crea escena nueva, por cada entidad
`createEntity()` y por cada componente del JSON usa el registro para añadirlo y
deserializarlo.

### 4. Resolución de assets al cargar

Un componente que referencia un asset (Sprite → textura) **guarda la ruta**, no el
handle (los handles son de runtime). Al cargar, se re-resuelve:

```cpp
// SpriteComponent serializable: guarda la ruta; el handle se resuelve al cargar.
struct SpriteComponent {
    std::string   texturePath;     // se serializa
    TextureHandle tex;             // se resuelve en deserialize: assets.loadTexture(texturePath)
    int           layer = 0;
};
```

En `deserialize` de Sprite: `tex = assets.loadTexture(texturePath);` (con la caché
del AssetManager, no recarga si ya está).

### 5. Integración con el editor (menú Archivo)

- **Nuevo** → `sceneManager.newScene()` (pide confirmar si hay cambios sin guardar).
- **Abrir** → diálogo de archivo → `load(path)`.
- **Guardar** → `save(currentPath())` (o "Guardar como" si no hay ruta).
- **Guardar como** → diálogo → `save(path)`.

Tras `load`, los paneles (jerarquía, inspector) ya muestran la escena nueva porque
son vistas de `sceneManager.current()`.

---

## Integración con lo existente

- **ECS:** extender `IComponentPool` con los hooks de (de)serialización. El
  SceneManager itera entidades × pools.
- **AssetManager:** el SceneManager lo usa para resolver rutas → handles al cargar.
- **Editor:** los sistemas y paneles pasan a operar sobre `sceneManager.current()`
  en vez de una `Scene` suelta.
- **Engine:** posee el SceneManager; este posee la escena activa.

---

## Milestones

1. `SceneManager` con `newScene` + `current`; el Engine/editor operan sobre
   `current()`.
2. Hooks `serialize`/`deserialize` en `ComponentPool<T>` + registro de tipos, para
   un par de componentes (Transform, Name).
3. `save`: la escena activa se escribe a JSON (entidades + componentes).
4. `load`: el JSON reconstruye la escena; los paneles la muestran.
5. Resolución de assets: Sprite guarda la ruta y al cargar resuelve el handle.
6. Menú Archivo (Nuevo/Abrir/Guardar/Guardar como) cableado.

---

## Criterios de aceptación

- [ ] Arrastrar un par de sprites (brief de Assets), **Guardar**, cerrar y reabrir,
      **Abrir** → las entidades vuelven en su posición, con su sprite.
- [ ] El JSON de la escena es legible y lista entidades con sus componentes.
- [ ] Cargar resuelve las texturas por su ruta (vía AssetManager, con caché).
- [ ] "Nuevo" deja la escena vacía; jerarquía e inspector se vacían.
- [ ] Un `ScriptComponent` (brief de Scripting) se guarda con su script + valores de
      exports, y al cargar vuelve a correr con esos valores.

---

## Fuera de alcance — NO construir

- **Save del jugador** (equipo, banderas, posición, slots): es otro sistema.
- **Cambio de escena con transición visual** (fade): la lógica de `switchTo` sí; el
  fade es una capa encima, después.
- **Escenas aditivas / múltiples cargadas a la vez:** una activa basta.
- **Streaming / chunks** de escenas grandes.
- **Versionado/migración** del formato (tolerar campos que faltan está bien; un
  sistema de migración formal, no aún).

---

## Fase 2 (después)

- Transiciones (fade) al `switchTo`, integradas con el post-proceso.
- Conexiones entre mapas (al pisar el borde, `switchTo` la escena vecina).
- Versionado del formato de escena cuando los componentes evolucionen mucho.
- Reflexión reemplazando las funciones `to_json`/`from_json` por tipo.

---

## Notas de diseño

- **Archivo de escena ≠ save del jugador.** Contenido autorado vs progreso. No
  mezclar en el mismo sistema.
- **Los componentes guardan rutas de assets, no handles.** El handle se resuelve al
  cargar.
- **La escena activa es propiedad del SceneManager.** Cargar = intercambiarla.
- **Empezar con `to_json`/`from_json` por componente** si la reflexión no está;
  migrar a reflexión luego (mismo resultado, menos boilerplate).
- **Disciplina:** v1 = guardar/cargar/cambiar una escena. Lo demás cuando se gane.
```