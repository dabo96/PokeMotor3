#version 450

// FluentUI Basic shader — flat color quads and lines. No texture sampling.

layout(push_constant) uniform PC {
    mat4  projection;
    vec4  textColor;
    float pxRange;
    float _pad0, _pad1, _pad2;
} pc;

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;

layout(location = 0) out vec4 outColor;

// FluentUI colors are authored in perceptual sRGB. The swapchain attachment
// is an _SRGB format, so the hardware re-encodes linear->sRGB on write. We
// undo that here (sRGB->linear) so the encoded result matches the original
// authored color — without this the whole UI looks washed out / lighter.
vec3 srgbToLinear(vec3 c) {
    bvec3 cutoff = lessThanEqual(c, vec3(0.04045));
    vec3 lower   = c / 12.92;
    vec3 higher  = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(higher, lower, cutoff);
}

void main() {
    outColor = vec4(srgbToLinear(v_color.rgb), v_color.a);
}
