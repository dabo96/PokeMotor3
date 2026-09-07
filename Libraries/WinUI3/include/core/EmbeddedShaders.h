#pragma once

namespace FluentUI {
namespace Shaders {

// --- Basic UI Shaders (Quads and Lines) ---

const char* const VertexShader = R"(
#version 450 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec4 aColor;
layout (location = 2) in vec2 aUV;

uniform mat4 uProjection;

out vec4 vColor;
void main() {
    vColor = aColor;
    gl_Position = uProjection * vec4(aPos, 0.0, 1.0);
}
)";

const char* const FragmentShader = R"(
#version 450 core
in vec4 vColor;
out vec4 FragColor;
void main() {
    FragColor = vColor;
}
)";

// --- Bitmap Text Shaders ---

const char* const TextVertexShader = R"(
#version 450 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec4 aColor;
layout (location = 2) in vec2 aUV;

uniform mat4 uProjection;

out vec2 vUV;
out vec4 vColor;

void main()
{
    vUV = aUV;
    vColor = aColor;
    gl_Position = uProjection * vec4(aPos, 0.0, 1.0);
}
)";

const char* const TextFragmentShader = R"(
#version 450 core
in vec2 vUV;
in vec4 vColor;

out vec4 FragColor;

uniform sampler2D uTexture;
uniform vec4 uTextColor;

void main()
{
    float alpha = texture(uTexture, vUV).r;
    FragColor = vec4(uTextColor.rgb, uTextColor.a * alpha);
}
)";

// --- MSDF Text Shaders ---

const char* const MSDFVertexShader = R"(
#version 450 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec4 aColor;
layout (location = 2) in vec2 aUV;

uniform mat4 uProjection;

out vec2 vUV;
out vec4 vColor;

void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = uProjection * vec4(aPos, 0.0, 1.0);
}
)";

const char* const MSDFFragmentShader = R"(
#version 450 core
in vec2 vUV;
in vec4 vColor;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform vec4 uTextColor;
uniform float pxRange;
// Brief 35-A: transfer curve (gamma + enhanced contrast). gamma<=0 or contrast=0
// degrade to the identity, so an uninitialized uniform can't blow up the render.
uniform float uTextGamma;
uniform float uTextContrast;

float median(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
}

// Coverage at a single UV, given the precomputed screen-px-range.
float msdfOpacity(vec2 uv, float screenPxRange) {
    vec3 msd = texture(uTexture, uv).rgb;
    float sd = median(msd.r, msd.g, msd.b);
    return clamp(screenPxRange * (sd - 0.5) + 0.5, 0.0, 1.0);
}

// Brief 35-A: gamma + enhanced-contrast transfer, the functional equivalent of
// DirectWrite's gamma/enhancedContrast pair. Coverage blends in sRGB space, where
// 0.5 coverage reads as ~0.73 luminance — that fills narrow counters (the ~1.3px
// gap between the two stems of "tt" reads as ON and the pair merges). The fix is
// asymmetric: light-on-dark must THIN (gamma > 1), dark-on-light must FATTEN
// (gamma < 1), because of visual irradiation. The S term pushes midtones toward
// the extremes, which sharpens the edge ramp.
float contrastCurve(float a, float gamma, float contrast) {
    if (gamma <= 0.0) return a;   // uniform never set => identity
    float x = pow(clamp(a, 0.0, 1.0), gamma);
    x = x + contrast * x * (1.0 - x) * (2.0 * x - 1.0);
    return clamp(x, 0.0, 1.0);
}

