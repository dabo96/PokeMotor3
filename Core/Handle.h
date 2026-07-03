// Core/Handle.h — ID ligero y tipado.
// Diseño: MotorGrafico_Core.md (Tipos y handles). La 'generation' detecta
// handles colgados (use-after-free); el tag impide pasar un MeshHandle donde se
// espera un MaterialHandle aunque por dentro ambos sean dos enteros.
#pragma once

#include <cstdint>

namespace pk {

template <typename Tag>
struct Handle {
    uint32_t index      = 0;
    uint32_t generation = 0;   // 0 = inválido / colgado

    bool valid() const { return generation != 0; }
    bool operator==(const Handle& o) const {
        return index == o.index && generation == o.generation;
    }
    bool operator!=(const Handle& o) const { return !(*this == o); }
};

using MeshHandle     = Handle<struct MeshTag>;
using MaterialHandle = Handle<struct MaterialTag>;
using TextureHandle  = Handle<struct TextureTag>;
using ShaderHandle   = Handle<struct ShaderTag>;

}  // namespace pk
