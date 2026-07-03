#version 450

// Upscale del target lowRes (480×270) a la swapchain. El sampler es NEAREST →
// píxeles crujientes (look pixel-art auténtico). v_uv viene de fullscreen.vert.
layout(location = 0) in vec2 v_uv;

layout(set = 0, binding = 0) uniform sampler2D uLowRes;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(uLowRes, v_uv);
}