void main()
{
    // Canonical msdfgen screen-px-range: half of dot(unitRange, screenTexSize),
    // floored at 1.0 so heavy minification never sharpens below a single pixel.
    // pxRange is now the atlas' real distanceRange (plumbed per-batch), not a
    // hand-tuned constant.
    vec2 fw = fwidth(vUV);
    vec2 unitRange = vec2(pxRange) / vec2(textureSize(uTexture, 0));
    vec2 screenTexSize = vec2(1.0) / fw;
    float screenPxRange = max(0.5 * dot(unitRange, screenTexSize), 1.0);

    float opacity = msdfOpacity(vUV, screenPxRange);

    // Brief 29 Part E: harden MSDF under strong intermediate minification.
    // footprint = texels covered per screen pixel. Above ~2.0 texels/px a single
    // sample aliases fine stems (weight depends on subpixel phase); take 4 diagonal
    // taps over the pixel footprint and average the *opacity* (never the msd channels)
    // to reconstruct sub-texel coverage. Below the threshold the central sample is
    // already exact, so magnified/large text pays nothing. Threshold raised 1.5->2.0
    // when the atlas dropped 64->32px (2026-07): mild minification (>=16px text, footprint
    // <=2.0) is now crisp single-sample; the 4-tap average softened curved stems (e.g. the
    // walls of 'o') that no longer needed it. Only small text (<=15px) still supersamples.
    vec2 footprint = fw * vec2(textureSize(uTexture, 0));
    if (max(footprint.x, footprint.y) > 2.0) {
        vec2 o = fw * 0.25; // quarter-pixel diagonal offsets, in UV space
        float s = msdfOpacity(vUV + vec2( o.x,  o.y), screenPxRange);
        s      += msdfOpacity(vUV + vec2(-o.x,  o.y), screenPxRange);
        s      += msdfOpacity(vUV + vec2( o.x, -o.y), screenPxRange);
        s      += msdfOpacity(vUV + vec2(-o.x, -o.y), screenPxRange);
        opacity = s * 0.25;
    }

    // Brief 35-A: the curve REPLACES the final clamp — it is not added on top.
    opacity = contrastCurve(opacity, uTextGamma, uTextContrast);

    if (opacity < 0.01) discard;
    FragColor = vec4(uTextColor.rgb, uTextColor.a * opacity);
}
)";

// --- MSDF Text, subpixel (ClearType-style) variant — brief 35-B ---
// Same field, sampled at three horizontal subpixel offsets so the effective
// horizontal resolution triples; each channel drives one LCD stripe. REQUIRES
// dual-source blending (GL_ARB_blend_func_extended, core in 3.3):
//   glBlendFuncSeparate(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR,
//                       GL_SRC1_ALPHA, GL_ONE_MINUS_SRC1_ALPHA)
// The program is only created when the extension is present; the grayscale
// program above stays the correct path for unsupported hardware, rotated text
// and offscreen targets.
const char* const MSDFSubpixelFragmentShader = R"(
#version 450 core
in vec2 vUV;
in vec4 vColor;

// index 0 = src0 (color), index 1 = src1 (per-channel coverage / blend factor).
layout(location = 0, index = 0) out vec4 outColor;
layout(location = 0, index = 1) out vec4 outCoverage;

uniform sampler2D uTexture;
uniform vec4 uTextColor;
uniform float pxRange;
uniform float uTextGamma;
uniform float uTextContrast;
uniform float uSubpixel;   // +1 = RGB stripes, -1 = BGR
uniform float uFringe;     // 0.15–0.25

float median(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
}

float msdfOpacity(vec2 uv, float screenPxRange) {
    vec3 msd = texture(uTexture, uv).rgb;
    float sd = median(msd.r, msd.g, msd.b);
    return clamp(screenPxRange * (sd - 0.5) + 0.5, 0.0, 1.0);
}

float contrastCurve(float a, float gamma, float contrast) {
    if (gamma <= 0.0) return a;
    float x = pow(clamp(a, 0.0, 1.0), gamma);
    x = x + contrast * x * (1.0 - x) * (2.0 * x - 1.0);
    return clamp(x, 0.0, 1.0);
}

