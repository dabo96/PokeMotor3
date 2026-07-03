#version 450

// FluentUI shared vertex shader. Consumes FluentUI::RenderVertex (32 B):
//   vec2 pos, vec4 color, vec2 uv. The host projection (orthographic NDC
//   for the current viewport) lives in a push-constant block.

layout(push_constant) uniform PC {
    mat4  projection;
    vec4  textColor;     // unused in vert
    float pxRange;       // unused in vert
    float _pad0, _pad1, _pad2;
} pc;

layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_uv;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;

void main() {
    gl_Position = pc.projection * vec4(a_pos, 0.0, 1.0);
    v_color = a_color;
    v_uv    = a_uv;
}
