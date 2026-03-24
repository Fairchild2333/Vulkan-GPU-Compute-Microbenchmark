#include <metal_stdlib>
using namespace metal;

struct Particle {
    float4 position;
    float4 velocity;
};

struct ComputeParams {
    float deltaTime;
    float bounds;
};

kernel void computeMain(device Particle* particles [[buffer(0)]],
                        constant ComputeParams& params [[buffer(1)]],
                        uint id [[thread_position_in_grid]]) {
    particles[id].position.xyz += particles[id].velocity.xyz * params.deltaTime;

    if (particles[id].position.x > params.bounds) {
        particles[id].position.x = -params.bounds;
    }
}

struct VertexOut {
    float4 position [[position]];
    float3 color;
    float  pointSize [[point_size]];
};

vertex VertexOut vertexMain(const device Particle* particles [[buffer(0)]],
                            uint vid [[vertex_id]]) {
    VertexOut out;
    out.position  = float4(particles[vid].position.xy, 0.0, 1.0);
    out.pointSize = 2.0;

    float speed = length(particles[vid].velocity.xy);
    out.color = mix(float3(0.1, 0.4, 1.0),
                    float3(1.0, 0.3, 0.1),
                    clamp(speed * 5.0, 0.0, 1.0));
    return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]]) {
    return float4(in.color, 1.0);
}
