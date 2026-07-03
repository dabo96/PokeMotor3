// main.cpp — punto de entrada. Crea el Engine y lo corre.
// Diseño: MotorGrafico_Engine.md (Engine en la cima; main solo lo arranca).
#include "Engine/Engine.h"

int main(int /*argc*/, char* /*argv*/[]) {
    pk::Engine engine;
    if (!engine.init()) return 1;
    engine.run();
    return 0;  // Engine::~Engine() hace el shutdown (RAII)
}
