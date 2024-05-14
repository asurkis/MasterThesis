#include "Util.hlsli"
#include "MainCommon.hlsli"

[RootSignature("RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT),CBV(b0)")]
TVertexOut main(TVertex vin, float3 off : INSTANCE_OFFSET, uint lod : INSTANCE_LOD)
{
    TVertexOut vout;
    vout.PositionVS = mul(float4(vin.Position + off, 1), MainCB.MatView).xyz;
    vout.PositionHS = mul(float4(vin.Position + off, 1), MainCB.MatViewProj);
    vout.Normal = mul(float4(vin.Normal, 0), MainCB.MatNormal).xyz;
    // vout.DiffuseColor = PaletteColor(lod);
    vout.DiffuseColor = 1.xxx;
    return vout;
}
