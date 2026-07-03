// Renderer/Vulkan/GltfLoader.h — carga glTF 2.0 (Fase 5d).
// Diseño: MotorGrafico_IndiceMaestro.md (Fase 5: carga de mallas/modelos glTF).
// Subconjunto soportado: .bin externos (no GLB), POSITION+NORMAL, índices
// UNSIGNED_BYTE/SHORT/INT, TRIANGLES, jerarquía de nodos, factores PBR
// (baseColorFactor / metallicFactor / roughnessFactor). Texturas: más adelante.
#pragma once

#include "Core/Math.h"
#include "Renderer/Vulkan/MeshVk.h"

#include <string>
#include <vector>

namespace pk {

// Una primitiva ya aplanada: geometría + transform de mundo + material resuelto.
struct GltfPrimitive {
    MeshData    data;
    Mat4        transform{ 1.0f };
    Vec4        baseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    float       metallic  = 0.0f;
    float       roughness = 0.9f;
    std::string diffusePath;   // ruta absoluta de la baseColorTexture ("" = sin textura)
};

// Devuelve las primitivas del modelo (vacío si falla; loguea el motivo).
std::vector<GltfPrimitive> loadGltf(const std::string& path);

}  // namespace pk
