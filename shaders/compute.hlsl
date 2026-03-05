struct Particle {
    float4 position;
    float4 velocity;
};

cbuffer ComputeParams : register(b0) {
    float deltaTime;
    float bounds;
};

RWStructuredBuffer<Particle> particles : register(u0);

[numthreads(256, 1, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID) {
    uint idx = DTid.x;

    particles[idx].position.xyz += particles[idx].velocity.xyz * deltaTime;

    if (particles[idx].position.x > bounds) {
        particles[idx].position.x = -bounds;
    }
}
