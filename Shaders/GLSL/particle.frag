#version 460

// Soft circular particle. Alpha falls off radially so quad edges aren't
// visible. Premultiplied alpha output expected by the host blend state.
// Soft-particle depth fade: where the billboard intersects opaque geometry,
// fade the alpha out so there's no hard cut seam against the scene.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec3 v_worldPos;

layout(location = 0) out vec4 outColor;

// set 0 — per-frame camera UBO (only cameraPos is used here).
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec4 cameraPos;
    vec4 lightDirection;
    vec4 lightColor;
    vec4 ambient;
    mat4 lightSpaceMatrix[3];
    vec4 cascadeSplits;
    mat4 prevViewProj;
    vec4 jitter;
} u;

// set 2 — reused G-Buffer descriptor; only gPosition (binding 0) is sampled.
layout(set = 2, binding = 0) uniform sampler2D u_gPosition;

layout(push_constant) uniform PC {
    float softEnabled;   // 0 = off, 1 = on
    float fadeDist;      // world-space distance over which the particle fades
    float motionBlur;    // unused in frag; declared so the block matches the vert
    float stretchScale;  // unused in frag; declared so the block matches the vert
} pc;

void main() {
    float dist  = length(v_uv - vec2(0.5));
    float alpha = 1.0 - smoothstep(0.3, 0.5, dist);

    if (pc.softEnabled > 0.5) {
        // World position of the opaque surface behind this fragment.
        vec2 screenUV = gl_FragCoord.xy / vec2(textureSize(u_gPosition, 0));
        vec3 scenePos = texture(u_gPosition, screenUV).xyz;
        // gPosition is ~0 where there is no geometry (sky) — skip the fade there
        // so particles against the sky keep full alpha.
        if (dot(scenePos, scenePos) > 1e-6) {
            float sceneDepth    = length(scenePos     - u.cameraPos.xyz);
            float particleDepth = length(v_worldPos   - u.cameraPos.xyz);
            // Fade as the particle approaches the surface in front of it.
            float diff = sceneDepth - particleDepth;
            alpha *= clamp(diff / max(pc.fadeDist, 1e-3), 0.0, 1.0);
        }
    }

    outColor = vec4(v_color.rgb, v_color.a * alpha);
}
