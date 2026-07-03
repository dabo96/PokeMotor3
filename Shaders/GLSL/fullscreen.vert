#version 450

// Emits a single oversized triangle that covers the whole viewport — no VBO
// required. The triangle's UVs in [0,1] are interpolated across the screen.
//   vertex 0: (-1,-1) UV (0,0)
//   vertex 1: ( 3,-1) UV (2,0)
//   vertex 2: (-1, 3) UV (0,2)
// The portion outside the [-1,1] clip range is discarded by the rasterizer.
layout(location = 0) out vec2 v_uv;

void main() {
    v_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
}
