// Renderer/Camera/PerspectiveCamera.h — cámara 3D / HD-2D.
// Diseño: MotorGrafico_2DyCamara.md. lookAt + perspective. Aplica el Y-flip de
// Vulkan en la proyección.
#pragma once

#include "Renderer/Camera/Camera.h"

namespace pk {

class PerspectiveCamera : public Camera {
public:
    void setAspect(float a)            { m_aspect = a; }
    void setFov(float radians)         { m_fov = radians; }
    void setClip(float n, float f)     { m_near = n; m_far = f; }
    void setPosition(Vec3 p)           { m_pos = p; }
    void setTarget(Vec3 t)             { m_target = t; }
    Vec3 position() const              { return m_pos; }

    Mat4 view() const override;
    Mat4 proj() const override;

private:
    Vec3  m_pos{ 0.0f, 1.5f, 4.0f };
    Vec3  m_target{ 0.0f, 0.0f, 0.0f };
    float m_fov    = 1.0471975f;   // 60°
    float m_aspect = 16.0f / 9.0f;
    float m_near   = 0.1f;
    float m_far    = 200.0f;
};

}  // namespace pk
