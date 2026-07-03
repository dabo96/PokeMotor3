#include "MeshVk.h"

#include <cstdio>
#include <cstring>

namespace pokemotor::vk {

namespace {

struct OneShotCmd {
    OneShotCmd(VkDevice dev, uint32_t queueFamily, VkQueue queue)
        : m_device(dev), m_queue(queue) {
        VkCommandPoolCreateInfo pi{};
        pi.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = queueFamily;
        vkCreateCommandPool(dev, &pi, nullptr, &m_pool);

        VkCommandBufferAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool        = m_pool;
        ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev, &ai, &m_cmd);

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(m_cmd, &bi);
    }
    ~OneShotCmd() {
        vkEndCommandBuffer(m_cmd);
        VkSubmitInfo s{};
        s.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        s.commandBufferCount = 1;
        s.pCommandBuffers    = &m_cmd;
        vkQueueSubmit(m_queue, 1, &s, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_queue);
        vkDestroyCommandPool(m_device, m_pool, nullptr);
    }
    VkCommandBuffer cmd() const { return m_cmd; }
    OneShotCmd(const OneShotCmd&) = delete;
    OneShotCmd& operator=(const OneShotCmd&) = delete;
private:
    VkDevice        m_device;
    VkQueue         m_queue;
    VkCommandPool   m_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd  = VK_NULL_HANDLE;
};

}  // namespace

bool MeshVk::Upload(VmaAllocator allocator, VkDevice device,
                    uint32_t graphicsQueueFamily, VkQueue graphicsQueue,
                    const std::vector<MeshVertex>& vertices,
                    const std::vector<uint32_t>& indices) {
    if (vertices.empty() || indices.empty()) {
        std::fprintf(stderr, "[MeshVk] empty vertex/index data\n");
        return false;
    }

    // Compute the local-space AABB of the raw vertices before staging, so a
    // later failure early-return still leaves a valid AABB (the geometry is
    // known at this point regardless of GPU upload success). Also retain the
    // positions + indices on the CPU for per-triangle ray picking in the editor.
    if (!vertices.empty()) {
        m_aabbMin = m_aabbMax = vertices[0].pos;
        m_cpuPositions.resize(vertices.size());
        for (size_t k = 0; k < vertices.size(); ++k) {
            const glm::vec3& p = vertices[k].pos;
            m_cpuPositions[k] = p;
            m_aabbMin = glm::min(m_aabbMin, p);
            m_aabbMax = glm::max(m_aabbMax, p);
        }
        m_cpuIndices = indices;
        m_aabbValid = true;
    }

    const VkDeviceSize vbSize = vertices.size() * sizeof(MeshVertex);
    const VkDeviceSize ibSize = indices.size()  * sizeof(uint32_t);
    m_indexCount = static_cast<uint32_t>(indices.size());

    BufferVk vbStaging, ibStaging;
    if (!vbStaging.CreateStaging(allocator, vbSize)) return false;
    if (!ibStaging.CreateStaging(allocator, ibSize)) return false;
    std::memcpy(vbStaging.Mapped(), vertices.data(), vbSize);
    std::memcpy(ibStaging.Mapped(), indices.data(),  ibSize);

    if (!m_vertexBuffer.CreateDeviceLocal(allocator, vbSize,
                                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return false;
    if (!m_indexBuffer.CreateDeviceLocal(allocator, ibSize,
                                         VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) return false;

    {
        OneShotCmd one(device, graphicsQueueFamily, graphicsQueue);
        BufferVk::CmdCopy(one.cmd(), vbStaging, m_vertexBuffer, vbSize);
        BufferVk::CmdCopy(one.cmd(), ibStaging, m_indexBuffer,  ibSize);
    }
    return true;
}

void MeshVk::Destroy() {
    m_vertexBuffer.Destroy();
    m_indexBuffer.Destroy();
    m_indexCount = 0;
}

namespace {

// Möller–Trumbore ray/triangle intersection. Double-sided (no cull). rd need
// not be normalized; on hit, outT is the parameter along rd (>= 0). Returns
// false for parallel rays, misses, or hits behind the origin.
bool rayTriangle(const glm::vec3& ro, const glm::vec3& rd,
                 const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
                 float& outT) {
    constexpr float kEps = 1e-8f;
    const glm::vec3 e1 = v1 - v0;
    const glm::vec3 e2 = v2 - v0;
    const glm::vec3 p  = glm::cross(rd, e2);
    const float det = glm::dot(e1, p);
    if (det > -kEps && det < kEps) return false;     // ray parallel to triangle
    const float invDet = 1.0f / det;
    const glm::vec3 tvec = ro - v0;
    const float u = glm::dot(tvec, p) * invDet;
    if (u < 0.0f || u > 1.0f) return false;
    const glm::vec3 q = glm::cross(tvec, e1);
    const float v = glm::dot(rd, q) * invDet;
    if (v < 0.0f || u + v > 1.0f) return false;
    const float t = glm::dot(e2, q) * invDet;
    if (t < 0.0f) return false;                       // behind the ray origin
    outT = t;
    return true;
}

}  // namespace

bool MeshVk::Raycast(const glm::mat4& world, const glm::vec3& ro,
                     const glm::vec3& rd, float& tHit) const {
    if (m_cpuPositions.empty() || m_cpuIndices.size() < 3) return false;

    // Transform each triangle to world space (world * localTransform) and test
    // the world-space ray directly, so tHit stays in world rd-units (comparable
    // across meshes). Vertex transforms dominate, but this only runs on a click.
    const glm::mat4 full = world * m_localTransform;
    float best = tHit;          // caller seeds tHit with its running nearest;
    bool  hit  = false;         // we only accept strictly-closer hits.
    for (size_t i = 0; i + 2 < m_cpuIndices.size(); i += 3) {
        const uint32_t i0 = m_cpuIndices[i];
        const uint32_t i1 = m_cpuIndices[i + 1];
        const uint32_t i2 = m_cpuIndices[i + 2];
        if (i0 >= m_cpuPositions.size() || i1 >= m_cpuPositions.size() ||
            i2 >= m_cpuPositions.size()) continue;
        const glm::vec3 v0 = glm::vec3(full * glm::vec4(m_cpuPositions[i0], 1.0f));
        const glm::vec3 v1 = glm::vec3(full * glm::vec4(m_cpuPositions[i1], 1.0f));
        const glm::vec3 v2 = glm::vec3(full * glm::vec4(m_cpuPositions[i2], 1.0f));
        float t;
        if (rayTriangle(ro, rd, v0, v1, v2, t) && t < best) { best = t; hit = true; }
    }
    if (hit) tHit = best;
    return hit;
}

}  // namespace pokemotor::vk
