// Assets/AssetManager.h — único dueño de los recursos pesados (mallas, texturas,
// materiales). Resuelve los handles que el resto del motor usa.
// Diseño: MotorGrafico_AssetManager.md. Un pool por tipo + caché por ruta.
#pragma once

#include "Assets/Material.h"
#include "Assets/ResourcePool.h"
#include "Core/Handle.h"
#include "Renderer/Vulkan/MeshVk.h"
#include "Renderer/Vulkan/Texture.h"

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace pk {

class VulkanContext;

class AssetManager {
public:
    bool init(VulkanContext* ctx);    // crea la textura blanca por defecto
    void shutdown();                  // libera todos los recursos (antes que el allocator)

    MeshHandle     createMesh(const MeshData& data);     // sube a GPU
    // PNG/JPG (caché por ruta). srgb=false para texturas de DATOS (atlas MSDF, etc.).
    // browsable=false para recursos INTERNOS del motor (p.ej. el atlas de la fuente):
    // se cargan igual pero NO aparecen en el navegador de assets del editor.
    // filter (por asset, parámetro FINAL para no romper llamadas existentes):
    // Smooth (defecto) = lineal+mips+premultiplicado para arte HD (sprites de entidades);
    // Pixel = nearest sin mips para pixel-art (tiles del overworld).
    TextureHandle  loadTexture(const std::string& path, bool srgb = true,
                               bool browsable = true, FilterMode filter = FilterMode::Smooth);

    // ¿La textura de 'path' es un asset del juego (navegable) y no un recurso interno?
    bool isBrowsable(const std::string& path) const { return !m_internal.count(path); }
    TextureHandle  whiteTexture() const { return m_white; }
    MaterialHandle createMaterial(const Material& m);

    MeshVk*   getMesh(MeshHandle h)        { return m_meshes.get(h); }
    Texture*  getTexture(TextureHandle h)  { return m_textures.get(h); }
    Material* getMaterial(MaterialHandle h){ return m_materials.get(h); }

    // Enumera las texturas cargadas (clave "ruta|filtro" → handle) para el navegador.
    const std::unordered_map<std::string, TextureHandle>& textureCache() const { return m_texCache; }

private:
    VulkanContext* m_ctx = nullptr;
    ResourcePool<MeshVk,   MeshTag>     m_meshes;
    ResourcePool<Texture,  TextureTag>  m_textures;
    ResourcePool<Material, MaterialTag> m_materials;
    std::unordered_map<std::string, TextureHandle> m_texCache;  // dedup por "ruta|filtro"
    std::unordered_set<std::string>                m_internal;  // recursos no navegables
    TextureHandle m_white;
};

}  // namespace pk
