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

}  // namespace pk
