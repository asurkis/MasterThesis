#include "MainCommon.hlsli"

#define GROUP_SIZE 128

uint3 UnpackPrimitive(uint primitive)
{
    // Unpacks a 10 bits per index triangle from a 32-bit uint.
    return uint3(primitive & 0x3FF, (primitive >> 10) & 0x3FF, (primitive >> 20) & 0x3FF);
}

uint3 GetPrimitive(TMeshlet m, uint index)
{
    uint prim = Primitives[m.PrimOffset + index];
    return UnpackPrimitive(prim);
}

uint GetVertexIndex(TMeshlet m, uint localIndex)
{
    return GlobalIndices[m.VertOffset + localIndex];
}

TVertexOut GetVertexAttributes(uint iMeshlet, TMeshlet meshlet, uint iLocVert)
{
    uint iParent = meshlet.Parent1;
    uint iVert = GetVertexIndex(meshlet, iLocVert);
    bool isBorder = (iVert & 0x80000000) != 0;
    iVert &= 0x7FFFFFFF;
    TVertex v = Vertices[iVert];

    TVertexOut vout;
    vout.PositionVS = mul(float4(v.Position, 1), Camera.MatView).xyz;
    vout.PositionHS = mul(float4(v.Position, 1), Camera.MatViewProj);
    vout.Normal = mul(float4(v.Normal, 0), Camera.MatNormal).xyz;
    
    if (MeshInfo.DisplayType == 0xFFFFFFFF)
    {
        vout.DiffuseColor = PaletteColor(iMeshlet);
    }
    else
    {
        switch (MeshInfo.DisplayType % 3)
        {
            case 0:
                if (isBorder)
                    vout.DiffuseColor = 0.xxx;
                else
                    vout.DiffuseColor = PaletteColor(iMeshlet);
                break;
            case 1:
                vout.DiffuseColor = PaletteColor(iMeshlet);
                break;
            case 2:
                vout.DiffuseColor = PaletteColor(iParent);
                break;
        }
    }

    return vout;
}

[RootSignature(ROOT_SIG)]
[OutputTopology("triangle")]
[numthreads(GROUP_SIZE, 1, 1)]
void main(
    in payload TPayload Payload,
    uint gid : SV_GroupID,
    uint gtid : SV_GroupThreadID,
    out indices uint3 tris[128],
    out vertices TVertexOut verts[256])
{
    uint iMeshlet = Payload.MeshletIndex[gid];
    TMeshlet m = Meshlets[iMeshlet];
    uint iParent = m.Parent1;
    SetMeshOutputCounts(m.VertCount, m.PrimCount);

    if (gtid < m.PrimCount)
        tris[gtid] = GetPrimitive(m, gtid);

    uint iLocVert;
    
    // Повторим код 2 раза, чтобы не было менее предсказуемого цикла
    iLocVert = gtid;
    if (iLocVert < m.VertCount)
        verts[iLocVert] = GetVertexAttributes(iMeshlet, m, iLocVert);
    
    iLocVert = gtid + 128;
    if (iLocVert < m.VertCount)
        verts[iLocVert] = GetVertexAttributes(iMeshlet, m, iLocVert);
}
