// Renderer/Camera/Camera.h — interfaz de cámara: solo produce matrices.
// Diseño: MotorGrafico_2DyCamara.md. El renderer solo pide viewProjection(); no
// le importa la clase concreta (ortográfica o perspectiva). El comportamiento
// (seguir, suavizar) vive en un controlador aparte.
#pragma once

#include "Core/Math.h"

namespace pk {

class Camera {
public:
    virtual ~Camera() = default;
    virtual Mat4 view() const = 0;
    virtual Mat4 proj() const = 0;
    Mat4 viewProjection() const { return proj() * view(); }
};

}  // namespace pk
