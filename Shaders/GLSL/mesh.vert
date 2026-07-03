#version 450

// Pass principal forward (Fase 5). Geometría con normal; ilumina en el fragment.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(set = 0, binding = 0) uniform Camera {
    mat4 viewProj;
    vec4 camPos;
    vec4 camRight;
    vec4 camUp;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambient;
    mat4 lightViewProj;
} u;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColor;   // rgb albedo
    vec4 matParams;   // x = metallic, y = roughness
} pc;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vWorldPos;
layout(location = 2) out vec2 vUV;

void main() {
    vec4 worldPos = pc.model * vec4(inPos, 1.0);
    gl_Position = u.viewProj * worldPos;
    vWorldPos   = worldPos.xyz;
    // Normal en mundo. Asumimos escala uniforme (suficiente aquí; el paso con
    // escala no uniforme usaría la normal-matrix correcta).
    vNormal = mat3(pc.model) * inNormal;
    vUV = inUV;
}
