#version 450

// FluentUI MSDF shader — multi-channel signed distance field text. The
// canonical sampling recipe from msdfgen (Chlumský): median of RGB encodes
// the signed distance; pxRange scales it to screen-space pixels for a
// proper anti-aliased opacity ramp around the 0.5 threshold.

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

float median(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
}

// sRGB->linear: cancel the swapchain's _SRGB encode so the authored glyph
// color is preserved. The MSDF texel is a signed distance (data), not a
// color, so it is never converted — only the output text color is.
vec3 srgbToLinear(vec3 c) {
    bvec3 cutoff = lessThanEqual(c, vec3(0.04045));
    vec3 lower   = c / 12.92;
    vec3 higher  = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(higher, lower, cutoff);
}

void main() {
    vec3 msd = texture(u_tex, v_uv).rgb;
    float sd = median(msd.r, msd.g, msd.b);
    float screenPxDistance = pc.pxRange * (sd - 0.5);
    float opacity = clamp(screenPxDistance + 0.5, 0.0, 1.0);
    outColor = vec4(srgbToLinear(pc.textColor.rgb), pc.textColor.a * opacity * v_color.a);
}
