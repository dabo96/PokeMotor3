// Renderer/2D/Animation.cpp — reproductor de clips + carga JSON.
#include "Renderer/2D/Animation.h"

#include "Assets/AssetManager.h"
#include "Core/Log.h"
#include "Core/Project.h"

#include <json.hpp>

#include <fstream>
#include <utility>

namespace pk {

void Animator::addClip(const std::string& name, AnimationClip clip) {
    m_clips[name] = std::move(clip);
}

void Animator::play(const std::string& name) {
    if (name == m_current) return;
    auto it = m_clips.find(name);
    if (it == m_clips.end()) return;   // clip desconocido: mantiene el actual
    m_current = name;
    m_elapsed = 0.0f;
    m_index   = 0;
    m_frame   = it->second.frames.empty() ? 0 : it->second.frames[0];
}

void Animator::update(float dt) {
    auto it = m_clips.find(m_current);
    if (it == m_clips.end()) return;
    const AnimationClip& c = it->second;
    if (c.frames.size() <= 1 || c.frameTime <= 0.0f) {
        m_frame = c.frames.empty() ? 0 : c.frames[0];
        return;
    }
    m_elapsed += dt;
    while (m_elapsed >= c.frameTime) {
        m_elapsed -= c.frameTime;
        if (m_index + 1 < c.frames.size()) ++m_index;
        else m_index = c.loop ? 0 : c.frames.size() - 1;
    }
    m_frame = c.frames[m_index];
}

AnimationSet loadAnimationSet(AssetManager& assets, const std::string& path) {
    AnimationSet out;
    std::ifstream f(Project::instance().resolveRead(path));
    if (!f) {
        LOG_INFO("Animation: '%s' no encontrado (sin animación).", path.c_str());
        return out;
    }
    try {
        nlohmann::json j;
        f >> j;

        const std::string tex = j.value("texture", std::string());
        TextureHandle h = tex.empty() ? TextureHandle{} : assets.loadTexture(tex);
        // loadTexture devuelve la textura blanca si el archivo falta → tratamos
        // ese caso como "sin hoja" para hacer fallback al sprite único.
        if (tex.empty() || h == assets.whiteTexture()) {
            LOG_WARN("Animation: hoja '%s' no disponible; se usará el sprite único.", tex.c_str());
            return out;
        }

        out.sheet.texture = h;
        out.sheet.columns = j.value("columns", 1);
        out.sheet.rows    = j.value("rows", 1);
        out.texturePath   = tex;   // el caller la serializa en el SpriteComponent

        if (j.contains("clips") && j["clips"].is_object()) {
            for (auto it = j["clips"].begin(); it != j["clips"].end(); ++it) {
                const auto& c = it.value();
                AnimationClip clip;
                if (c.contains("frames") && c["frames"].is_array())
                    for (const auto& fr : c["frames"]) clip.frames.push_back(fr.get<int>());
                if (clip.frames.empty()) clip.frames.push_back(0);
                clip.frameTime = c.value("frameTime", 0.12f);
                clip.loop      = c.value("loop", true);
                out.clips[it.key()] = std::move(clip);
            }
        }

        out.valid = !out.clips.empty();
        LOG_INFO("Animation: '%s' cargado (%zu clips, hoja %dx%d).",
                 path.c_str(), out.clips.size(), out.sheet.columns, out.sheet.rows);
    } catch (...) {
        LOG_WARN("Animation: '%s' corrupto; sin animación.", path.c_str());
        out.valid = false;
    }
    return out;
}

}  // namespace pk
