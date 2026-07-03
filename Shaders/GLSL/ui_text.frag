#version 450

// FluentUI Text shader — bitmap (alphaOnly = R8) font atlas. The texel's
// R channel is the glyph coverage; final alpha = atlasAlpha * textColor.a
// * vertex.a so per-letter tinting (color hover, dim disabled) still works.

layout(set = 0, binding = 0) uniform sampler2D u_tex;

layout(push_constant) uniform PC {
    mat4  projection;
    vec4  textColor;
    float pxRange;
    float _pad0, _pad1, _pad2;
} pc;

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;

layout(location = 0) out vec4 outColor;

// sRGB->linear: cancel the swapchain's _SRGB encode so the authored text
// color is preserved instead of coming out lighter. Coverage is alpha and
// must NOT be converted.
vec3 srgbToLinear(vec3 c) {
    bvec3 cutoff = lessThanEqual(c, vec3(0.04045));
    vec3 lower   = c / 12.92;
    vec3 higher  = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(higher, lower, cutoff);
}

void main() {
    float coverage = texture(u_tex, v_uv).r;
    outColor = vec4(srgbToLinear(pc.textColor.rgb), pc.textColor.a * coverage * v_color.a);
}
