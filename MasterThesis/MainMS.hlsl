#include "MainCommon.hlsli"

groupshared TMeshlet Meshlet;

uint3 UnpackPrimitive(uint primitive)
{
    // Unpacks a 10 bits per index triangle from a 32-bit uint.
    return uint3(primitive & 0x3FF, (primitive >> 10) & 0x3FF, (primitive >> 20) & 0x3FF);
}

uint3 GetPrimitive(uint index)
{
    uint prim = Primitives[Meshlet.Prim.Offset + index];
    return UnpackPrimitive(prim);
}

// uint GetVertexIndex(uint localIndex)
// {
//     return GlobalIndices[Meshlet.VertOffset + localIndex];
// }

TVertexOut GetVertexAttributes(float3 pos, uint iLocVert)
{
    TVertex v = Vertices[Meshlet.Vert.Offset + iLocVert];
    TVertexOut vout;
    vout.PositionVS = mul(float4(v.Position + pos, 1), MainCB.MatView).xyz;
    vout.PositionHS = mul(float4(v.Position + pos, 1), MainCB.MatViewProj);
    vout.Normal = mul(float4(v.Normal, 0), MainCB.MatNormal).xyz;

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
    // vout.DiffuseColor = e.AdditionalInfo.xyz;
    vout.DiffuseColor = 1.xxx;

    return vout;
}

[RootSignature(ROOT_SIG)]
[OutputTopology("triangle")]
[numthreads(128, 1, 1)]
void main(
    in payload TPayload Payload,
    uint gid : SV_GroupID,
    uint gtid : SV_GroupThreadID,
    out vertices TVertexOut verts[128],
    out indices uint3 tris[128])
{
    Meshlet = Payload.Meshlets[gid];
    SetMeshOutputCounts(Meshlet.Vert.Count, Meshlet.Prim.Count);

    if (gtid < Meshlet.Prim.Count)
        tris[gtid] = GetPrimitive(gtid);

    uint iLocVert;
    
    // �������� ��� 2 ����, ����� �� ���� ����� �������������� �����
    iLocVert = gtid;
    if (iLocVert < Meshlet.Vert.Count)
        verts[iLocVert] = GetVertexAttributes(Payload.Position.xyz, iLocVert);
    
    //iLocVert = gtid + 128;
    //if (iLocVert < Meshlet.VertCount)
    //    verts[iLocVert] = GetVertexAttributes(Payload.Position.xyz, iLocVert);
}
