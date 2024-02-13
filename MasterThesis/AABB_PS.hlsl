#include "AABB_Common.hlsli"

float4 main(VertexOut input) : SV_Target
{
    uint meshletIndex = input.MeshletIndex;
    float3 diffuseColor = PaletteColor(meshletIndex);
    return float4(diffuseColor, 1.0f);
}
