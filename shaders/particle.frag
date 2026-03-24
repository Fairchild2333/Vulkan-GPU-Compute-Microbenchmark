#version 450

// Fragment shader for particle rendering.
// Receives the interpolated colour from the vertex stage and writes it to
// the colour attachment with full opacity.

layout(location = 0) in vec3 inColor;
layout(location = 0) out vec4 outFragColor;

void main() {
    outFragColor = vec4(inColor, 1.0);
}
