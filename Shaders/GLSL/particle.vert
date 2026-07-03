#version 460

// Billboard quad per particle instance. No VBO — gl_VertexIndex picks one
// of 4 corner positions, gl_InstanceIndex indexes the particle SSBO.
// Per-frame camera UBO supplies camera-right / camera-up so the quads
// always face the camera (spherical billboard).

struct Particle {
    vec4 position;   // xyz = pos, w = size
    vec4 velocity;
    vec4 color;
    vec4 params;
    vec4 extra;
};

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

layout(set = 1, binding = 0, std430) readonly buffer ParticleBuffer {
    Particle particles[];
};

// Shared with particle.frag. Vertex uses motionBlur/stretchScale; fragment uses
// softEnabled/fadeDist. Declared identically in both stages so the offsets match.
layout(push_constant) uniform PC {
    float softEnabled;   // fragment
    float fadeDist;      // fragment
    float motionBlur;    // vertex — 1 = stretch billboard along velocity
    float stretchScale;  // vertex — world units of stretch per unit speed
} pc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec3 v_worldPos;  // for soft-particle depth fade

const vec2 QUAD[4] = vec2[4](
    vec2(-0.5, -0.5),
    vec2( 0.5, -0.5),
    vec2(-0.5,  0.5),
    vec2( 0.5,  0.5)
);

void main() {
    Particle p = particles[gl_InstanceIndex];
    if (p.params.w < 0.5) {
        gl_Position = vec4(0.0);  // degenerate
        v_uv = vec2(0.0);
        v_color = vec4(0.0);
        return;
    }

    // Reconstruct camera right/up from the view matrix (view row vectors).
    vec3 camRight = vec3(u.view[0][0], u.view[1][0], u.view[2][0]);
    vec3 camUp    = vec3(u.view[0][1], u.view[1][1], u.view[2][1]);

    vec2  corner = QUAD[gl_VertexIndex];
    float size   = p.position.w;

    // Default spherical billboard.
    vec2 axisX = vec2(1.0, 0.0);  // along camRight
    vec2 axisY = vec2(0.0, 1.0);  // along camUp
    float stretch = size;

    // Motion blur: stretch the quad along the velocity direction projected into
    // the camera-facing plane (camRight, camUp). corner.y runs along velocity,
    // corner.x stays perpendicular at the base size — the texture stretches with
    // the quad, reading as a speed streak. Skip when velocity points at/away from
    // the camera (no usable screen direction) or the particle is near-still.
    vec3  vel   = p.velocity.xyz;
    float speed = length(vel);
    if (pc.motionBlur > 0.5 && speed > 1e-4) {
        vec3 velDir   = vel / speed;
        vec2 velPlane = vec2(dot(velDir, camRight), dot(velDir, camUp));
        float vlen    = length(velPlane);
        if (vlen > 1e-4) {
            axisY   = velPlane / vlen;
            axisX   = vec2(-axisY.y, axisY.x);
            stretch = size + min(speed * pc.stretchScale, size * 4.0);
        }
    }

    vec2 local = axisX * (corner.x * size) + axisY * (corner.y * stretch);
    vec3 world = p.position.xyz + camRight * local.x + camUp * local.y;

    gl_Position = u.proj * u.view * vec4(world, 1.0);
    v_uv       = corner + 0.5;
    v_color    = p.color;
    v_worldPos = world;
}
