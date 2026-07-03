#version 450

// FluentUI Image shader — full RGBA atlas sampling with per-vertex tint.

layout(set = 0, binding = 0) uniform sampler2D u_tex;

layout(push_constant) uniform PC {
    mat4  projection;
    vec4  textColor;     // unused for image — tint comes from v_color
    float pxRange;
    float _pad0, _pad1, _pad2;
} pc;

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;

layout(location = 0) out vec4 outColor;

// The sampled texture is a color image registered in an _SRGB format (e.g.
// the engine's viewport target), so the sampler already returns linear RGB.
// Only the per-vertex tint is authored in perceptual sRGB, so we linearize
// that before multiplying. For a white tint (1,1,1,1) this is a no-op, so the
// 3D viewport keeps looking identical.
vec3 srgbToLinear(vec3 c) {
    bvec3 cutoff = lessThanEqual(c, vec3(0.04045));
    vec3 lower   = c / 12.92;
    vec3 higher  = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(higher, lower, cutoff);
}

void main() {
    vec4 tex = texture(u_tex, v_uv);
    outColor = tex * vec4(srgbToLinear(v_color.rgb), v_color.a);
}
