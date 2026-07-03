#version 450

// Blur gaussiano separable de 9 taps (Fase 6b). Se usa dos veces (horizontal y
// vertical) vía el push 'dir' (paso de un téxel en el eje correspondiente).

layout(set = 0, binding = 0) uniform sampler2D u_tex;

layout(push_constant) uniform PC {
    vec2 dir;   // paso de téxel en un eje (1/w,0) o (0,1/h)
} pc;

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 outColor;

void main() {
    const float w[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
    vec3 c = texture(u_tex, v_uv).rgb * w[0];
    for (int i = 1; i < 5; ++i) {
        c += texture(u_tex, v_uv + pc.dir * float(i)).rgb * w[i];
        c += texture(u_tex, v_uv - pc.dir * float(i)).rgb * w[i];
    }
    outColor = vec4(c, 1.0);
}
