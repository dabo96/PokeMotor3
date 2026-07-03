#version 460

// Quad-per-instance sprite renderer. Mesh is the shared unit quad (4 verts);
// `gl_InstanceIndex` indexes the sprite SSBO for the per-instance model
// matrix, color, atlas UV, and material parameters.

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
    vec4 jitter;
} u;

struct SpriteInstance {
    mat4 model;
    vec4 color;                // rgba multiplier
    vec4 uvOffsetScale;        // offset.xy, scale.xy
    vec4 material;             // alphaClip, metallic, roughness, textureIndex(uint-bitcast)
};

layout(set = 2, binding = 0, std430) readonly buffer SpriteBuffer {
    SpriteInstance sprites[];
};

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;

layout(location = 0) out vec3 v_worldPos;
layout(location = 1) out vec3 v_worldNormal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) flat out int v_instance;

void main() {
    SpriteInstance s = sprites[gl_InstanceIndex];

    vec4 world = s.model * vec4(a_pos, 1.0);
    v_worldPos    = world.xyz;
    mat3 nm       = transpose(inverse(mat3(s.model)));
    v_worldNormal = normalize(nm * a_normal);
    // Quad mesh UVs are (0,0) at bottom-left; PNG/stb convention is (0,0)
    // at top-left, so flip Y before applying the atlas offset/scale.
    vec2 uv = vec2(a_uv.x, 1.0 - a_uv.y);
    v_uv    = s.uvOffsetScale.xy + uv * s.uvOffsetScale.zw;
    v_instance    = gl_InstanceIndex;

    gl_Position = u.proj * u.view * world;
}
