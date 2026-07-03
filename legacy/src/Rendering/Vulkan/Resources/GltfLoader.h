#pragma once

#include "MeshVk.h"

#include <string>

namespace pokemotor::vk {

// Loads a glTF 2.0 model from disk. Supports the subset of the spec we use:
// - Separate .bin buffers (no GLB single-blob, no data: URIs).
// - POSITION (vec3), NORMAL (vec3), TEXCOORD_0 (vec2) vertex attributes.
// - Indexed primitives with UNSIGNED_BYTE/SHORT/INT indices, mode = TRIANGLES.
// - pbrMetallicRoughness baseColorTexture + baseColorFactor.
// - Node hierarchy (matrix or TRS).
//
// Returns an empty LoadedModel on parse error (logs to stderr).
LoadedModel LoadModelCPU(const std::string& path);

}  // namespace pokemotor::vk
