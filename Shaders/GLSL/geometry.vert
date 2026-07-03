#version 450

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec4 cameraPos;
    vec4 lightDirection;
    vec4 lightColor;
    vec4 ambient;
    mat4 lightSpaceMatrix[3];
    vec4 cascadeSplits;
    mat4 prevViewProj;
    vec4 jitter;          // .xy = current frame NDC jitter, .zw = previous frame
} u;

layout(push_constant) uniform PushConstants {
    mat4  model;
    vec4  baseColorFactor;          // unused in vert; declared so block matches frag.
    uint  materialIndex;            // unused in vert; declared so block matches frag.
    uint  metallicRoughnessIndex;   // unused in vert.
    float metallicFactor;           // unused in vert.
    float roughnessFactor;          // unused in vert.
    float specularAA;               // unused in vert; declared so block matches frag.
} pc;

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;

layout(location = 0) out vec3 v_worldPos;
layout(location = 1) out vec3 v_worldNormal;
layout(location = 2) out vec2 v_uv;
// Current and previous-frame clip-space positions, both jittered. Used by the
// frag shader to compute screen-space motion vectors for TAA reprojection.
layout(location = 3) out vec4 v_currClip;
layout(location = 4) out vec4 v_prevClip;

void main() {
    vec4 world = pc.model * vec4(a_pos, 1.0);
    v_worldPos    = world.xyz;
    v_worldNormal = mat3(pc.model) * a_normal;
    v_uv          = a_uv;

    // Current frame: jittered projection (TAA halton offset added to clip XY).
    vec4 curClip = u.proj * u.view * world;
    curClip.xy += u.jitter.xy * curClip.w;

    // Previous frame: object is assumed static (prevModel = currentModel).
    // Once moving objects join we'll pass a per-mesh prev model matrix too.
    vec4 prevClip = u.prevViewProj * world;
    prevClip.xy  += u.jitter.zw * prevClip.w;

    v_currClip = curClip;
    v_prevClip = prevClip;
    gl_Position = curClip;
}
