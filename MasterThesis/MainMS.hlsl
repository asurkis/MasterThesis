#include "MainCommon.hlsli"

struct TPrimOut1
{
    float3 DiffuseColor : COLOR0;
};

groupshared TMeshlet Meshlet;

uint3 UnpackPrimitive(uint primitive)
{
    // Unpacks a 10 bits per index triangle from a 32-bit uint.
    return uint3(primitive & 0x3FF, (primitive >> 10) & 0x3FF, (primitive >> 20) & 0x3FF);
}

uint3 GetPrimitive(uint index)
{
    uint prim = Primitives[Meshlet.PrimOffset + index];
    return UnpackPrimitive(prim);
}

TVertexOut GetVertexAttributes(float3 pos, uint iLocVert)
{
    // uint iVert = GlobalIndices[Meshlet.VertOffset + iLocVert];
    uint iVert = Meshlet.VertOffset + iLocVert;
    TVertex v = Vertices[iVert];
    TVertexOut vout;
    vout.PositionVS = mul(float4(v.Position + pos, 1), MainCB.MatView).xyz;
    vout.PositionHS = mul(float4(v.Position + pos, 1), MainCB.MatViewProj);
    vout.Normal = mul(float4(v.Normal, 0), MainCB.MatNormal).xyz;
    vout.DiffuseColor = 1.xxx;

    /*
    if (MeshInfo.DisplayType == 0xFFFFFFFF)
    {
        vout.DiffuseColor = PaletteColor(m.iMeshlet);
    }
    else
    {
        switch (MeshInfo.DisplayType % 3)
        {
            case 0:
                if (isBorder)
                    vout.DiffuseColor = 0.xxx;
                else
                    vout.DiffuseColor = PaletteColor(m.iMeshlet);
                break;
            case 1:
                vout.DiffuseColor = PaletteColor(m.iMeshlet);
                break;
            case 2:
                vout.DiffuseColor = PaletteColor(iParent);
                break;
        }
    }
    */

    return vout;
}

[RootSignature(ROOT_SIG)]
[OutputTopology("triangle")]
[numthreads(128, 1, 1)]
void main(
    in payload TPayload Payload,
    uint gid : SV_GroupID,
    uint gtid : SV_GroupThreadID,
    out vertices TVertexOut Verts[128],
    out indices uint3 Idx[128])
{
    Meshlet = Payload.Meshlets[gid];
    SetMeshOutputCounts(Meshlet.VertCount, Meshlet.PrimCount);
    
    if (gtid < Meshlet.VertCount)
        Verts[gtid] = GetVertexAttributes(Payload.Position.xyz, gtid);

    if (gtid < Meshlet.PrimCount)
        Idx[gtid] = GetPrimitive(gtid);
}
