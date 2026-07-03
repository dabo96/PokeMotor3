#include "Assets/AssetManager.h"

#include "Core/Log.h"
#include "Core/Project.h"
#include "Renderer/Vulkan/VulkanContext.h"

namespace pk {

bool AssetManager::init(VulkanContext* ctx) {
    m_ctx = ctx;
    Texture white;
    if (!white.createSolid(*m_ctx, 255, 255, 255, 255)) {
        LOG_ERROR("AssetManager: textura blanca por defecto falló");
        return false;
    }
    m_white = m_textures.add(std::move(white));
    return true;
}

void AssetManager::shutdown() {
    m_meshes.clear();
    m_textures.clear();
    m_materials.clear();
    m_texCache.clear();
    m_ctx = nullptr;
}

MeshHandle AssetManager::createMesh(const MeshData& data) {
    MeshVk mesh;
    if (!mesh.create(*m_ctx, data)) return MeshHandle{};   // inválido (generation 0)
    return m_meshes.add(std::move(mesh));
}

TextureHandle AssetManager::loadTexture(const std::string& path, bool srgb,
                                        bool browsable, FilterMode filter) {
    if (!browsable) m_internal.insert(path);   // recurso interno: fuera del navegador

    // Clave de caché por (ruta + filtro): el FilterMode fija el sampler y los mips DENTRO
    // de la Texture, así que el mismo PNG con filtros distintos son recursos distintos
    // (permite calidad por sprite). Pedir otra calidad re-resuelve a su propia textura.
    const std::string key = path + (filter == FilterMode::Pixel ? "|pixel" : "|smooth");
    auto it = m_texCache.find(key);
    if (it != m_texCache.end()) return it->second;         // ya cargada (mismo filtro)

    // La ruta guardada es RELATIVA al proyecto; se resuelve al leer (proyecto → fallback
    // del motor). La caché sigue por la ruta relativa (key), así es portable entre proyectos.
    const std::string full = Project::instance().resolveRead(path);
    Texture tex;
    if (!tex.loadFromFile(*m_ctx, full.c_str(), filter, srgb)) {
        LOG_WARN("AssetManager: '%s' no cargó; usando textura blanca", full.c_str());
        return m_white;
    }
    TextureHandle h = m_textures.add(std::move(tex));
    m_texCache[key] = h;
    return h;
}

MaterialHandle AssetManager::createMaterial(const Material& m) {
    return m_materials.add(Material(m));
}

}  // namespace pk
