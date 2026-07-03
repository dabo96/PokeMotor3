// Assets/Material.h — material data-driven.
// Diseño: MotorGrafico_Materiales.md / AssetManager.md. Solo datos: textura de
// albedo (handle) + factores PBR. El descriptor set (set 1) lo construye/cachea
// el Renderer; el material NO conoce Vulkan.
#pragma once

#include "Core/Handle.h"
#include "Core/Math.h"

namespace pk {

struct Material {
    TextureHandle albedo;          // inválida → el Renderer usa la textura blanca 1×1
    Vec4  baseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    float metallic  = 0.0f;
    float roughness = 0.9f;
};

}  // namespace pk
