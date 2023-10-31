struct VertexOut
{
    float4 pos : SV_Position;
    float3 col : COLOR;
};

float4 main(VertexOut vOut) : SV_Target
{
    return float4(vOut.col, 1);
}
