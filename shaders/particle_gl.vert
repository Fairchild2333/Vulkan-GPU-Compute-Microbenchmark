#version 430

layout(location = 0) in vec4 inPosition;
layout(location = 1) in vec4 inVelocity;

out vec3 fragColor;

void main() {
    gl_Position = vec4(inPosition.xy, 0.0, 1.0);
    gl_PointSize = 2.0;

    float speed = length(inVelocity.xy);
    fragColor = mix(vec3(0.1, 0.4, 1.0), vec3(1.0, 0.3, 0.1), clamp(speed * 5.0, 0.0, 1.0));
}
