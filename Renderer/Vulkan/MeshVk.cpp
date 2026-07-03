#include "Renderer/Vulkan/MeshVk.h"

#include "Renderer/Vulkan/VulkanContext.h"

#include <cstring>

namespace pk {

bool MeshVk::create(VulkanContext& ctx, const MeshData& data) {
    const VkDeviceSize vsize = data.vertices.size() * sizeof(Vertex3D);
    const VkDeviceSize isize = data.indices.size() * sizeof(uint32_t);
    if (vsize == 0 || isize == 0) return false;

    BufferVk vstage, istage;
    if (!vstage.CreateStaging(ctx.allocator(), vsize)) return false;
    if (!istage.CreateStaging(ctx.allocator(), isize)) return false;
    std::memcpy(vstage.Mapped(), data.vertices.data(), static_cast<size_t>(vsize));
    std::memcpy(istage.Mapped(), data.indices.data(),  static_cast<size_t>(isize));

    if (!m_vertex.CreateDeviceLocal(ctx.allocator(), vsize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return false;
    if (!m_index.CreateDeviceLocal(ctx.allocator(), isize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT))   return false;

    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        BufferVk::CmdCopy(cmd, vstage, m_vertex, vsize);
        BufferVk::CmdCopy(cmd, istage, m_index,  isize);
    });
    // staging se destruye al salir del scope (immediateSubmit ya esperó).

    m_indexCount = static_cast<uint32_t>(data.indices.size());
    return true;
}

void MeshVk::destroy() {
    m_vertex.Destroy();
    m_index.Destroy();
    m_indexCount = 0;
}

void MeshVk::draw(VkCommandBuffer cmd) const {
    VkDeviceSize offset = 0;
    VkBuffer vb = m_vertex.Handle();
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    vkCmdBindIndexBuffer(cmd, m_index.Handle(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
}

MeshData MeshVk::cube(float h) {
    MeshData m;
    struct Face { Vec3 n; Vec3 v[4]; };
    const Face faces[6] = {
        {{ 1, 0, 0}, {{ h,-h, h},{ h,-h,-h},{ h, h,-h},{ h, h, h}}},  // +X
        {{-1, 0, 0}, {{-h,-h,-h},{-h,-h, h},{-h, h, h},{-h, h,-h}}},  // -X
        {{ 0, 1, 0}, {{-h, h, h},{ h, h, h},{ h, h,-h},{-h, h,-h}}},  // +Y
        {{ 0,-1, 0}, {{-h,-h,-h},{ h,-h,-h},{ h,-h, h},{-h,-h, h}}},  // -Y
        {{ 0, 0, 1}, {{-h,-h, h},{ h,-h, h},{ h, h, h},{-h, h, h}}},  // +Z
        {{ 0, 0,-1}, {{ h,-h,-h},{-h,-h,-h},{-h, h,-h},{ h, h,-h}}},  // -Z
    };
    const Vec2 uv[4] = { {0, 0}, {1, 0}, {1, 1}, {0, 1} };
    for (const Face& f : faces) {
        uint32_t base = static_cast<uint32_t>(m.vertices.size());
        for (int i = 0; i < 4; ++i) m.vertices.push_back({ f.v[i], f.n, uv[i] });
        m.indices.insert(m.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }
    return m;
}

MeshData MeshVk::plane(float s) {
    MeshData m;
    const Vec3 n{ 0.0f, 1.0f, 0.0f };
    m.vertices = {
        {{-s, 0.0f, -s}, n, {0, 0}},
        {{ s, 0.0f, -s}, n, {1, 0}},
        {{ s, 0.0f,  s}, n, {1, 1}},
        {{-s, 0.0f,  s}, n, {0, 1}},
    };
    // CCW vista desde arriba (+Y), coherente con cull BACK + frontFace CCW.
    m.indices = { 0, 2, 1, 0, 3, 2 };
    return m;
}

}  // namespace pk
