// Assets/ModelLoader.h — puente glTF → AssetManager + Scene.
// Diseño: MotorGrafico_AssetManager.md (importadores) + Scene.md. Carga un glTF,
// crea mesh+textura+material por primitiva en el AssetManager, y spawnea una
// entidad por primitiva en la Scene.
#pragma once

#include "Core/Math.h"

#include <string>

namespace pk {

class AssetManager;
class Scene;

void loadModelIntoScene(AssetManager& assets, Scene& scene, const std::string& path,
                        const Mat4& root = Mat4(1.0f));

}  // namespace pk
