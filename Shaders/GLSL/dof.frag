#version 450

// DoF / tilt-shift (Fase 6c): desenfoque por gradiente vertical. La banda central
// (alrededor de focusCenter) queda nítida; arriba y abajo se desenfocan → efecto
// de maqueta/diorama, el corazón del look HD-2D (MotorGrafico_PostProceso.md).
// Versión screen-space (no usa depth); un disco de muestreo escalado por el CoC.

layout(set = 0, binding = 0) uniform sampler2D u_color;

layout(push_constant) uniform PC {
    float focusCenter;  // uv.y del centro de la banda nítida (0.5)
    float focusRange;   // mitad del ancho de la banda nítida
    float maxRadius;    // radio máximo de desenfoque (en uv)
} pc;

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 outColor;

void main() {
    float d   = abs(v_uv.y - pc.focusCenter);
    float coc = clamp((d - pc.focusRange) / max(1.0 - pc.focusRange, 1e-4), 0.0, 1.0);
    coc *= coc;                        // transición suave hacia los bordes
    float r = coc * pc.maxRadius;

    vec3 c = texture(u_color, v_uv).rgb;
    if (r < 1e-5) {
        outColor = vec4(c, 1.0);
        return;
    }

    // Disco de 16 muestras en anillo + centro.
    vec3  acc = c;
    float w   = 1.0;
    const int N = 16;
    for (int i = 0; i < N; ++i) {
        float a = (float(i) / float(N)) * 6.2831853;
        vec2  off = vec2(cos(a), sin(a)) * r;
        acc += texture(u_color, v_uv + off).rgb;
        w   += 1.0;
    }
    outColor = vec4(acc / w, 1.0);
}