void main()
{
    vec2 fw = fwidth(vUV);
    vec2 unitRange = vec2(pxRange) / vec2(textureSize(uTexture, 0));
    vec2 screenTexSize = vec2(1.0) / fw;
    float screenPxRange = max(0.5 * dot(unitRange, screenTexSize), 1.0);

    // One subpixel = one third of a screen pixel, in UV. Sign flips for BGR panels.
    float dx = dFdx(vUV.x) * 0.3333333;
    if (uSubpixel < 0.0) dx = -dx;

    // FreeType's FIR5 filter, {8,77,86,77,8}/256, operating in SUBPIXEL space.
    // Seven samples at 1/3-px spacing cover the union of the three 5-tap windows:
    //   s[2] = R stripe (-1/3 px), s[3] = pixel center = G, s[4] = B (+1/3 px).
    // A per-pixel "mix toward the mean" is NOT a spatial filter: a 1px stem still
    // lands almost entirely on one stripe and the glyph takes a color cast. This
    // spreads each stem's energy across its neighbours instead, so the channels
    // converge while the 3x horizontal resolution survives. DC gain is exactly 1.
    vec2 footprint = fw * vec2(textureSize(uTexture, 0));
    bool supersample = max(footprint.x, footprint.y) > 2.0;
    float oy = fw.y * 0.25;

    float s[7];
    for (int i = 0; i < 7; ++i) {
        vec2 uv = vUV + vec2(dx * float(i - 3), 0.0);
        // Brief 29 Part E interplay: the taps only reconstruct the HORIZONTAL axis,
        // so under strong minification pair each with a vertical ±0.25px sample.
        s[i] = supersample
             ? 0.5 * (msdfOpacity(uv + vec2(0.0,  oy), screenPxRange) +
                      msdfOpacity(uv + vec2(0.0, -oy), screenPxRange))
             : msdfOpacity(uv, screenPxRange);
    }

    const float w0 = 8.0 / 256.0, w1 = 77.0 / 256.0, w2 = 86.0 / 256.0;
    vec3 c = vec3(
        w0*s[0] + w1*s[1] + w2*s[2] + w1*s[3] + w0*s[4],   // R, centered on s[2]
        w0*s[1] + w1*s[2] + w2*s[3] + w1*s[4] + w0*s[5],   // G, centered on s[3]
        w0*s[2] + w1*s[3] + w2*s[4] + w1*s[5] + w0*s[6]);  // B, centered on s[4]

    // Residual knob on top of the FIR5. 0 keeps the filtered stripes, 1 = grayscale.
    c = mix(c, vec3(dot(c, vec3(0.3333333))), uFringe);

    c = vec3(contrastCurve(c.r, uTextGamma, uTextContrast),
             contrastCurve(c.g, uTextGamma, uTextContrast),
             contrastCurve(c.b, uTextGamma, uTextContrast));

    float maxC = max(max(c.r, c.g), c.b);
    if (maxC < 0.01) discard;

    outColor = vec4(uTextColor.rgb, 1.0);
    // src1.a drives the destination ALPHA. Writing 1.0 there (as the brief does)
    // would make the whole glyph quad opaque in a layered/transparent window even
    // where coverage is zero; the perceptual mean keeps alpha honest.
    outCoverage = vec4(c * uTextColor.a, dot(c, vec3(0.3333333)) * uTextColor.a);
}
)";

// --- Image Shaders (Full RGBA texture sampling with vertex color tint) ---

const char* const ImageVertexShader = R"(
#version 450 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec4 aColor;
layout (location = 2) in vec2 aUV;

uniform mat4 uProjection;

out vec2 vUV;
out vec4 vColor;

void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = uProjection * vec4(aPos, 0.0, 1.0);
}
)";

const char* const ImageFragmentShader = R"(
#version 450 core
in vec2 vUV;
in vec4 vColor;
out vec4 FragColor;

uniform sampler2D uTexture;

void main() {
    vec4 texColor = texture(uTexture, vUV);
    FragColor = texColor * vColor;
}
)";

// --- SDF Rounded-Rect Shaders (instanced; fill + border) ---
// A unit quad (location 0) is instanced per widget; per-instance attributes carry
// the rounded box parameters. The fragment shader resolves the shape analytically
// (no tessellation). mode==1 (shadow) is added in brief 03; reveal in brief 04.

const char* const SDFRectVertexShader = R"(
#version 450 core
layout (location = 0) in vec2 aQuad;     // unit quad in [-1,1]
layout (location = 1) in vec2 iCenter;   // rect center (px)
layout (location = 2) in vec2 iHalf;     // half-size (px)
layout (location = 3) in vec4 iParams;   // radius, borderW(/blur), softness, mode
layout (location = 4) in vec4 iFill;     // fill rgba
layout (location = 5) in vec4 iBorder;   // border rgba (color superior del bisel)
layout (location = 6) in float iReveal;  // reveal intensity (brief 04)
layout (location = 7) in vec3 iBorder2;  // color inferior del bisel (brief 34 Parte E)

