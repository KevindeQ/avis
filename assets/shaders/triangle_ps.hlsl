struct PSInput
{
    float4 position : SV_POSITION;
    uint3 color : COLOR;
};

float4 main(PSInput input) : SV_TARGET
{
    return float4(input.color / 255.0f, 1.0f);
}