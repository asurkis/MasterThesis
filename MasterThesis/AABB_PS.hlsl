struct VertexOut
{
    float4 PositionHS : SV_Position;
    float3 PositionVS : POSITION0;
    uint   MeshletIndex : MESHLET_INDEX;
};

float4 main(VertexOut input) : SV_Target
{
    uint   meshletIndex = input.MeshletIndex;
    float3 diffuseColor = float3(float(meshletIndex & 1), float(meshletIndex & 3) / 4, float(meshletIndex & 7) / 8);
    return float4(diffuseColor, 1.0f);
}