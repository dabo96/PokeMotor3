#version 450

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec4 cameraPos;
    vec4 lightDirection;
    vec4 lightColor;
    vec4 ambient;
    mat4 lightSpaceMatrix[3];
    vec4 cascadeSplits;  // .xyz = view-distance thresholds; .w = PCSS light size (0 = off)
    // Unused here, but declared so this block matches the CPU CameraUBO and
    // geometry.vert byte-for-byte — keeps the std140 layout safe if fields are
    // ever reordered or new readers are added.
    mat4 prevViewProj;
    vec4 jitter;
} u;

// G-Buffer samplers (set=2). Order matches createGBufferDescriptors().
layout(set = 2, binding = 0) uniform sampler2D u_gPosition;
layout(set = 2, binding = 1) uniform sampler2D u_gNormal;
layout(set = 2, binding = 2) uniform sampler2D u_gAlbedo;
layout(set = 2, binding = 3) uniform sampler2D u_gEmissive;
layout(set = 2, binding = 4) uniform sampler2D u_gMotion;
// Hardware-PCF shadow array — sampler was created with compareEnable + LESS,
// so texture() returns a [0,1] filtered visibility value directly.
layout(set = 2, binding = 5) uniform sampler2DArrayShadow u_shadow;
// Diffuse irradiance cubemap stored as a 6-layer 2D array (no VIEW_TYPE_CUBE).
layout(set = 2, binding = 6) uniform sampler2DArray u_irradiance;
// Specular prefiltered cubemap (GGX, kPrefilteredMips mip levels).
layout(set = 2, binding = 7) uniform sampler2DArray u_prefiltered;
// Split-sum BRDF LUT (R = F0 scale, G = F0 bias), indexed by (NdotV, roughness).
layout(set = 2, binding = 8) uniform sampler2D u_brdfLut;
// Raw SSR result — RGB is reflected color pre-multiplied by Fresnel at the
// source pixel, A is confidence. Lit areas with confidence>0 favour SSR over
// the cubemap fallback. Will be denoised by SVGF in the next iteration.
layout(set = 2, binding = 9) uniform sampler2D u_ssr;

// Point/spot lights (set 2, binding 10). lightHeader.x = active light count.
struct GpuLight {
    vec4 posRange;        // xyz position, w range
    vec4 colorIntensity;  // rgb color, w intensity
    vec4 dirType;         // xyz spot direction, w type (0 = point, 1 = spot)
    vec4 spotCos;         // x = cos(inner), y = cos(outer)
};
layout(set = 2, binding = 10, std430) readonly buffer LightBuffer {
    uvec4    lightHeader;
    GpuLight lights[];
};

