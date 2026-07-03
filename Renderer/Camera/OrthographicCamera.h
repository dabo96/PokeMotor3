// Renderer/Camera/OrthographicCamera.h — cámara 2D (modo clásico / HD-2D UI).
// Diseño: MotorGrafico_2DyCamara.md. Centro en mundo + zoom + viewport en píxeles.
// A zoom 1, 1 unidad de mundo = 1 píxel. Eje +Y hacia abajo (cómodo para tilemaps
// y coords de pantalla), coherente con el NDC de Vulkan.
#pragma once

#include "Renderer/Camera/Camera.h"

namespace pk {

class OrthographicCamera : public Camera {
public:
    void  setViewport(float w, float h) { m_w = w; m_h = h; }
    void  setCenter(Vec2 c) { m_center = c; }
    void  setZoom(float z)  { m_zoom = z; }
    Vec2  center() const { return m_center; }
    float zoom()   const { return m_zoom; }

    Mat4 view() const override;
    Mat4 proj() const override;

private:
    Vec2  m_center{ 0.0f, 0.0f };
    float m_zoom = 1.0f;
    float m_w = 1280.0f;
    float m_h = 720.0f;
};

}  // namespace pk
