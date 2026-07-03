#version 460
#extension GL_EXT_nonuniform_qualifier : require

// Depth-only: just discards transparent atlas texels so the sprite casts a
// cut-out shadow instead of a solid quad.
layout(set = 0, binding = 0) uniform sampler2D u_textures[256];

struct SpriteInstance {
    mat4 model;
    vec4 color;
    vec4 uvOffsetScale;
    vec4 material;
};
layout(set = 1, binding = 0, std430) readonly buffer SpriteBuffer {
    SpriteInstance sprites[];
};

layout(location = 0) in vec2 v_uv;
layout(location = 1) flat in int v_instance;

void main() {
    SpriteInstance s = sprites[v_instance];
    uint texIndex = floatBitsToUint(s.material.w);
    float alpha = texture(u_textures[texIndex], v_uv).a * s.color.a;
    if (alpha < s.material.x) discard;
}
