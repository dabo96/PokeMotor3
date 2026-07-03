#version 450

// Fragmento HD-2D: muestreo NEAREST (lo da el sampler), alpha-clip, e iluminado
// suave (half-lambert) por el sol con sombra. Pixel-art crujiente pero integrado.

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec3 vWorldPos;

layout(set = 0, binding = 0) uniform Camera {
    mat4 viewProj;
    vec4 camPos;
    vec4 camRight;
    vec4 camUp;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambient;
    mat4 lightViewProj;
} u;

layout(set = 0, binding = 2) uniform sampler2D shadowMap;
layout(set = 1, binding = 0) uniform sampler2D spriteTex;

layout(push_constant) uniform Push {
    vec4 center;
    vec4 misc;   // x = alto, y = alphaClip
} pc;

layout(location = 0) out vec4 outColor;

float sunShadow(vec3 worldPos, vec3 N, vec3 L) {
    vec4 lc = u.lightViewProj * vec4(worldPos, 1.0);
    vec3 proj = lc.xyz / lc.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || proj.z > 1.0)
        return 1.0;
    float bias = max(0.0010 * (1.0 - dot(N, L)), 0.0003);
    float current = proj.z - bias;
    vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));
    float sum = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y) {
            float closest = texture(shadowMap, uv + vec2(x, y) * texel).r;
            sum += current <= closest ? 1.0 : 0.0;
        }
    return sum / 9.0;
}

void main() {
    vec4 tex = texture(spriteTex, vUV);
    if (tex.a < pc.misc.y) discard;   // alpha-clip → bordes nítidos + escribe depth

    vec3  N = vec3(0.0, 1.0, 0.0);                 // normal "hacia arriba" (HD-2D)
    vec3  L = normalize(-u.sunDir.xyz);
    float halfLambert = dot(N, L) * 0.5 + 0.5;     // difuso suavizado
    float shadow = sunShadow(vWorldPos, N, L);

    vec3 lit = tex.rgb * (u.ambient.rgb + u.sunColor.rgb * halfLambert * shadow);
    outColor = vec4(lit, 1.0);
}
