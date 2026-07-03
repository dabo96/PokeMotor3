#version 450

// Temporal Antialiasing — single-pass resolve.
// Inputs:
//   - sceneColor (current frame, jittered)
//   - history    (previous frame's TAA output, reprojected this frame)
//   - motion     (gMotion: previous → current UV delta written by geometry.frag)
// Outputs (MRT, both write the same value):
//   - location 0 → swapchain   (LDR-friendly)
//   - location 1 → history[N]  (stays HDR, fed back next frame)

layout(set = 0, binding = 0) uniform sampler2D u_sceneColor;
layout(set = 0, binding = 1) uniform sampler2D u_history;
layout(set = 0, binding = 2) uniform sampler2D u_motion;

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 outSwapchain;
layout(location = 1) out vec4 outHistory;

// RGB → YCoCg keeps the variance neighborhood inside a tight luminance/chroma
// box that catches ghosting better than raw RGB clipping.
vec3 rgbToYCoCg(vec3 c) {
    return vec3(
        0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
        0.5  * c.r              - 0.5  * c.b,
       -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
vec3 yCoCgToRgb(vec3 y) {
    return vec3(
        y.x + y.y - y.z,
        y.x       + y.z,
        y.x - y.y - y.z);
}

void main() {
    vec3 current = texture(u_sceneColor, v_uv).rgb;

    // Reproject: history sample was rendered at uv - motion last frame.
    vec2 motion  = texture(u_motion, v_uv).rg;
    vec2 prevUV  = v_uv - motion;

    // If reprojection lands outside the frame, fall back to current with no
    // history blending — avoids ghosting at the screen border.
    if (prevUV.x < 0.0 || prevUV.x > 1.0 || prevUV.y < 0.0 || prevUV.y > 1.0) {
        outSwapchain = vec4(current, 1.0);
        outHistory   = vec4(current, 1.0);
        return;
    }
    vec3 history = texture(u_history, prevUV).rgb;

    // Variance-clip the history into the 3x3 neighborhood of the current pixel
    // in YCoCg space. This is the textbook fix for ghosting at edges.
    vec3 cur = rgbToYCoCg(current);
    vec3 m1 = vec3(0.0), m2 = vec3(0.0);
    const int RADIUS = 1;
    vec2 texel = 1.0 / vec2(textureSize(u_sceneColor, 0));
    for (int y = -RADIUS; y <= RADIUS; ++y) {
        for (int x = -RADIUS; x <= RADIUS; ++x) {
            vec3 s = rgbToYCoCg(texture(u_sceneColor, v_uv + vec2(x, y) * texel).rgb);
            m1 += s;
            m2 += s * s;
        }
    }
    float N = float((2 * RADIUS + 1) * (2 * RADIUS + 1));
    vec3 mean = m1 / N;
    vec3 sd   = sqrt(max(m2 / N - mean * mean, 0.0));
    // Per-channel clip width: tighter on luma (Y) to suppress specular/highlight
    // ghosting, looser on chroma (Co/Cg) so colors stay stable (less flicker).
    vec3 gamma = vec3(0.85, 1.25, 1.25);
    vec3 minC = mean - sd * gamma;
    vec3 maxC = mean + sd * gamma;
    vec3 histY = clamp(rgbToYCoCg(history), minC, maxC);
    history = yCoCgToRgb(histY);

    // Motion-adaptive blend: keep lots of history when still (strong AA), but
    // lean toward the current frame under motion to cut blur/ghosting.
    vec2  res        = vec2(textureSize(u_sceneColor, 0));
    float motionPx   = length(motion) * max(res.x, res.y);
    float historyAmt = mix(0.9, 0.6, clamp(motionPx * 0.1, 0.0, 1.0));
    vec3 resolved = mix(current, history, historyAmt);

    outSwapchain = vec4(resolved, 1.0);
    outHistory   = vec4(resolved, 1.0);
}
