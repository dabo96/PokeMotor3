#version 450

// Tonemap final (Fase 6): HDR lineal → ACES filmic → swapchain sRGB.
// Último eslabón de la cadena de post. Bloom y DoF se insertarán antes de este.

layout(set = 0, binding = 0) uniform sampler2D u_hdr;
layout(set = 0, binding = 1) uniform sampler2D u_bloom;

layout(push_constant) uniform PC {
    float exposure;        // EV; 0 = sin cambio (exp2(0)=1)
    float bloomIntensity;  // mezcla del bloom
} pc;

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 outColor;

// ACES filmic (fit de Stephen Hill).
const mat3 ACESInput = mat3(
    0.59719, 0.07600, 0.02840,
    0.35458, 0.90834, 0.13383,
    0.04823, 0.01566, 0.83777);
const mat3 ACESOutput = mat3(
     1.60475, -0.10208, -0.00327,
    -0.53108,  1.10813, -0.07276,
    -0.07367, -0.00605,  1.07602);
vec3 RRTAndODTFit(vec3 v) {
    vec3 a = v * (v + 0.0245786) - 0.000090537;
    vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}
vec3 ACESFitted(vec3 c) {
    c = ACESInput * c;
    c = RRTAndODTFit(c);
    c = ACESOutput * c;
    return clamp(c, 0.0, 1.0);
}

void main() {
    vec3 hdr   = texture(u_hdr, v_uv).rgb;
    vec3 bloom = texture(u_bloom, v_uv).rgb;
    hdr += bloom * pc.bloomIntensity;
    hdr *= exp2(pc.exposure);
    vec3 ldr = ACESFitted(max(hdr, vec3(0.0)));

    // Dither suave para romper el banding antes del store a 8 bits sRGB.
    float dither = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    ldr += (dither - 0.5) / 255.0;

    outColor = vec4(ldr, 1.0);
}
