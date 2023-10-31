struct VSOutput
{
    float4 pos : SV_Position;
    float3 col : COLOR;
};

VSOutput main(float2 pos : POSITION, float3 col : COLOR)
{
    VSOutput result;
    result.pos = float4(pos, 0, 1);
    result.col = col;
    return result;
}