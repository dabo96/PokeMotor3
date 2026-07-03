#version 450

// Billboard HD-2D (Fase 5e): un quad que mira a la cámara. Los vértices se
// generan en el shader desde el centro + tamaño, usando los ejes de cámara
// (camRight/camUp) del set 0. Se dibuja con vkCmdDraw(6, 1, 0, 0).

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
    vec4 center;   // xyz = centro en mundo, w = ancho
    vec4 misc;     // x = alto, y = alphaClip
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec3 vWorldPos;

vec2 corners[6] = vec2[](
    vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(0.5, 0.5),
    vec2(-0.5, -0.5), vec2(0.5,  0.5), vec2(-0.5, 0.5)
);

void main() {
    vec2 c = corners[gl_VertexIndex];
    vec3 worldPos = pc.center.xyz
                  + u.camRight.xyz * (c.x * pc.center.w)
                  + u.camUp.xyz    * (c.y * pc.misc.x);
    gl_Position = u.viewProj * vec4(worldPos, 1.0);
    vUV = vec2(c.x + 0.5, 0.5 - c.y);   // y-flip: c.y=+0.5 (arriba) → uv.y=0 (top)
    vWorldPos = worldPos;
}
