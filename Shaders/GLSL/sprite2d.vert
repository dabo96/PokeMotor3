#version 450

// Sprite 2D: vértices en unidades de mundo (coords de tile/píxel), proyectados
// por la matriz ortográfica. La posición/UV/color vienen del SpriteBatch.
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;

layout(push_constant) uniform Push {
    mat4 viewProj;
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

void main() {
    gl_Position = pc.viewProj * vec4(inPos, 0.0, 1.0);
    vUV    = inUV;
    vColor = inColor;
}
