// Core/Math.h — envoltura de GLM con nombres propios del motor.
// Diseño: MotorGrafico_Core.md (Matemáticas). Si mañana se cambia GLM por otra
// librería, solo se toca este archivo: el resto del motor habla de Vec3, no de
// glm::vec3.
#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace pk {

using Vec2  = glm::vec2;
using Vec3  = glm::vec3;
using Vec4  = glm::vec4;
using IVec2 = glm::ivec2;
using IVec3 = glm::ivec3;
using Mat3  = glm::mat3;
using Mat4  = glm::mat4;
using Quat  = glm::quat;

}  // namespace pk
