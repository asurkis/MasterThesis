struct VSOutput
{
    float4 pos : SV_Position;
    float3 col : COLOR;
};

float4 main(VSOutput vsOut) : SV_Target
{
    return float4(vsOut.col, 1);
}
