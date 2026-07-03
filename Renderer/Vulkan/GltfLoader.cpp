#include "Renderer/Vulkan/GltfLoader.h"

#include "Core/Log.h"

#include <json.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace pk {

namespace {

using json = nlohmann::json;

constexpr int CT_UNSIGNED_BYTE  = 5121;
constexpr int CT_UNSIGNED_SHORT = 5123;
constexpr int CT_UNSIGNED_INT   = 5125;
constexpr int CT_FLOAT          = 5126;

size_t componentByteSize(int ct) {
    switch (ct) {
        case CT_UNSIGNED_BYTE:  return 1;
        case CT_UNSIGNED_SHORT: return 2;
        case CT_UNSIGNED_INT:   return 4;
        case CT_FLOAT:          return 4;
        default:                return 0;
    }
}

size_t typeComponentCount(const std::string& t) {
    if (t == "SCALAR") return 1;
    if (t == "VEC2")   return 2;
    if (t == "VEC3")   return 3;
    if (t == "VEC4")   return 4;
    return 0;
}

std::vector<uint8_t> readBinaryFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return {};
    std::streamsize size = f.tellg();
    if (size <= 0) return {};
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), size);
    return buf;
}

std::vector<float> readFloatAccessor(const json& gltf, int accessorIdx,
                                     const std::vector<std::vector<uint8_t>>& buffers) {
    const auto& acc = gltf["accessors"][accessorIdx];
    if (acc["componentType"].get<int>() != CT_FLOAT) return {};
    const auto& view = gltf["bufferViews"][acc["bufferView"].get<int>()];
    const int bufIdx = view["buffer"].get<int>();

    const size_t count      = acc["count"].get<size_t>();
    const size_t components = typeComponentCount(acc["type"].get<std::string>());
    const size_t elementSize = components * 4;
    const size_t stride = view.value("byteStride", elementSize);
    const size_t offset = view.value("byteOffset", size_t(0)) + acc.value("byteOffset", size_t(0));

    std::vector<float> out(count * components);
    const uint8_t* base = buffers[bufIdx].data();
    for (size_t i = 0; i < count; ++i)
        std::memcpy(&out[i * components], base + offset + i * stride, elementSize);
    return out;
}

std::vector<uint32_t> readIndexAccessor(const json& gltf, int accessorIdx,
                                        const std::vector<std::vector<uint8_t>>& buffers) {
    const auto& acc = gltf["accessors"][accessorIdx];
    const int compType = acc["componentType"].get<int>();
    const auto& view = gltf["bufferViews"][acc["bufferView"].get<int>()];
    const int bufIdx = view["buffer"].get<int>();

    const size_t count = acc["count"].get<size_t>();
    const size_t elementSize = componentByteSize(compType);
    const size_t stride = view.value("byteStride", elementSize);
    const size_t offset = view.value("byteOffset", size_t(0)) + acc.value("byteOffset", size_t(0));

    std::vector<uint32_t> out(count);
    const uint8_t* base = buffers[bufIdx].data();
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* src = base + offset + i * stride;
        switch (compType) {
            case CT_UNSIGNED_BYTE:  out[i] = *src; break;
            case CT_UNSIGNED_SHORT: { uint16_t v; std::memcpy(&v, src, 2); out[i] = v; break; }
            case CT_UNSIGNED_INT:   { uint32_t v; std::memcpy(&v, src, 4); out[i] = v; break; }
            default: return {};
        }
    }
    return out;
}

Mat4 nodeLocalMatrix(const json& node) {
    if (node.contains("matrix")) {
        const auto& m = node["matrix"];
        Mat4 r(1.0f);
        for (int i = 0; i < 16; ++i) glm::value_ptr(r)[i] = m[i].get<float>();
        return r;
    }
    Vec3 t(0.0f);
    glm::quat rot(1.0f, 0.0f, 0.0f, 0.0f);
    Vec3 s(1.0f);
    if (node.contains("translation")) { const auto& a = node["translation"]; t = { a[0], a[1], a[2] }; }
    if (node.contains("rotation"))    { const auto& a = node["rotation"]; rot = glm::quat(a[3], a[0], a[1], a[2]); }
    if (node.contains("scale"))       { const auto& a = node["scale"]; s = { a[0], a[1], a[2] }; }
    return glm::translate(Mat4(1.0f), t) * glm::mat4_cast(rot) * glm::scale(Mat4(1.0f), s);
}

struct MatFactors {
    Vec4        baseColor{ 1.0f };
    float       metallic  = 0.0f;
    float       roughness = 0.9f;
    std::string diffusePath;
};

