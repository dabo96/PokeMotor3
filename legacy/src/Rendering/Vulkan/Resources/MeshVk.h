#pragma once

#include "BufferVk.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pokemotor::vk {

struct MeshVertex {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec2 uv;
};

struct MeshCPU {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t>   indices;
    glm::mat4               localTransform = glm::mat4(1.0f);
    // Index into LoadedModel::materials. uint32_t(-1) means "no material".
    uint32_t                sceneMaterialIndex = static_cast<uint32_t>(-1);
};

// Per-aiMaterial metadata captured from the source file.
struct MaterialCPU {
    std::string diffusePath;                            // empty when no baseColorTexture
    std::string metallicRoughnessPath;                  // empty → use factors directly
    glm::vec4   baseColorFactor      = glm::vec4(1.0f); // glTF baseColorFactor
    float       metallicFactor       = 1.0f;            // glTF default
    float       roughnessFactor      = 1.0f;            // glTF default
};

// Result of LoadModelCPU. Materials are parsed once from the scene; each mesh
// references one via sceneMaterialIndex. The Vulkan side resolves
// materials → bindless texture slots + per-mesh push constants at load time.
struct LoadedModel {
    std::vector<MeshCPU>     meshes;
    std::vector<MaterialCPU> materials;
};

class MeshVk {
public:
    MeshVk() = default;
    ~MeshVk() = default;
    MeshVk(const MeshVk&) = delete;
    MeshVk& operator=(const MeshVk&) = delete;
    MeshVk(MeshVk&& other) noexcept = default;
    MeshVk& operator=(MeshVk&& other) noexcept = default;

    // Uploads vertex + index data through a single staging round-trip. Caller-supplied
    // queue/family is used for a transient OneShotCmd. waitIdle on the queue before returning.
    bool Upload(VmaAllocator allocator, VkDevice device,
                uint32_t graphicsQueueFamily, VkQueue graphicsQueue,
                const std::vector<MeshVertex>& vertices,
                const std::vector<uint32_t>& indices);

    void Destroy();

    VkBuffer VertexBuffer() const { return m_vertexBuffer.Handle(); }
    VkBuffer IndexBuffer()  const { return m_indexBuffer.Handle();  }
    uint32_t IndexCount()   const { return m_indexCount; }
    const glm::mat4& LocalTransform() const { return m_localTransform; }
    void SetLocalTransform(const glm::mat4& m) { m_localTransform = m; }

    // Local-space AABB of the raw vertices (pre-localTransform). False if the
    // mesh was never uploaded / had no vertices.
    bool LocalAABB(glm::vec3& outMin, glm::vec3& outMax) const {
        if (!m_aabbValid) return false;
        outMin = m_aabbMin; outMax = m_aabbMax; return true;
    }

    // Per-triangle ray test against the retained CPU geometry. `world` is the
    // entity world matrix (the mesh localTransform is applied internally). ro/rd
    // are the world-space ray (rd need NOT be normalized; tHit comes back in rd
    // units, so it's comparable across meshes sharing the same rd). Returns the
    // nearest hit. False if the mesh kept no CPU geometry or nothing was hit.
    bool Raycast(const glm::mat4& world, const glm::vec3& ro, const glm::vec3& rd,
                 float& tHit) const;

    uint32_t MaterialIndex() const { return m_materialIndex; }
    void SetMaterialIndex(uint32_t i) { m_materialIndex = i; }

    const glm::vec4& BaseColorFactor() const { return m_baseColorFactor; }
    void SetBaseColorFactor(const glm::vec4& c) { m_baseColorFactor = c; }

    // glTF metallicRoughness texture lives at this bindless slot. 0 = fallback
    // white → factors used straight (texture × factor where texture = 1).
    uint32_t MetallicRoughnessIndex() const { return m_metallicRoughnessIndex; }
    void SetMetallicRoughnessIndex(uint32_t i) { m_metallicRoughnessIndex = i; }

    float MetallicFactor() const { return m_metallicFactor; }
    void SetMetallicFactor(float v) { m_metallicFactor = v; }
    float RoughnessFactor() const { return m_roughnessFactor; }
    void SetRoughnessFactor(float v) { m_roughnessFactor = v; }

private:
    BufferVk  m_vertexBuffer;
    BufferVk  m_indexBuffer;
    uint32_t  m_indexCount             = 0;
    uint32_t  m_materialIndex          = 0;
    uint32_t  m_metallicRoughnessIndex = 0;   // fallback slot
    glm::mat4 m_localTransform         = glm::mat4(1.0f);
    glm::vec4 m_baseColorFactor        = glm::vec4(1.0f);
    float     m_metallicFactor         = 1.0f;
    float     m_roughnessFactor        = 1.0f;
    glm::vec3 m_aabbMin                = glm::vec3(0.0f);
    glm::vec3 m_aabbMax                = glm::vec3(0.0f);
    bool      m_aabbValid              = false;

    // CPU-side geometry retained for ray picking (positions + indices only —
    // normals/UVs are not needed for intersection). ~1-2 MB for a typical model;
    // an editor convenience, not used by rendering.
    std::vector<glm::vec3> m_cpuPositions;
    std::vector<uint32_t>  m_cpuIndices;
};

}  // namespace pokemotor::vk
