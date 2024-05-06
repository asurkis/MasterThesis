#include "Util.hlsli"
#include "MainCommon.hlsli"

[RootSignature("RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT),CBV(b0)")]
TVertexOut main(TVertex vin)
{
    TVertexOut vout;
    vout.PositionVS = mul(float4(vin.Position, 1), MainCB.MatView).xyz;
    vout.PositionHS = mul(float4(vin.Position, 1), MainCB.MatViewProj);
    vout.Normal = mul(float4(vin.Normal, 0), MainCB.MatNormal).xyz;
    vout.DiffuseColor = 1.xxx;
    return vout;
}
