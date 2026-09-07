// Game/GameEvents.h — eventos de juego para el EventBus. La lógica emite; otros
// subsistemas (audio, UI, analítica) reaccionan sin acoplarse.
// Diseño: MotorGrafico_EventBus.md.
#pragma once

#include "Core/Math.h"

#include <string>

namespace pk {

struct PlayerMovedEvent { int x; int y; };
struct EncounterEvent   { int speciesId; std::string speciesName; };

// El editor de tiles (2ª ventana) guardó el mapa a 'path'. El overworld lo escucha
// para recargar en caliente y reflejar los cambios sin reiniciar ni pulsar F7.
struct MapSavedEvent { std::string path; };

// Se soltó un asset del navegador sobre el viewport (en píxeles de pantalla). El modo
// activo lo convierte a mundo y crea una entidad con ese sprite.
struct AssetDroppedEvent { std::string path; Vec2 screenPos; };

// Se soltó un .lua del navegador sobre el viewport. A diferencia de un sprite, un script
// no crea nada: el modo resuelve qué entidad hay bajo ese punto (mismo picking que el
// click) y le adjunta el ScriptComponent.
struct ScriptDroppedEvent { std::string path; Vec2 screenPos; };

// El editor (menú Entidad) pide CREAR una entidad. El editor no sabe dónde mira la vista ni
// qué lleva cada tipo de entidad, así que solo declara el QUÉ: lo materializa el modo activo,
// que la coloca en el centro de la vista, la selecciona y avisa con SceneEditedEvent.
enum class NewEntityKind { Empty, Sprite, Camera, TileMap, Player };
struct CreateEntityEvent { NewEntityKind kind; std::string path; };   // path: la textura (Sprite)

// La escena se modificó desde el VIEWPORT (arrastrando un gizmo). El editor lo escucha para
// marcar el proyecto como "cambios sin guardar": las ediciones del inspector ya pasan por él,
// pero las del gizmo ocurren dentro del modo, que no conoce al editor.
struct SceneEditedEvent {};

}  // namespace pk
