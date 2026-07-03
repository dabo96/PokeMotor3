#version 450

// Sprite 2D: textura del atlas × tinte por vértice. El pipeline usa blend PREMULTIPLICADO
// (src = ONE). Las texturas Smooth ya vienen con rgb premultiplicado por su alpha al
// cargarse; aquí solo falta aplicar el alpha del TINTE (opacidad por vértice) también al
// rgb, para que un sprite con opacidad < 1 se atenúe sin volverse aditivo (sin halo).
// Textura blanca + color = tile plano. uColor.rgb es tinte; vColor.a = opacidad.
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;

layout(set = 0, binding = 0) uniform sampler2D uTex;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 tex = texture(uTex, vUV);
    // rgb: tex (ya premult por su alpha) × tinte rgb × opacidad del tinte.
    // a:   alpha de la textura × opacidad del tinte.
    outColor = vec4(tex.rgb * vColor.rgb * vColor.a, tex.a * vColor.a);
}
