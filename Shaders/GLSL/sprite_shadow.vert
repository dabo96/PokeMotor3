#version 460

// Alpha-tested shadow depth for billboard sprites. Reads the same sprite SSBO
// as the G-Buffer pass; transforms by the cascade's light-space matrix (push
// constant). The fragment shader does the alpha discard against the atlas.
struct SpriteInstance {
    mat4 model;
    vec4 color;
    vec4 uvOffsetScale;
    vec4 material;
};
layout(set = 1, binding = 0, std430) readonly buffer SpriteBuffer {
    SpriteInstance sprites[];
};

layout(push_constant) uniform PC { mat4 lightSpace; } pc;

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;

layout(location = 0) out vec2 v_uv;
layout(location = 1) flat out int v_instance;

void main() {
    SpriteInstance s = sprites[gl_InstanceIndex];
    vec4 world = s.model * vec4(a_pos, 1.0);
    vec2 uv = vec2(a_uv.x, 1.0 - a_uv.y);
    v_uv = s.uvOffsetScale.xy + uv * s.uvOffsetScale.zw;
    v_instance = gl_InstanceIndex;
    gl_Position = pc.lightSpace * world;
}
