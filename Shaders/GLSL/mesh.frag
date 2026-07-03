#version 450

// Pass principal forward — PBR Cook-Torrance + sombra del sol (Fase 5c).

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vWorldPos;
layout(location = 2) in vec2 vUV;

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

struct GpuLight {
    vec4 posType;
    vec4 dirRange;
    vec4 color;
    vec4 spot;
};
layout(std430, set = 0, binding = 1) readonly buffer Lights {
    uint     count;
    GpuLight lights[];
} lb;

layout(set = 0, binding = 2) uniform sampler2D shadowMap;  // depth del sol

layout(set = 1, binding = 0) uniform sampler2D albedoTex;  // textura del material

layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColor;
    vec4 matParams;  // x = metallic, y = roughness
} pc;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

float D_GGX(float NoH, float a) {
    float a2 = a * a;
    float d  = (NoH * NoH * (a2 - 1.0) + 1.0);
    return a2 / max(PI * d * d, 1e-7);
}
float G_Smith(float NoV, float NoL, float a) {
    float k  = (a * a) * 0.5;
    float gv = NoV / (NoV * (1.0 - k) + k);
    float gl = NoL / (NoL * (1.0 - k) + k);
    return gv * gl;
}
vec3 F_Schlick(float VoH, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - VoH, 0.0, 1.0), 5.0);
}

vec3 brdf(vec3 N, vec3 V, vec3 L, vec3 radiance,
          vec3 albedo, float metallic, float rough) {
    float NoL = max(dot(N, L), 0.0);
    if (NoL <= 0.0) return vec3(0.0);
    vec3  H   = normalize(V + L);
    float NoV = max(dot(N, V), 1e-4);
    float NoH = max(dot(N, H), 0.0);
    float VoH = max(dot(V, H), 0.0);
    vec3  F0  = mix(vec3(0.04), albedo, metallic);
    float a   = max(rough * rough, 1e-3);
    float D   = D_GGX(NoH, a);
    float G   = G_Smith(NoV, NoL, a);
    vec3  F   = F_Schlick(VoH, F0);
    vec3 spec = (D * G * F) / max(4.0 * NoV * NoL, 1e-4);
    vec3 kd   = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diff = kd * albedo / PI;
    return (diff + spec) * radiance * NoL;
}

// 1.0 = totalmente iluminado, 0.0 = en sombra. PCF 3x3.
float sunShadow(vec3 worldPos, vec3 N, vec3 L) {
    vec4 lc = u.lightViewProj * vec4(worldPos, 1.0);
    vec3 proj = lc.xyz / lc.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || proj.z > 1.0)
        return 1.0;  // fuera del shadow map → iluminado
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
    vec3  N        = normalize(vNormal);
    vec3  V        = normalize(u.camPos.xyz - vWorldPos);
    vec3  albedo   = texture(albedoTex, vUV).rgb * pc.baseColor.rgb;
    float metallic = pc.matParams.x;
    float rough    = pc.matParams.y;

    vec3 color = u.ambient.rgb * albedo;

    // Sol direccional (con sombra).
    vec3 Lsun = normalize(-u.sunDir.xyz);
    float shadow = sunShadow(vWorldPos, N, Lsun);
    color += brdf(N, V, Lsun, u.sunColor.rgb, albedo, metallic, rough) * shadow;

    // Luces locales (sin sombra por ahora — diseño: sombras locales aplazadas).
    for (uint i = 0u; i < lb.count; ++i) {
        GpuLight li = lb.lights[i];
        vec3  toL  = li.posType.xyz - vWorldPos;
        float dist = length(toL);
        vec3  L    = toL / max(dist, 1e-4);
        float range = li.dirRange.w;
        float att   = clamp(1.0 - dist / max(range, 1e-4), 0.0, 1.0);
        att *= att;
        vec3 radiance = li.color.rgb * att;
        if (li.posType.w > 0.5) {
            float cd      = dot(normalize(li.dirRange.xyz), -L);
            float spotAtt = clamp((cd - li.spot.y) / max(li.spot.x - li.spot.y, 1e-4), 0.0, 1.0);
            radiance *= spotAtt;
        }
        color += brdf(N, V, L, radiance, albedo, metallic, rough);
    }

    outColor = vec4(color, 1.0);
}
