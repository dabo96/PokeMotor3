// Scene/SpriteRenderSystem.h — puente ECS → render 2D: recoge las entidades con
// Transform + SpriteComponent y produce Sprites para el SpriteBatch existente.
// Diseño: MotorGrafico_BriefRenderFeed.md. Es un SISTEMA (corre por frame), no un
// componente; mantiene el desacople Scene↔renderer (el renderer no conoce el ECS).
#pragma once

#include <vector>

namespace pk {

class Scene;
struct Sprite;

// Añade a 'out' un Sprite por cada entidad con Transform + SpriteComponent.
void collectEntitySprites(Scene& scene, std::vector<Sprite>& out);

}  // namespace pk
