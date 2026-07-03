#version 450

// Bloom prefilter (Fase 6b): extrae las zonas brillantes (> umbral) con soft-knee.
// Entrada: sceneColor HDR (full-res). Salida: bright (half-res).

layout(set = 0, binding = 0) uniform sampler2D u_scene;

layout(push_constant) uniform PC {
    float threshold;
    float knee;
} pc;

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 outColor;

void main() {
    vec3  c  = texture(u_scene, v_uv).rgb;
    float br = max(c.r, max(c.g, c.b));

    // Soft-knee: transición suave alrededor del umbral (evita el corte duro).
    float soft = clamp(br - pc.threshold + pc.knee, 0.0, 2.0 * pc.knee);
    soft = (soft * soft) / (4.0 * pc.knee + 1e-4);
    float contrib = max(soft, br - pc.threshold) / max(br, 1e-4);

    outColor = vec4(c * contrib, 1.0);
}
