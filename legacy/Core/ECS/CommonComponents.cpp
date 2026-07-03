#include "CommonComponents.h"
#include <glm/gtc/matrix_transform.hpp>

namespace ecs {

glm::mat4 TransformComponent::GetMatrix() const {
    glm::mat4 mat = glm::translate(glm::mat4(1.0f), position);
    mat *= glm::mat4_cast(rotation);
    mat = glm::scale(mat, scale);
    return mat;
}

} // namespace ecs