uniform mat4 uProjection;

out vec2 vLocal;
flat out vec2 vHalf;
flat out float vRadius;
flat out float vBorderW;
flat out float vSoft;
flat out float vMode;
flat out vec4 vFill;
flat out vec4 vBorder;
flat out vec2 vCenter;   // for reveal world-space reconstruction (brief 04)
flat out float vReveal;
flat out vec3 vBorder2;

void main() {
    // Expand the quad to cover what each mode needs (brief 11). iParams.y holds
    // borderWidth for fills (mode 0) and sigma for shadow/inset (mode 1/3); the
    // gaussian penumbra reaches ~3*sigma, so the shadow quad must grow by that or
    // its tail gets clipped. mode 2 (acrylic-mask) only needs the AA softness.
    float mode = iParams.w;
    float pad;
    if (mode > 0.5 && mode < 1.5)       pad = 3.0 * iParams.y + 2.0; // shadow
    else if (mode > 2.5)                pad = 3.0 * iParams.y + 2.0; // inset
    else if (mode > 1.5 && mode < 2.5)  pad = iParams.z + 2.0;       // acrylic-mask
    else                                pad = iParams.y + iParams.z + 2.0; // fill
    vec2 local = aQuad * (iHalf + vec2(pad));
    vLocal   = local;
    vHalf    = iHalf;
    vRadius  = iParams.x;
    vBorderW = iParams.y;
    vSoft    = iParams.z;
    vMode    = iParams.w;
    vFill    = iFill;
    vBorder  = iBorder;
    vCenter  = iCenter;
    vReveal  = iReveal;
    vBorder2 = iBorder2;
    gl_Position = uProjection * vec4(iCenter + local, 0.0, 1.0);
}
)";

const char* const SDFRectFragmentShader = R"(
#version 450 core
in vec2 vLocal;
flat in vec2 vHalf;
flat in float vRadius;
flat in float vBorderW;
flat in float vSoft;
flat in float vMode;
flat in vec4 vFill;
flat in vec4 vBorder;
flat in vec2 vCenter;
flat in float vReveal;
flat in vec3 vBorder2;

uniform vec3 uReveal; // cursorX, cursorY, revealRadius (px). z<=0 disables (brief 04)

out vec4 FragColor;

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;
}

// --- Analytic gaussian rounded-box shadow (brief 11, Evan Wallace technique) ---
// Approximation of the error function (Abramowitz-Stegun 7.1.27), vectorized.
vec2 erf2(vec2 x) {
    vec2 s = sign(x), a = abs(x);
    vec2 r = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a*a)) * a) * a;
    r *= r;                 // ^2
    return s - s / (r*r);   // ^4
}

// Shadow coverage of a rounded box integrated in X for one row y.
float shadowX(float x, float y, float sigma, float corner, vec2 hs) {
    float delta  = min(hs.y - corner - abs(y), 0.0);
    float curved = hs.x - corner + sqrt(max(0.0, corner*corner - delta*delta));
    vec2 integral = 0.5 + 0.5 * erf2((x + vec2(-curved, curved)) * (0.70710678 / sigma));
    return integral.y - integral.x;
}

// Total coverage [0..1] of the shadow of a box [-hs,hs] at 'p' (relative to center).
float roundedBoxShadow(vec2 hs, vec2 p, float sigma, float corner) {
    float low  = p.y - hs.y;
    float high = p.y + hs.y;
    float start = clamp(-3.0 * sigma, low, high);
    float end   = clamp( 3.0 * sigma, low, high);
    float stepv = (end - start) / 4.0;
    float y = start + stepv * 0.5;
    float value = 0.0;
    const float invSqrt2pi = 0.39894228;
    for (int i = 0; i < 4; ++i) {
        float g = exp(-(y*y) / (2.0*sigma*sigma)) * (invSqrt2pi / sigma); // gaussian(y)
        value += shadowX(p.x, p.y - y, sigma, corner, hs) * g * stepv;
        y += stepv;
    }
    return clamp(value, 0.0, 1.0);
}

