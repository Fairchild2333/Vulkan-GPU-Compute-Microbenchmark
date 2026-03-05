#version 450

// Vertex shader for particle rendering (POINT_LIST topology).
//
// Vertex input comes from the Particle struct in the vertex buffer:
//   location 0 = position (vec4, only xy used for 2D placement)
//   location 1 = velocity (vec4, only xy used for colour mapping)
//
// gl_PointSize requires the largePoints device feature for sizes > 1.0.

layout(location = 0) in vec4 inPosition;
layout(location = 1) in vec4 inVelocity;

layout(location = 0) out vec3 outColor;

void main() {
    // Place the particle in 2D normalised device coordinates.
    gl_Position = vec4(inPosition.xy, 0.0, 1.0);
    gl_PointSize = 2.0;

    // Map speed to a colour gradient: slow = blue, fast = orange-red.
    float speed = length(inVelocity.xy);
    outColor = mix(vec3(0.1, 0.4, 1.0), vec3(1.0, 0.3, 0.1), clamp(speed * 5.0, 0.0, 1.0));
}
