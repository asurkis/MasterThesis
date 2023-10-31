struct VSOutput
{
    float4 pos : SV_Position;
    float3 col : COLOR;
};

[RootSignature("")]
VSOutput main(uint vertId : SV_VertexID)
{
    VSOutput result;
    switch (vertId)
    {
        case 0:
            result.pos = float4(0, 0, 0, 1);
            break;
        case 1:
            result.pos = float4(0, 1, 0, 1);
            break;
        case 2:
            result.pos = float4(1, 0, 0, 1);
            break;
    }
    result.col = float3(1, 1, 1);
    //result.pos = float4(pos, 0, 1);
    //result.col = col;
    return result;
}