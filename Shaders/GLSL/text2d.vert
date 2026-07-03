#version 450
// text2d.vert — glifos MSDF en espacio de pantalla. Mismo Vertex2D que sprite2d.
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

layout(push_constant) uniform Push {
    mat4  viewProj;   // ortográfica de pantalla
    float pxRange;    // distanceRange del atlas (lo usa el fragment)
} pc;

void main() {
    gl_Position = pc.viewProj * vec4(inPos, 0.0, 1.0);
    vUV    = inUV;
    vColor = inColor;
}
