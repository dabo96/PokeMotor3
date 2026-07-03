// Renderer/Vulkan/MeshVk.h — malla 3D en GPU (vertex + index buffer).
// Diseño: Fase 5 (carga de mallas + pipeline de geometría). De momento mallas
// procedurales (cubo/plano); el cargador glTF llega en el paso 5d.
#pragma once

#include "Core/Math.h"
#include "Renderer/Vulkan/BufferVk.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace pk {

class VulkanContext;

struct Vertex3D {
    Vec3 pos;
    Vec3 normal;
    Vec2 uv;
};

struct MeshData {
    std::vector<Vertex3D> vertices;
    std::vector<uint32_t> indices;
};

class MeshVk {
public:
    bool create(VulkanContext& ctx, const MeshData& data);
    void destroy();
    void draw(VkCommandBuffer cmd) const;   // bind vertex+index + drawIndexed
    uint32_t indexCount() const { return m_indexCount; }

    static MeshData cube(float halfExtent);
    static MeshData plane(float halfSize);

private:
    BufferVk m_vertex;
    BufferVk m_index;
    uint32_t m_indexCount = 0;
};

}  // namespace pk
