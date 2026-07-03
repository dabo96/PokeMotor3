#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Bindless material textures. Lighting is computed in lighting.frag from
// the G-Buffer outputs we write below.
layout(set = 1, binding = 0) uniform sampler2D u_textures[256];

layout(push_constant) uniform PushConstants {
    mat4  model;
    vec4  baseColorFactor;
    uint  materialIndex;          // bindless slot of baseColor texture
    uint  metallicRoughnessIndex; // bindless slot of MR texture (0 = fallback white)
    float metallicFactor;
    float roughnessFactor;
    float specularAA;             // 1 = geometric specular AA on
} pc;

layout(location = 0) in vec3 v_worldPos;
layout(location = 1) in vec3 v_worldNormal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec4 v_currClip;
layout(location = 4) in vec4 v_prevClip;

// G-Buffer attachments — formats must match createGBuffer():
//   gPosition (RGBA16F): xyz worldPos, w metallic
//   gNormal   (RGBA16F): xyz worldNormal, w roughness
//   gAlbedo   (RGBA8)  : rgb albedo, a AO
//   gEmissive (RGBA16F): rgb emissive, a sssStrength
//   gMotion   (RG16F)  : xy motion vector (FASE 11 TAA fills this)
layout(location = 0) out vec4 outPosition;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outAlbedo;
layout(location = 3) out vec4 outEmissive;
layout(location = 4) out vec2 outMotion;

void main() {
    vec3 albedo = texture(u_textures[pc.materialIndex], v_uv).rgb * pc.baseColorFactor.rgb;
    vec3 N      = normalize(v_worldNormal);

    // glTF 2.0 §5.22: metallicRoughness texture stores roughness in G, metallic
    // in B. When no texture is bound, the fallback white sampler returns (1,1,1),
    // so the factors pass through unchanged. AO is hardcoded for now (glTF
    // occlusionTexture R channel could land later as a separate sample).
    // Floor the roughness so perfectly-smooth materials still feed a usable
    // mip into the SSR/IBL chain (avoids degenerate alpha=0 GGX).
    vec4 mrSample = texture(u_textures[pc.metallicRoughnessIndex], v_uv);
    float roughness = max(mrSample.g * pc.roughnessFactor, 0.04);
    float metallic  = mrSample.b * pc.metallicFactor;
    float ao        = 1.0;

    // Geometric specular antialiasing (Tokuyoshi & Kaplanyan 2019). A surface
    // whose normal swings fast across one pixel — high curvature, or a minified
    // normal-mapped detail — aliases under sharp speculars (shimmer in motion).
    // Estimate that sub-pixel normal variance from the screen-space derivatives
    // of N and fold it into the roughness so the BRDF lobe covers it. Done here
    // in the G-Buffer pass because the lighting pass is fullscreen and its
    // derivatives would cross object silhouettes.
    if (pc.specularAA > 0.5) {
        vec3  dndu = dFdx(N);
        vec3  dndv = dFdy(N);
        float variance         = 0.5 * (dot(dndu, dndu) + dot(dndv, dndv));  // sigma^2
        float kernelRoughness2 = min(2.0 * variance, 0.18);                  // clamp kappa
        roughness = sqrt(min(roughness * roughness + kernelRoughness2, 1.0));
    }

    outPosition = vec4(v_worldPos, metallic);
    outNormal   = vec4(N,          roughness);
    outAlbedo   = vec4(albedo,     ao);
    outEmissive = vec4(0.0, 0.0, 0.0, 0.0);

    // Screen-space motion vector in UV units. Both clip positions already carry
    // the Vulkan Y-flip (CPU negates proj[1][1] for the current frame and bakes
    // the same flip into prevViewProj), so their NDC.y already points down — the
    // same direction as texture UVs. The plain ndc*0.5+0.5 maps straight to the
    // [0,1] UV space the TAA/SVGF/fog passes use to sample history. Negating Y
    // here (the old vec2(0.5,-0.5)) double-flipped it, inverting motion.y and
    // causing vertical ghosting on vertical camera pans.
    vec2 curNDC  = v_currClip.xy / v_currClip.w;
    vec2 prevNDC = v_prevClip.xy / v_prevClip.w;
    vec2 curUV   = curNDC  * 0.5 + 0.5;
    vec2 prevUV  = prevNDC * 0.5 + 0.5;
    outMotion    = curUV - prevUV;
}