// Raw CSM depth (non-comparison sampler) — PCSS blocker search reads actual
// depths from here, while u_shadow (binding 5) stays for hardware-PCF compares.
layout(set = 2, binding = 11) uniform sampler2DArray u_shadowDepth;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 outColor;

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (vec3(1.0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Fresnel with roughness (Lagarde/Frostbite). Rough microsurfaces don't produce
// the sharp grazing-angle reflection spike that a mirror does, so the term tops
// out at (1 - roughness) instead of 1.0. Without this, mate dielectric surfaces
// pick up a strong sky-tinted rim at grazing angles — looks like a fake reflection.
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    vec3 Fmax = max(vec3(1.0 - roughness), F0);
    return F0 + (Fmax - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// World-space direction → (u, v, layer) for sampling a sampler2DArray that
// holds an environment cubemap. Mirrors the convention in env_procedural.comp.
vec3 dirToFaceUV(vec3 dir) {
    vec3  absD = abs(dir);
    uint  face;
    vec2  uv;
    float ma;
    if (absD.x >= absD.y && absD.x >= absD.z) {
        face = (dir.x > 0.0) ? 0u : 1u;
        uv   = (dir.x > 0.0) ? vec2(-dir.z, -dir.y) : vec2( dir.z, -dir.y);
        ma   = absD.x;
    } else if (absD.y >= absD.z) {
        face = (dir.y > 0.0) ? 2u : 3u;
        uv   = (dir.y > 0.0) ? vec2( dir.x,  dir.z) : vec2( dir.x, -dir.z);
        ma   = absD.y;
    } else {
        face = (dir.z > 0.0) ? 4u : 5u;
        uv   = (dir.z > 0.0) ? vec2( dir.x, -dir.y) : vec2(-dir.x, -dir.y);
        ma   = absD.z;
    }
    uv = uv / ma * 0.5 + 0.5;
    return vec3(uv, float(face));
}

// Selects the cascade whose far-distance threshold first exceeds viewDist.
// Returns kCascadeCount when the fragment falls outside every cascade.
int pickCascade(float viewDist) {
    if (viewDist < u.cascadeSplits.x) return 0;
    if (viewDist < u.cascadeSplits.y) return 1;
    if (viewDist < u.cascadeSplits.z) return 2;
    return 3;
}

// 16-tap Poisson disk for the PCSS blocker search and the variable-radius PCF.
// Pre-rotating per-pixel would cut banding further, but a fixed disk already
// looks clean at 2048² and avoids the noise a random rotation introduces.
const vec2 POISSON_DISK[16] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2( 0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870), vec2( 0.34495938,  0.29387760),
    vec2(-0.91588581,  0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543,  0.27676845), vec2( 0.97484398,  0.75648379),
    vec2( 0.44323325, -0.97511554), vec2( 0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023), vec2( 0.79197514,  0.19090188),
    vec2(-0.24188840,  0.99706507), vec2(-0.81409955,  0.91437590),
    vec2( 0.19984126,  0.78641367), vec2( 0.14383161, -0.14100790)
);

// Average depth of the texels that occlude this fragment (depth < receiver),
// over a search disk. Returns false when nothing blocks the light.
bool pcssBlockerSearch(int cascade, vec2 uv, float refZ, float searchRadius,
                       out float avgBlocker) {
    float sum   = 0.0;
    int   count = 0;
    for (int i = 0; i < 16; ++i) {
        vec2  o = POISSON_DISK[i] * searchRadius;
        float d = texture(u_shadowDepth, vec3(uv + o, float(cascade))).r;
        if (d < refZ) { sum += d; ++count; }
    }
    if (count == 0) { avgBlocker = 0.0; return false; }
    avgBlocker = sum / float(count);
    return true;
}

// Variable-radius PCF through the hardware comparison sampler.
float pcssPCF(int cascade, vec2 uv, float refZ, float radius) {
    float vis = 0.0;
    for (int i = 0; i < 16; ++i) {
        vec2 o = POISSON_DISK[i] * radius;
        vis += texture(u_shadow, vec4(uv + o, float(cascade), refZ));
    }
    return vis / 16.0;
}

float sampleShadow(int cascade, vec3 worldPos) {
    if (cascade >= 3) return 1.0;
    vec4 lc = u.lightSpaceMatrix[cascade] * vec4(worldPos, 1.0);
    vec3 ndc = lc.xyz / lc.w;
    // glTF/Vulkan light proj already includes the Y flip; map NDC.xy to [0,1].
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;

    // Vulkan depth range [0,1] — no remap needed.
    float bias  = 0.002;
    float refZ  = clamp(ndc.z - bias, 0.0, 1.0);
    vec2  texel = vec2(1.0 / 2048.0);

    float lightSize = u.cascadeSplits.w;  // PCSS light size in shadow-UV units
    if (lightSize > 0.0) {
        // ---- PCSS: blocker search → penumbra estimate → variable PCF ----
        float avgBlocker;
        if (!pcssBlockerSearch(cascade, uv, refZ, lightSize, avgBlocker))
            return 1.0;  // unoccluded → fully lit, skip the PCF entirely
        // Penumbra ratio (PCSS): bigger receiver-to-blocker gap → softer edge.
        // Normalise by the RECEIVER depth (refZ), not the blocker depth. The CSM
        // uses an orthographic projection so these depths are linear; the blocker
        // sits in front of the receiver, hence avgBlocker < refZ and the ratio
        // stays in [0,1). Dividing by avgBlocker (the old code) blew up whenever a
        // blocker landed near the cascade near plane (avgBlocker → 0), producing
        // intermittent giant penumbras. Clamp keeps contact crisp and bounded.
        float penumbra = (refZ - avgBlocker) / max(refZ, 1e-4);
        float radius   = clamp(penumbra * lightSize, texel.x, lightSize);
        return pcssPCF(cascade, uv, refZ, radius);
    }

    // ---- Fixed 3×3 PCF on top of the hardware 2×2 (PCSS disabled) ----
    float visibility = 0.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 o = vec2(dx, dy) * texel;
            visibility += texture(u_shadow, vec4(uv + o, float(cascade), refZ));
        }
    }
    return visibility / 9.0;
}

