#include "Renderer/Camera/PerspectiveCamera.h"

#include <glm/gtc/matrix_transform.hpp>

namespace pk {

Mat4 PerspectiveCamera::view() const {
    return glm::lookAt(m_pos, m_target, Vec3(0.0f, 1.0f, 0.0f));
}

Mat4 PerspectiveCamera::proj() const {
    // GLM_FORCE_DEPTH_ZERO_TO_ONE deja z en [0,1] (Vulkan). El [1][1] *= -1
    // corrige el eje Y invertido del framebuffer de Vulkan (si no, la escena
    // saldría boca abajo).
    Mat4 p = glm::perspective(m_fov, m_aspect, m_near, m_far);
    p[1][1] *= -1.0f;
    return p;
}

}  // namespace pk
