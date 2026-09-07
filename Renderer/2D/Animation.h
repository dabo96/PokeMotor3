// Renderer/2D/Animation.h — clips de animación 2D + reproductor (Animator) +
// carga data-driven desde JSON. Diseño: MotorGrafico_Plan2D.md (Paso 2). Los clips
// en datos permiten iterar sin recompilar (decisión abierta del doc resuelta a
// favor de datos).
#pragma once

#include "Renderer/2D/SpriteSheet.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace pk {

class AssetManager;

struct AnimationClip {
    std::vector<int> frames;          // índices de frame en la hoja
    float            frameTime = 0.12f;  // segundos por frame
    bool             loop      = true;
};

// Avanza un clip con el tiempo y expone el frame de hoja actual.
class Animator {
public:
    void addClip(const std::string& name, AnimationClip clip);
    void play(const std::string& name);   // cambia de clip (no resetea si ya es el actual)
    void update(float dt);

    int  frame()    const { return m_frame; }
    bool hasClips() const { return !m_clips.empty(); }

private:
    std::unordered_map<std::string, AnimationClip> m_clips;
    std::string m_current;
    float       m_elapsed = 0.0f;
    size_t      m_index   = 0;   // posición dentro de clip.frames
    int         m_frame   = 0;   // frame de hoja resultante
};

// Hoja + clips, normalmente cargados de un JSON.
struct AnimationSet {
    SpriteSheet                                    sheet;
    std::unordered_map<std::string, AnimationClip> clips;
    std::string                                    texturePath;  // ruta de la hoja (vacía si no cargó)
    bool                                           valid = false;
};

// Carga desde JSON. valid=false si el archivo o su textura no existen (el caller
// hace fallback). Formato: { "texture", "columns", "rows", "clips": { name: {frames,frameTime,loop} } }.
AnimationSet loadAnimationSet(AssetManager& assets, const std::string& path);

}  // namespace pk
