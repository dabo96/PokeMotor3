#include "GltfLoader.h"

#include <json.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace pokemotor::vk {

namespace {

using json = nlohmann::json;

// glTF componentType constants (GL enum values).
constexpr int CT_BYTE           = 5120;
constexpr int CT_UNSIGNED_BYTE  = 5121;
constexpr int CT_SHORT          = 5122;
constexpr int CT_UNSIGNED_SHORT = 5123;
constexpr int CT_UNSIGNED_INT   = 5125;
constexpr int CT_FLOAT          = 5126;

size_t componentByteSize(int compType) {
    switch (compType) {
        case CT_BYTE: case CT_UNSIGNED_BYTE:  return 1;
        case CT_SHORT: case CT_UNSIGNED_SHORT: return 2;
        case CT_UNSIGNED_INT: case CT_FLOAT:   return 4;
        default: return 0;
    }
}

size_t typeComponentCount(const std::string& type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2")   return 2;
    if (type == "VEC3")   return 3;
    if (type == "VEC4")   return 4;
    if (type == "MAT2")   return 4;
    if (type == "MAT3")   return 9;
    if (type == "MAT4")   return 16;
    return 0;
}

std::vector<uint8_t> readBinaryFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return {};
    std::streamsize size = f.tellg();
    if (size <= 0) return {};
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(buf.data()), size);
    return buf;
}

// Copies an accessor's float data into a flat vector. Handles non-zero
// bufferView.byteStride (interleaved attributes) by walking element-by-element.
// Returns count*componentCount floats. Requires componentType == FLOAT.
std::vector<float> readFloatAccessor(const json& gltf, int accessorIdx,
                                     const std::vector<std::vector<uint8_t>>& buffers) {
    const auto& acc = gltf["accessors"][accessorIdx];
    const int compType = acc["componentType"].get<int>();
    if (compType != CT_FLOAT) {
        std::fprintf(stderr, "[Gltf] accessor %d: expected FLOAT, got componentType %d\n",
                     accessorIdx, compType);
        return {};
    }
    const int viewIdx = acc["bufferView"].get<int>();
    const auto& view = gltf["bufferViews"][viewIdx];
    const int bufIdx = view["buffer"].get<int>();

    const size_t count      = acc["count"].get<size_t>();
    const size_t components = typeComponentCount(acc["type"].get<std::string>());
    const size_t elementSize = components * 4;  // float
    const size_t stride     = view.value("byteStride", elementSize);
    const size_t offset     = view.value("byteOffset", size_t(0))
                            + acc.value("byteOffset", size_t(0));

    std::vector<float> out(count * components);
    const uint8_t* base = buffers[bufIdx].data();
    for (size_t i = 0; i < count; ++i) {
        std::memcpy(&out[i * components], base + offset + i * stride, elementSize);
    }
    return out;
}

// Read indices accessor (SCALAR) and promote to uint32_t.
std::vector<uint32_t> readIndexAccessor(const json& gltf, int accessorIdx,
                                        const std::vector<std::vector<uint8_t>>& buffers) {
    const auto& acc = gltf["accessors"][accessorIdx];
    const int compType = acc["componentType"].get<int>();
    const int viewIdx = acc["bufferView"].get<int>();
    const auto& view = gltf["bufferViews"][viewIdx];
    const int bufIdx = view["buffer"].get<int>();

    const size_t count = acc["count"].get<size_t>();
    const size_t elementSize = componentByteSize(compType);
    const size_t stride = view.value("byteStride", elementSize);
    const size_t offset = view.value("byteOffset", size_t(0))
                        + acc.value("byteOffset", size_t(0));

    std::vector<uint32_t> out(count);
    const uint8_t* base = buffers[bufIdx].data();
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* src = base + offset + i * stride;
        switch (compType) {
            case CT_UNSIGNED_BYTE: {
                out[i] = *src;
                break;
            }
            case CT_UNSIGNED_SHORT: {
                uint16_t v;
                std::memcpy(&v, src, 2);
                out[i] = v;
                break;
            }
            case CT_UNSIGNED_INT: {
                uint32_t v;
                std::memcpy(&v, src, 4);
                out[i] = v;
                break;
            }
            default:
                std::fprintf(stderr, "[Gltf] unsupported index componentType %d\n", compType);
                return {};
        }
    }
    return out;
}