void main() {
    float d = sdRoundBox(vLocal, vHalf, vRadius);

    // mode == 1: drop shadow (brief 11). vBorderW aliases sigma (penumbra, px).
    // Analytic gaussian box shadow: correct contact-hardening penumbra (sharp at
    // the edge, diffuse far away) in a single pass.
    if (vMode > 0.5 && vMode < 1.5) {
        float sigma = max(vBorderW, 0.5);
        float corner = min(vRadius, min(vHalf.x, vHalf.y));
        float cov = roundedBoxShadow(vHalf, vLocal, sigma, corner);
        float sa = vFill.a * cov;
        if (sa < 0.002) discard;
        FragColor = vec4(vFill.rgb, sa);
        return;
    }

    // mode == 3: inner / inset shadow (brief 11). Darkens INSIDE the rect, stronger
    // near the inner edges. inset = (1 - coverage) clipped to the rect interior.
    if (vMode > 2.5) {
        float sigma = max(vBorderW, 0.5);
        float corner = min(vRadius, min(vHalf.x, vHalf.y));
        float aaMask = 1.0 - smoothstep(-vSoft, vSoft, d);   // 1 inside, 0 outside
        float cov = roundedBoxShadow(vHalf, vLocal, sigma, corner);
        float inset = (1.0 - cov) * aaMask;
        float sa = vFill.a * inset;
        if (sa < 0.002) discard;
        FragColor = vec4(vFill.rgb, sa);
        return;
    }

    // Brief 34 Parte E: usar la magnitud real del gradiente (≈1 uniforme para un SDF
    // verdadero) en vez de fwidth = |dFdx|+|dFdy|, que se infla ~√2× en las diagonales
    // (esquinas redondeadas) y adelgazaba/desvanecía el borde con bisel en la curva.
    float aa = max(vSoft, length(vec2(dFdx(d), dFdy(d))));
    float fillCov = 1.0 - smoothstep(-aa, aa, d);
    vec3  rgb = vFill.rgb;
    float a   = vFill.a * fillCov;
    if (vBorderW > 0.0) {
        float inner = 1.0 - smoothstep(-aa, aa, d + vBorderW);
        float borderCov = clamp(fillCov - inner, 0.0, 1.0);
        // Brief 34 Parte E: bisel vertical. t=0 arriba, t=1 abajo (origen top-left).
        // vBorder2==(0,0,0) = pads sin usar (draws del Renderer / retrocompat) ⇒ plano.
        // También plano si vBorder2 == vBorder.rgb (el mix queda no-op).
        vec3 bottom = all(equal(vBorder2, vec3(0.0))) ? vBorder.rgb : vBorder2;
        float tGrad = clamp((vLocal.y + vHalf.y) / (2.0 * vHalf.y), 0.0, 1.0);
        vec3 bcol = mix(vBorder.rgb, bottom, tGrad);
        rgb = mix(rgb, bcol, borderCov);
        a   = max(a, vBorder.a * borderCov);
    }

    // Reveal highlight (brief 04): the edge lights up by proximity to the cursor.
    if (vReveal > 0.0 && uReveal.z > 0.0) {
        vec2 fragWorld = vCenter + vLocal;
        float dCursor = length(fragWorld - uReveal.xy);
        float prox = 1.0 - clamp(dCursor / uReveal.z, 0.0, 1.0);
        prox = prox * prox;                                   // soft curve
        float edge = (1.0 - smoothstep(-aa, aa, d)) - (1.0 - smoothstep(-aa, aa, d + 1.5));
        edge = clamp(edge, 0.0, 1.0);                          // ~1.5px edge band
        float revealA = prox * vReveal * edge;
        rgb = mix(rgb, vec3(1.0), revealA);
        a   = max(a, revealA * 0.9);
    }

    if (a < 0.002) discard;
    FragColor = vec4(rgb, a);
}
)";

// --- Backdrop blur (dual Kawase) + Acrylic composite (brief 06) ---
// A fullscreen quad ([-1,1] clip space, uv in [0,1]) feeds the two blur passes.
// The composite draws the panel quad in logical px (via uProjection) and samples
// the blurred backdrop in screen space (gl_FragCoord), masked by the rounded box.

const char* const BlurVertexShader = R"(
#version 450 core
layout (location = 0) in vec2 aPos;   // clip-space [-1,1]
layout (location = 1) in vec2 aUV;    // [0,1]
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

