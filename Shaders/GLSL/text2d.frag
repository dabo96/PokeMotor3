#version 450
// text2d.frag — render MSDF: la mediana de los 3 canales es la distancia firmada al
// borde del glifo; screenPxRange (vía derivadas) da AA independiente del tamaño.
// Técnica de Viktor Chlumský (msdfgen). El color sale del vértice (tinte por glifo).
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uAtlas;

layout(push_constant) uniform Push {
    mat4  viewProj;
    float pxRange;    // distanceRange del atlas, en píxeles
} pc;

float median(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
}

void main() {
    vec3 msd = texture(uAtlas, vUV).rgb;
    float sd = median(msd.r, msd.g, msd.b);

    // Rango de la distancia, en píxeles de PANTALLA (robusto a cualquier escala).
    vec2 unitRange     = vec2(pc.pxRange) / vec2(textureSize(uAtlas, 0));
    vec2 screenTexSize = vec2(1.0) / fwidth(vUV);
    float screenPxRange = max(0.5 * dot(unitRange, screenTexSize), 1.0);

    float screenPxDist = screenPxRange * (sd - 0.5);
    float opacity = clamp(screenPxDist + 0.5, 0.0, 1.0);
    if (opacity <= 0.0) discard;

    outColor = vec4(vColor.rgb, vColor.a * opacity);
}
