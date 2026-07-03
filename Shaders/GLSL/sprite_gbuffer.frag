#version 460
#extension GL_EXT_nonuniform_qualifier : require

// Writes the same 5 G-Buffer attachments as geometry.frag, plus motion = 0
// (sprites are assumed static for now — motion comes when prevModel is fed
// through). Supports alpha clip per-sprite and per-sprite texture lookup
// through the existing bindless table (set 1 = sampler2D u_textures[]).

layout(set = 1, binding = 0) uniform sampler2D u_textures[256];

struct SpriteInstance {
    mat4 model;
    vec4 color;
    vec4 uvOffsetScale;
    vec4 material;    // alphaClip, metallic, roughness, textureIndex(uint-bitcast)
};

layout(set = 2, binding = 0, std430) readonly buffer SpriteBuffer {
    SpriteInstance sprites[];
};

layout(location = 0) in vec3 v_worldPos;
layout(location = 1) in vec3 v_worldNormal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) flat in int v_instance;

layout(location = 0) out vec4 outPosition;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outAlbedo;
layout(location = 3) out vec4 outEmissive;
layout(location = 4) out vec2 outMotion;

void main() {
    SpriteInstance s = sprites[v_instance];
    uint textureIndex = floatBitsToUint(s.material.w);

    vec4 tex = texture(u_textures[textureIndex], v_uv);
    float alpha = tex.a * s.color.a;
    if (alpha < s.material.x) discard;

    vec3  albedo    = tex.rgb * s.color.rgb;
    float metallic  = s.material.y;
    float roughness = max(s.material.z, 0.04);
    vec3  N         = normalize(v_worldNormal);

    outPosition = vec4(v_worldPos, metallic);
    outNormal   = vec4(N,          roughness);
    outAlbedo   = vec4(albedo,     1.0);
    outEmissive = vec4(0.0);
    outMotion   = vec2(0.0);  // static sprites — TAA will use 0 motion
}