const char* const KawaseDownFragment = R"(
#version 450 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
uniform vec2 uHalfpixel; // 0.5 / texSize (of the SOURCE texture)
void main() {
    vec4 sum = texture(uTex, vUV) * 4.0;
    sum += texture(uTex, vUV - uHalfpixel.xy);
    sum += texture(uTex, vUV + uHalfpixel.xy);
    sum += texture(uTex, vUV + vec2(uHalfpixel.x, -uHalfpixel.y));
    sum += texture(uTex, vUV - vec2(uHalfpixel.x, -uHalfpixel.y));
    FragColor = sum / 8.0;
}
)";

const char* const KawaseUpFragment = R"(
#version 450 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
uniform vec2 uHalfpixel; // 0.5 / texSize (of the SOURCE texture)
void main() {
    vec4 sum = texture(uTex, vUV + vec2(-uHalfpixel.x * 2.0, 0.0));
    sum += texture(uTex, vUV + vec2(-uHalfpixel.x, uHalfpixel.y)) * 2.0;
    sum += texture(uTex, vUV + vec2(0.0, uHalfpixel.y * 2.0));
    sum += texture(uTex, vUV + vec2(uHalfpixel.x, uHalfpixel.y)) * 2.0;
    sum += texture(uTex, vUV + vec2(uHalfpixel.x * 2.0, 0.0));
    sum += texture(uTex, vUV + vec2(uHalfpixel.x, -uHalfpixel.y)) * 2.0;
    sum += texture(uTex, vUV + vec2(0.0, -uHalfpixel.y * 2.0));
    sum += texture(uTex, vUV + vec2(-uHalfpixel.x, -uHalfpixel.y)) * 2.0;
    FragColor = sum / 12.0;
}
)";

const char* const AcrylicCompositeVertexShader = R"(
#version 450 core
// Panel quad generated from gl_VertexID (triangle strip, 4 verts) — no VBO.
uniform mat4 uProjection;
uniform vec2 uCenter;   // rect center (logical px)
uniform vec2 uHalf;     // rect half-size (logical px)
uniform float uSoft;    // AA width (px)
out vec2 vLocal;
void main() {
    vec2 c = vec2((gl_VertexID == 1 || gl_VertexID == 3) ? 1.0 : -1.0,
                  (gl_VertexID >= 2) ? 1.0 : -1.0);
    vec2 ext = uHalf + vec2(uSoft + 2.0);
    vec2 local = c * ext;
    vLocal = local;
    gl_Position = uProjection * vec4(uCenter + local, 0.0, 1.0);
}
)";

const char* const AcrylicCompositeFragmentShader = R"(
#version 450 core
in vec2 vLocal;
out vec4 FragColor;

uniform sampler2D uBlur;     // blurred backdrop (full screen, framebuffer-oriented)
uniform sampler2D uNoiseTex; // 64x64 blue noise (R channel)
uniform vec2  uScreenSize;   // framebuffer px
uniform vec2  uHalf;         // rect half-size (logical px)
uniform float uRadius;       // corner radius (logical px)
uniform float uSoft;         // AA width (px)
uniform vec3  uTint;
uniform float uTintOpacity;
uniform float uLuminosityOpacity;
uniform float uNoiseAmount;

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;
}

void main() {
    // Screen-space UV. gl_FragCoord and the blurred texture share the GL
    // bottom-left framebuffer orientation, so no Y flip is needed.
    vec2 screenUV = gl_FragCoord.xy / uScreenSize;
    vec3 backdrop = texture(uBlur, screenUV).rgb;
    vec3 lum = vec3(dot(backdrop, vec3(0.299, 0.587, 0.114)));
    vec3 col = mix(backdrop, lum, uLuminosityOpacity);
    col = mix(col, uTint, uTintOpacity);
    float n = texture(uNoiseTex, gl_FragCoord.xy / 64.0).r - 0.5;
    col += n * uNoiseAmount;

    float d = sdRoundBox(vLocal, uHalf, uRadius);
    float aa = max(uSoft, fwidth(d));
    float mask = 1.0 - smoothstep(-aa, aa, d);
    if (mask < 0.002) discard;
    FragColor = vec4(col, mask);
}
)";

} // namespace Shaders
} // namespace FluentUI