void main() {
    vec4 posSample = texture(u_gPosition, v_uv);
    vec4 norSample = texture(u_gNormal,   v_uv);
    vec4 albSample = texture(u_gAlbedo,   v_uv);
    vec4 emiSample = texture(u_gEmissive, v_uv);

    if (dot(norSample.xyz, norSample.xyz) < 0.5) {
        outColor = vec4(0.04, 0.05, 0.08, 1.0);
        return;
    }

    vec3  worldPos = posSample.xyz;
    vec3  N        = normalize(norSample.xyz);
    vec3  albedo   = albSample.rgb;
    float ao       = albSample.a;
    vec3  emissive = emiSample.rgb;
    float metallic = posSample.w;  // gPosition.w holds metallic (geometry.frag)

    vec3 L = normalize(-u.lightDirection.xyz);
    vec3 V = normalize(u.cameraPos.xyz - worldPos);
    vec3 H = normalize(L + V);

    float viewDist  = length(u.cameraPos.xyz - worldPos);
    int   cascade   = pickCascade(viewDist);
    // Normal-offset bias: nudge the sample point along the surface normal to kill
    // shadow acne without the peter-panning of a big constant depth bias.
    vec3  shadowPos = worldPos + N * 0.02;
    float shadowVis = sampleShadow(cascade, shadowPos);
    // Cascade cross-fade: blend into the next cascade near the split boundary so
    // the resolution change isn't a visible hard line.
    if (cascade < 2) {
        float split     = (cascade == 0) ? u.cascadeSplits.x : u.cascadeSplits.y;
        float fadeStart = split * 0.85;
        if (viewDist > fadeStart) {
            float t       = (viewDist - fadeStart) / max(split - fadeStart, 1e-4);
            float nextVis = sampleShadow(cascade + 1, shadowPos);
            shadowVis     = mix(shadowVis, nextVis, clamp(t, 0.0, 1.0));
        }
    }


    // Material terms (roughness/metallic from the G-Buffer).
    float roughness = norSample.a;                        // gNormal.w holds roughness
    vec3  F0        = mix(vec3(0.04), albedo, metallic);  // metals tint reflection

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    // Diffuse (Lambert) — metals have no diffuse component.
    vec3 diffuse = albedo * (1.0 - metallic) * NdotL * u.lightColor.xyz;

    // Specular: Cook-Torrance GGX, energy-conserving and consistent with the IBL
    // BRDF (replaces the old Blinn-Phong pow(NdotH,64)).
    float a    = roughness * roughness;
    float a2   = a * a;
    float dDen = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    float D    = a2 / max(3.14159265 * dDen * dDen, 1e-7);
    float kDir = (roughness + 1.0) * (roughness + 1.0) / 8.0;  // Smith k (direct)
    float gV   = NdotV / (NdotV * (1.0 - kDir) + kDir);
    float gL   = NdotL / (NdotL * (1.0 - kDir) + kDir);
    float G    = gV * gL;
    vec3  Fd   = fresnelSchlick(VdotH, F0);
    vec3 specular = (D * G * Fd) / max(4.0 * NdotL * NdotV, 1e-4)
                  * NdotL * u.lightColor.xyz;

    // ---- IBL ambient -------------------------------------------------------
    // Diffuse irradiance from the precomputed cubemap; the legacy u.ambient
    // flat color is kept as a floor so totally unlit pockets stay above black.
    vec3 envIrr  = texture(u_irradiance, dirToFaceUV(N)).rgb;
    vec3 R       = reflect(-V, N);
    vec3 F_ibl   = fresnelSchlickRoughness(NdotV, F0, roughness);

    float mipLevel = roughness * float(8 - 1);  // kPrefilteredMips - 1
    vec3 prefiltered = textureLod(u_prefiltered, dirToFaceUV(R), mipLevel).rgb;
    vec2 brdf = texture(u_brdfLut, vec2(NdotV, roughness)).rg;
    // Single-scatter split-sum term.
    vec3  FssEss = F_ibl * brdf.x + brdf.y;
    // Multiple-scattering energy compensation (Fernández-Agüera 2019): plain
    // single-scatter GGX loses energy at high roughness, darkening rough metals.
    // Add the missing inter-reflection energy back.
    float Ess  = brdf.x + brdf.y;
    float Ems  = 1.0 - Ess;
    vec3  Favg = F0 + (vec3(1.0) - F0) / 21.0;
    vec3  Fms  = FssEss * Favg / (vec3(1.0) - Ems * Favg);
    vec3 specularIBL = prefiltered * (FssEss + Fms * Ems);

    // ---- SSR mix (SVGF-denoised) -------------------------------------------
    // The trace shader already pre-multiplied by Fresnel at the source pixel,
    // so SSR.rgb is the final specular contribution for that hit, denoised
    // by the SVGF temporal + 3× A-Trous spatial pipeline. Where SSR hit
    // something (confidence>0), substitute it for the cubemap specular;
    // otherwise fall back to the prefiltered IBL.
    vec4 ssrSample = texture(u_ssr, v_uv);
    // Only trust SSR on surfaces smooth enough to have actually traced rays — the
    // ray-sort skips roughness >= 0.70, but the SVGF denoiser / half-res upsample
    // bleed neighbours' reflections (e.g. the carts' chrome) onto rough surfaces
    // like the ground, which shows up as streaks at grazing angles. Match the
    // sort cutoff here so the ground falls back to the smooth cubemap IBL instead.
    float ssrConfidence = (roughness < 0.70) ? ssrSample.a : 0.0;
    vec3 specularReflection = mix(specularIBL, ssrSample.rgb, ssrConfidence);

    vec3 kS = F_ibl;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);  // metals have no diffuse
    // Diffuse IBL is tinted by albedo; specular IBL/SSR already carries its F0
    // (albedo for metals, via the split-sum F*brdf.x + brdf.y term), so it must
    // NOT be multiplied by albedo again — doing so squared dark albedos to black
    // on metallic surfaces.
    vec3 ambient = (kD * envIrr * albedo + specularReflection) * ao
                 + u.ambient.xyz * albedo;

    // ---- Punctual lights — Cook-Torrance GGX. dirType.w selects the type:
    // 0 = point, 1 = spot, 2 = area sphere. No per-light shadows yet.
    vec3 punctual = vec3(0.0);
    uint lightCount = lightHeader.x;
    for (uint li = 0u; li < lightCount; ++li) {
        GpuLight lt = lights[li];
        float ltype = lt.dirType.w;
        vec3  toL     = lt.posRange.xyz - worldPos;
        float dCenter = length(toL);
        if (dCenter > lt.posRange.w) continue;
        // Windowed inverse-square-ish range falloff (shared by every type).
        float att = clamp(1.0 - (dCenter / lt.posRange.w), 0.0, 1.0);
        att *= att;

        if (ltype > 1.5) {
            // ---- Area sphere light (Karis 2013 representative point) ----
            // Specular: stand the light in for the point on the sphere closest to
            // the reflection ray, and widen the GGX lobe by the light's solid
            // angle (alphaPrime) with an energy renormalisation so the brightness
            // is conserved as the highlight spreads. Diffuse uses the sphere
            // centre — the area mostly matters for the specular highlight.
            float radius = max(lt.spotCos.x, 1e-3);
            vec3  centerToRay = dot(toL, R) * R - toL;
            vec3  closest     = toL + centerToRay
                              * clamp(radius / max(length(centerToRay), 1e-4), 0.0, 1.0);
            vec3  Lspec  = normalize(closest);
            float nlS    = max(dot(N, Lspec), 0.0);
            float alphaP = clamp(a + radius / (2.0 * dCenter), 0.0, 1.0);
            float a2p    = alphaP * alphaP;
            float sphereNorm = (a * a) / max(a2p, 1e-7);   // (alpha / alphaPrime)^2
            vec3  Hs  = normalize(Lspec + V);
            float nhS = max(dot(N, Hs), 0.0);
            float vhS = max(dot(V, Hs), 0.0);
            float dS  = (nhS * nhS) * (a2p - 1.0) + 1.0;
            float DS  = a2p / max(3.14159265 * dS * dS, 1e-7);
            float gvS = NdotV / (NdotV * (1.0 - kDir) + kDir);
            float glS = nlS   / (nlS   * (1.0 - kDir) + kDir);
            vec3  FS  = fresnelSchlick(vhS, F0);
            vec3  specA = (DS * (gvS * glS) * FS) / max(4.0 * nlS * NdotV, 1e-4)
                        * sphereNorm * nlS;
            vec3  Lc   = toL / max(dCenter, 1e-4);
            float nlC  = max(dot(N, Lc), 0.0);
            vec3  diffA = albedo * (1.0 - metallic) * (vec3(1.0) - FS) * 0.31830989 * nlC;
            punctual += (diffA + specA) * lt.colorIntensity.rgb * lt.colorIntensity.w * att;
            continue;
        }

        // ---- Point / spot ----
        vec3  Ll = toL / max(dCenter, 1e-4);
        float nl = max(dot(N, Ll), 0.0);
        if (nl <= 0.0) continue;
        if (ltype > 0.5) {  // spot cone
            float cd = dot(-Ll, normalize(lt.dirType.xyz));
            att *= clamp((cd - lt.spotCos.y) / max(lt.spotCos.x - lt.spotCos.y, 1e-4), 0.0, 1.0);
        }
        if (att <= 0.0) continue;
        vec3  radiance = lt.colorIntensity.rgb * lt.colorIntensity.w * att;
        vec3  Hl  = normalize(Ll + V);
        float nh2 = max(dot(N, Hl), 0.0);
        float vh2 = max(dot(V, Hl), 0.0);
        float dl  = (nh2 * nh2) * (a2 - 1.0) + 1.0;
        float Dl  = a2 / max(3.14159265 * dl * dl, 1e-7);
        float gv  = NdotV / (NdotV * (1.0 - kDir) + kDir);
        float gl  = nl    / (nl    * (1.0 - kDir) + kDir);
        vec3  Fl  = fresnelSchlick(vh2, F0);
        vec3  specL = (Dl * (gv * gl) * Fl) / max(4.0 * nl * NdotV, 1e-4);
        vec3  diffL = albedo * (1.0 - metallic) * (vec3(1.0) - Fl) * 0.31830989; // 1/PI
        punctual += (diffL + specL) * radiance * nl;
    }

    vec3 lit = ambient
             + (diffuse + specular) * shadowVis
             + punctual
             + emissive;
    outColor = vec4(lit, 1.0);
}
