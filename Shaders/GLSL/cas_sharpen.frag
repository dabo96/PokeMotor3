#version 450

// AMD FidelityFX Contrast Adaptive Sharpening (sharpen-only, no upscale).
// Runs as a dedicated fullscreen pass after tonemap: the _SRGB input sampler
// linearises to [0,1] (the ACES tonemap already clamps there), CAS sharpens,
// and the _SRGB output attachment re-encodes on write. Replaces the unsharp
// mask that used to live inline in tonemap.frag.
layout(set = 0, binding = 0) uniform sampler2D u_input;

layout(push_constant) uniform PC {
    float sharpness;  // 0 = soft, 1 = sharp
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 ts = 1.0 / vec2(textureSize(u_input, 0));

    // 3×3 neighbourhood:  a b c
    //                     d e f
    //                     g h i
    vec3 a = texture(u_input, v_uv + vec2(-ts.x, -ts.y)).rgb;
    vec3 b = texture(u_input, v_uv + vec2( 0.0,  -ts.y)).rgb;
    vec3 c = texture(u_input, v_uv + vec2( ts.x, -ts.y)).rgb;
    vec3 d = texture(u_input, v_uv + vec2(-ts.x,  0.0)).rgb;
    vec3 e = texture(u_input, v_uv).rgb;
    vec3 f = texture(u_input, v_uv + vec2( ts.x,  0.0)).rgb;
    vec3 g = texture(u_input, v_uv + vec2(-ts.x,  ts.y)).rgb;
    vec3 h = texture(u_input, v_uv + vec2( 0.0,   ts.y)).rgb;
    vec3 i = texture(u_input, v_uv + vec2( ts.x,  ts.y)).rgb;

    // Soft min/max over the cross, then folded with the diagonals.
    vec3 mnRGB  = min(min(min(d, e), min(f, b)), h);
    vec3 mnRGB2 = min(mnRGB, min(min(a, c), min(g, i)));
    mnRGB += mnRGB2;

    vec3 mxRGB  = max(max(max(d, e), max(f, b)), h);
    vec3 mxRGB2 = max(mxRGB, max(max(a, c), max(g, i)));
    mxRGB += mxRGB2;

    // Smooth minimum distance to the signal limit, divided by smooth max.
    vec3 rcpMRGB = vec3(1.0) / mxRGB;
    vec3 ampRGB  = clamp(min(mnRGB, vec3(2.0) - mxRGB) * rcpMRGB, 0.0, 1.0);
    ampRGB = inversesqrt(ampRGB);

    // Shaping: sharpness 0→8 (weak), 1→5 (strong).
    float peak = 8.0 - 3.0 * clamp(pc.sharpness, 0.0, 1.0);
    vec3 wRGB  = -vec3(1.0) / (ampRGB * peak);
    vec3 rcpWeightRGB = vec3(1.0) / (1.0 + 4.0 * wRGB);

    vec3 window = (b + d) + (f + h);
    vec3 outc   = clamp((window * wRGB + e) * rcpWeightRGB, 0.0, 1.0);
    outColor = vec4(outc, 1.0);
}