void processPrimitive(const json& prim, const json& gltf,
                      const std::vector<std::vector<uint8_t>>& buffers,
                      const Mat4& world, const std::vector<MatFactors>& materials,
                      std::vector<GltfPrimitive>& out) {
    if (prim.contains("mode") && prim["mode"].get<int>() != 4) return;  // solo TRIANGLES
    const auto& attrs = prim["attributes"];
    if (!attrs.contains("POSITION")) return;

    auto positions = readFloatAccessor(gltf, attrs["POSITION"].get<int>(), buffers);
    if (positions.empty()) return;
    const size_t vertCount = positions.size() / 3;

    std::vector<float> normals;
    if (attrs.contains("NORMAL"))
        normals = readFloatAccessor(gltf, attrs["NORMAL"].get<int>(), buffers);
    std::vector<float> uvs;
    if (attrs.contains("TEXCOORD_0"))
        uvs = readFloatAccessor(gltf, attrs["TEXCOORD_0"].get<int>(), buffers);

    GltfPrimitive gp;
    gp.transform = world;
    if (prim.contains("material")) {
        size_t mi = prim["material"].get<size_t>();
        if (mi < materials.size()) {
            gp.baseColor   = materials[mi].baseColor;
            gp.metallic    = materials[mi].metallic;
            gp.roughness   = materials[mi].roughness;
            gp.diffusePath = materials[mi].diffusePath;
        }
    }

    gp.data.vertices.resize(vertCount);
    for (size_t i = 0; i < vertCount; ++i) {
        gp.data.vertices[i].pos = { positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2] };
        if (normals.size() >= (i + 1) * 3)
            gp.data.vertices[i].normal = { normals[i * 3], normals[i * 3 + 1], normals[i * 3 + 2] };
        else
            gp.data.vertices[i].normal = { 0.0f, 1.0f, 0.0f };
        if (uvs.size() >= (i + 1) * 2)
            gp.data.vertices[i].uv = { uvs[i * 2], uvs[i * 2 + 1] };
        else
            gp.data.vertices[i].uv = { 0.0f, 0.0f };
    }

    if (prim.contains("indices")) {
        gp.data.indices = readIndexAccessor(gltf, prim["indices"].get<int>(), buffers);
        if (gp.data.indices.empty()) return;
    } else {
        gp.data.indices.resize(vertCount);
        for (uint32_t i = 0; i < vertCount; ++i) gp.data.indices[i] = i;
    }

    out.push_back(std::move(gp));
}

void processNode(int nodeIdx, const json& gltf,
                 const std::vector<std::vector<uint8_t>>& buffers,
                 const Mat4& parent, const std::vector<MatFactors>& materials,
                 std::vector<GltfPrimitive>& out) {
    const auto& node = gltf["nodes"][nodeIdx];
    Mat4 world = parent * nodeLocalMatrix(node);
    if (node.contains("mesh")) {
        const auto& mesh = gltf["meshes"][node["mesh"].get<int>()];
        for (const auto& prim : mesh["primitives"])
            processPrimitive(prim, gltf, buffers, world, materials, out);
    }
    if (node.contains("children"))
        for (const auto& c : node["children"])
            processNode(c.get<int>(), gltf, buffers, world, materials, out);
}

}  // namespace

std::vector<GltfPrimitive> loadGltf(const std::string& path) {
    std::filesystem::path gltfPath(path);
    std::ifstream in(gltfPath);
    if (!in.is_open()) { LOG_ERROR("glTF: no se pudo abrir '%s'", path.c_str()); return {}; }

    json gltf;
    try { in >> gltf; }
    catch (const std::exception& e) { LOG_ERROR("glTF: error de parseo '%s': %s", path.c_str(), e.what()); return {}; }

    const std::filesystem::path modelDir = gltfPath.parent_path();

    std::vector<std::vector<uint8_t>> buffers;
    if (gltf.contains("buffers")) {
        for (const auto& b : gltf["buffers"]) {
            if (!b.contains("uri")) { LOG_ERROR("glTF: buffer sin uri (GLB no soportado)"); return {}; }
            auto data = readBinaryFile(modelDir / b["uri"].get<std::string>());
            if (data.empty()) { LOG_ERROR("glTF: no se pudo leer un buffer .bin"); return {}; }
            buffers.push_back(std::move(data));
        }
    }

    auto resolveTexturePath = [&](int texIdx) -> std::string {
        if (!gltf.contains("textures")) return {};
        const auto& tex = gltf["textures"][texIdx];
        if (!tex.contains("source")) return {};
        int imgIdx = tex["source"].get<int>();
        if (!gltf.contains("images")) return {};
        const auto& img = gltf["images"][imgIdx];
        if (!img.contains("uri")) return {};
        return (modelDir / img["uri"].get<std::string>()).lexically_normal().string();
    };

    std::vector<MatFactors> materials;
    if (gltf.contains("materials")) {
        for (const auto& mat : gltf["materials"]) {
            MatFactors mf;
            if (mat.contains("pbrMetallicRoughness")) {
                const auto& pbr = mat["pbrMetallicRoughness"];
                if (pbr.contains("baseColorFactor")) {
                    const auto& a = pbr["baseColorFactor"];
                    mf.baseColor = { a[0], a[1], a[2], a[3] };
                }
                if (pbr.contains("metallicFactor"))  mf.metallic  = pbr["metallicFactor"].get<float>();
                if (pbr.contains("roughnessFactor")) mf.roughness = pbr["roughnessFactor"].get<float>();
                if (pbr.contains("baseColorTexture"))
                    mf.diffusePath = resolveTexturePath(pbr["baseColorTexture"]["index"].get<int>());
            }
            materials.push_back(mf);
        }
    }

    std::vector<GltfPrimitive> out;
    int sceneIdx = gltf.value("scene", 0);
    if (gltf.contains("scenes") && static_cast<int>(gltf["scenes"].size()) > sceneIdx) {
        const auto& scene = gltf["scenes"][sceneIdx];
        if (scene.contains("nodes"))
            for (const auto& n : scene["nodes"])
                processNode(n.get<int>(), gltf, buffers, Mat4(1.0f), materials, out);
    }

    LOG_INFO("glTF cargado: %s (%zu primitivas, %zu materiales)",
             path.c_str(), out.size(), materials.size());
    return out;
}

}  // namespace pk
