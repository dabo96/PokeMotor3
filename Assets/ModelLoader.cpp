#include "Assets/ModelLoader.h"

#include "Assets/AssetManager.h"
#include "Core/Log.h"
#include "Renderer/Vulkan/GltfLoader.h"
#include "Scene/Scene.h"

namespace pk {

void loadModelIntoScene(AssetManager& assets, Scene& scene, const std::string& path,
                        const Mat4& root) {
    std::vector<GltfPrimitive> prims = loadGltf(path);
    if (prims.empty()) {
        LOG_WARN("loadModelIntoScene: '%s' sin geometría", path.c_str());
        return;
    }
    for (GltfPrimitive& p : prims) {
        MeshHandle mesh = assets.createMesh(p.data);
        if (!mesh.valid()) continue;

        TextureHandle tex = p.diffusePath.empty() ? assets.whiteTexture()
                                                   : assets.loadTexture(p.diffusePath);
        Material mat;
        mat.albedo    = tex;
        mat.baseColor = p.baseColor;
        mat.metallic  = p.metallic;
        mat.roughness = p.roughness;
        MaterialHandle matH = assets.createMaterial(mat);

        EntityId e = scene.createEntity();
        scene.addRender(e, mesh, matH, root * p.transform);
    }
    LOG_INFO("Modelo en escena: %s (%zu primitivas)", path.c_str(), prims.size());
}

}  // namespace pk
