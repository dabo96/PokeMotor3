// Scene/SpriteRenderSystem.cpp — implementación del render-feed 2D.
#include "Scene/SpriteRenderSystem.h"

#include "Renderer/2D/Sprite.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

namespace pk {

void collectEntitySprites(Scene& scene, std::vector<Sprite>& out) {
    scene.view<Transform, SpriteComponent>().each(
        [&out](Entity, Transform& tr, SpriteComponent& sp) {
            Sprite s;
            s.position = tr.position;
            s.size     = tr.scale;
            s.rotation = tr.rotation;
            s.uvRect   = sp.uvRect;     // frame de la hoja (o 0,0,1,1 = textura completa)
            s.color    = sp.tint;       // rgb tinte, a = opacidad
            s.layer    = sp.layer;
            s.texture  = sp.tex;
            out.push_back(s);
        });
}

}  // namespace pk