glm::mat4 nodeLocalMatrix(const json& node) {
    if (node.contains("matrix")) {
        // glTF matrices are column-major, same as glm — load as 16 floats.
        const auto& m = node["matrix"];
        glm::mat4 result(1.0f);
        for (int i = 0; i < 16; ++i) glm::value_ptr(result)[i] = m[i].get<float>();
        return result;
    }
    glm::vec3 t(0.0f);
    glm::quat r(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 s(1.0f);
    if (node.contains("translation")) {
        const auto& a = node["translation"];
        t = { a[0].get<float>(), a[1].get<float>(), a[2].get<float>() };
    }
    if (node.contains("rotation")) {
        // glTF rotation = [x, y, z, w]; glm::quat constructor = (w, x, y, z).
        const auto& a = node["rotation"];
        r = glm::quat(a[3].get<float>(),
                      a[0].get<float>(), a[1].get<float>(), a[2].get<float>());
    }
    if (node.contains("scale")) {
        const auto& a = node["scale"];
        s = { a[0].get<float>(), a[1].get<float>(), a[2].get<float>() };
    }
    return glm::translate(glm::mat4(1.0f), t)
         * glm::mat4_cast(r)
         * glm::scale(glm::mat4(1.0f), s);
}

void processPrimitive(const json& prim, const json& gltf,
                      const std::vector<std::vector<uint8_t>>& buffers,
                      const glm::mat4& worldTransform,
                      std::vector<MeshCPU>& out) {
    // mode default = 4 (TRIANGLES). Other modes not supported here.
    if (prim.contains("mode") && prim["mode"].get<int>() != 4) {
        std::fprintf(stderr, "[Gltf] skipping primitive with mode %d (only TRIANGLES supported)\n",
                     prim["mode"].get<int>());
        return;
    }

    const auto& attrs = prim["attributes"];
    if (!attrs.contains("POSITION")) {
        std::fprintf(stderr, "[Gltf] primitive missing POSITION\n");
        return;
    }

    auto positions = readFloatAccessor(gltf, attrs["POSITION"].get<int>(), buffers);
    if (positions.empty()) return;
    const size_t vertCount = positions.size() / 3;

    std::vector<float> normals;
    if (attrs.contains("NORMAL")) {
        normals = readFloatAccessor(gltf, attrs["NORMAL"].get<int>(), buffers);
    }
    std::vector<float> uvs;
    if (attrs.contains("TEXCOORD_0")) {
        uvs = readFloatAccessor(gltf, attrs["TEXCOORD_0"].get<int>(), buffers);
    }

    MeshCPU m;
    m.localTransform     = worldTransform;
    m.sceneMaterialIndex = prim.contains("material")
        ? prim["material"].get<uint32_t>()
        : static_cast<uint32_t>(-1);
    m.vertices.resize(vertCount);
    for (size_t i = 0; i < vertCount; ++i) {
        m.vertices[i].pos = {
            positions[i * 3 + 0],
            positions[i * 3 + 1],
            positions[i * 3 + 2],
        };
        if (normals.size() >= (i + 1) * 3) {
            m.vertices[i].normal = {
                normals[i * 3 + 0],
                normals[i * 3 + 1],
                normals[i * 3 + 2],
            };
        } else {
            m.vertices[i].normal = { 0.0f, 1.0f, 0.0f };
        }
        if (uvs.size() >= (i + 1) * 2) {
            m.vertices[i].uv = { uvs[i * 2 + 0], uvs[i * 2 + 1] };
        } else {
            m.vertices[i].uv = { 0.0f, 0.0f };
        }
    }

    if (prim.contains("indices")) {
        m.indices = readIndexAccessor(gltf, prim["indices"].get<int>(), buffers);
        if (m.indices.empty()) return;
    } else {
        // Non-indexed: implicit 0..vertCount-1.
        m.indices.resize(vertCount);
        for (uint32_t i = 0; i < vertCount; ++i) m.indices[i] = i;
    }

    out.push_back(std::move(m));
}

void processNode(int nodeIdx, const json& gltf,
                 const std::vector<std::vector<uint8_t>>& buffers,
                 const glm::mat4& parentTransform,
                 std::vector<MeshCPU>& out) {
    const auto& node = gltf["nodes"][nodeIdx];
    glm::mat4 world = parentTransform * nodeLocalMatrix(node);

    if (node.contains("mesh")) {
        const int meshIdx = node["mesh"].get<int>();
        const auto& mesh = gltf["meshes"][meshIdx];
        for (const auto& prim : mesh["primitives"]) {
            processPrimitive(prim, gltf, buffers, world, out);
        }
    }
    if (node.contains("children")) {
        for (const auto& child : node["children"]) {
            processNode(child.get<int>(), gltf, buffers, world, out);
        }
    }
}

void loadMaterials(const json& gltf, const std::filesystem::path& modelDir,
                   LoadedModel& out) {
    if (!gltf.contains("materials")) return;

    auto resolveImagePath = [&](int textureIdx) -> std::string {
        if (!gltf.contains("textures")) return {};
        const auto& tex = gltf["textures"][textureIdx];
        if (!tex.contains("source")) return {};
        int imgIdx = tex["source"].get<int>();
        if (!gltf.contains("images")) return {};
        const auto& img = gltf["images"][imgIdx];
        if (!img.contains("uri")) return {};
        std::filesystem::path rel = img["uri"].get<std::string>();
        if (rel.is_absolute()) return rel.string();
        return (modelDir / rel).lexically_normal().string();
    };

    for (size_t i = 0; i < gltf["materials"].size(); ++i) {
        const auto& mat = gltf["materials"][i];
        MaterialCPU m;
        if (mat.contains("pbrMetallicRoughness")) {
            const auto& pbr = mat["pbrMetallicRoughness"];
            if (pbr.contains("baseColorFactor")) {
                const auto& a = pbr["baseColorFactor"];
                m.baseColorFactor = {
                    a[0].get<float>(), a[1].get<float>(),
                    a[2].get<float>(), a[3].get<float>(),
                };
            }
            if (pbr.contains("baseColorTexture")) {
                const int texIdx = pbr["baseColorTexture"]["index"].get<int>();
                m.diffusePath = resolveImagePath(texIdx);
            }
            // glTF 2.0 §5.22: metallicFactor / roughnessFactor default to 1.0;
            // metallicRoughnessTexture stores metalness in B, roughness in G.
            if (pbr.contains("metallicFactor")) {
                m.metallicFactor = pbr["metallicFactor"].get<float>();
            }
            if (pbr.contains("roughnessFactor")) {
                m.roughnessFactor = pbr["roughnessFactor"].get<float>();
            }
            if (pbr.contains("metallicRoughnessTexture")) {
                const int texIdx = pbr["metallicRoughnessTexture"]["index"].get<int>();
                m.metallicRoughnessPath = resolveImagePath(texIdx);
            }
        }
        std::printf("[Gltf] material[%zu] diffuse='%s' mr='%s' baseColor=(%.2f,%.2f,%.2f,%.2f) m=%.2f r=%.2f\n",
                    i,
                    m.diffusePath.empty() ? "(none)" : m.diffusePath.c_str(),
                    m.metallicRoughnessPath.empty() ? "(none)" : m.metallicRoughnessPath.c_str(),
                    m.baseColorFactor.r, m.baseColorFactor.g,
                    m.baseColorFactor.b, m.baseColorFactor.a,
                    m.metallicFactor, m.roughnessFactor);
        out.materials.push_back(std::move(m));
    }
}

}  // namespace

