#version 450
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_color;
layout(push_constant) uniform PC { mat4 viewProj; } pc;
layout(location = 0) out vec3 v_color;
void main() {
    gl_Position = pc.viewProj * vec4(a_pos, 1.0);
    v_color = a_color;
}
