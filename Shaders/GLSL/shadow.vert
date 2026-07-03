#version 450

// Shadow pass (Fase 5c): depth-only desde el punto de vista del sol. Sin
// fragment shader ni color attachment — solo escribe profundidad. El push
// constant trae mvp = lightViewProj * model ya combinado en CPU.

layout(location = 0) in vec3 inPos;

layout(push_constant) uniform Push {
    mat4 mvp;
} pc;

void main() {
    gl_Position = pc.mvp * vec4(inPos, 1.0);
}
