#version 450

layout(location = 0) out vec3 outColor;

void main() {
    // Full-screen triangle generated from vertex ID.
    vec2 positions[3] = vec2[](
        vec2(-1.0, -1.0),
        vec2(3.0, -1.0),
        vec2(-1.0, 3.0)
    );

    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    outColor = vec3(0.2, 0.8, 1.0);
}
