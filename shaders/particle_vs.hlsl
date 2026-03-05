struct VSInput {
    float4 position : POSITION;
    float4 velocity : VELOCITY;
};

struct VSOutput {
    float4 pos   : SV_POSITION;
    float3 color : COLOR;
};

VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.pos = float4(input.position.xy, 0.0, 1.0);

    float speed = length(input.velocity.xy);
    output.color = lerp(float3(0.1, 0.4, 1.0),
                        float3(1.0, 0.3, 0.1),
                        saturate(speed * 5.0));
    return output;
}