LoadedModel LoadModelCPU(const std::string& path) {
    std::filesystem::path gltfPath(path);
    std::ifstream in(gltfPath);
    if (!in.is_open()) {
        std::fprintf(stderr, "[Gltf] cannot open '%s'\n", path.c_str());
        return {};
    }

    json gltf;
    try {
        in >> gltf;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[Gltf] JSON parse error for '%s': %s\n", path.c_str(), e.what());
        return {};
    }

    const std::filesystem::path modelDir = gltfPath.parent_path();

    // 1. Resolve external .bin buffers.
    std::vector<std::vector<uint8_t>> buffers;
    if (gltf.contains("buffers")) {
        for (size_t i = 0; i < gltf["buffers"].size(); ++i) {
            const auto& b = gltf["buffers"][i];
            if (!b.contains("uri")) {
                std::fprintf(stderr, "[Gltf] buffer %zu has no uri (GLB blobs unsupported)\n", i);
                return {};
            }
            std::filesystem::path bufPath = modelDir / b["uri"].get<std::string>();
            auto data = readBinaryFile(bufPath);
            if (data.empty()) {
                std::fprintf(stderr, "[Gltf] failed reading buffer '%s'\n", bufPath.string().c_str());
                return {};
            }
            std::printf("[Gltf] buffer[%zu] %s (%zu bytes)\n", i, bufPath.string().c_str(), data.size());
            buffers.push_back(std::move(data));
        }
    }

    LoadedModel result;

    // 2. Materials (in scene order).
    loadMaterials(gltf, modelDir, result);

    // 3. Walk scene nodes.
    int sceneIdx = gltf.value("scene", 0);
    if (gltf.contains("scenes") && static_cast<int>(gltf["scenes"].size()) > sceneIdx) {
        const auto& scene = gltf["scenes"][sceneIdx];
        if (scene.contains("nodes")) {
            for (const auto& nodeRef : scene["nodes"]) {
                processNode(nodeRef.get<int>(), gltf, buffers, glm::mat4(1.0f), result.meshes);
            }
        }
    }

    std::printf("[Gltf] %s -> %zu submesh(es), %zu material(s)\n",
                path.c_str(), result.meshes.size(), result.materials.size());
    for (size_t i = 0; i < result.meshes.size(); ++i) {
        std::printf("[Gltf]   mesh[%zu] verts=%zu indices=%zu sceneMaterialIndex=%u\n",
                    i, result.meshes[i].vertices.size(), result.meshes[i].indices.size(),
                    result.meshes[i].sceneMaterialIndex);
    }
    return result;
}

}  // namespace pokemotor::vk
