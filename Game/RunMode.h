// Game/RunMode.h — estado de ejecución del motor (editor ↔ juego).
// Hasta ahora el juego SIEMPRE corría: los scripts movían al jugador y a la cámara
// mientras se editaba. El editor pilota este estado con Play/Pausa/Stop; el Engine lo
// aplica al bucle y lo publica en el GameContext para que cada modo se auto-gatee.
// Header mínimo a propósito: lo incluyen tanto Game/ como Editor/.
#pragma once

namespace pk {

// Edit   = editando: no hay simulación (ni scripts, ni pasos, ni encuentros); sí
//          picking, drops y render de lo que hay en la escena.
// Play   = el juego corre.
// Paused = el juego está montado pero congelado (Play lo reanuda, Stop lo termina).
enum class RunMode { Edit, Play, Paused };

}  // namespace pk
