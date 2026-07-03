#include "Renderer/Camera/OrthographicCamera.h"

#include <glm/gtc/matrix_transform.hpp>

namespace pk {

Mat4 OrthographicCamera::view() const {
    // Traslada el mundo para que m_center quede en el centro de la vista.
    return glm::translate(Mat4(1.0f), Vec3(-m_center.x, -m_center.y, 0.0f));
}

Mat4 OrthographicCamera::proj() const {
    const float halfW = (m_w * 0.5f) / m_zoom;
    const float halfH = (m_h * 0.5f) / m_zoom;
    // En Vulkan, y_ndc=+1 es el BORDE INFERIOR de la pantalla. Con bottom=-halfH,
    // top=+halfH, el mundo +Y mapea a y_ndc creciente → +Y apunta HACIA ABAJO en
    // pantalla (origen arriba-izquierda, como un tilemap). GLM_FORCE_DEPTH_ZERO_TO_ONE
    // deja z en [0,1]. (No se aplica Y-flip: ese es para la perspectiva 3D.)
    return glm::ortho(-halfW, halfW, -halfH, halfH, -1.0f, 1.0f);
}

}  // namespace pk
