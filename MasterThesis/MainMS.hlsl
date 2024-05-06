#include "MainCommon.hlsli"

#define GROUP_SIZE 128

struct TMeshletExpanded
{
    float4x4 MatTransView;
    float4x4 MatTransViewProj;
    float4x4 MatTransNormal;
    float4 AdditionalInfo;
    uint iMeshlet;
    TMeshlet m;
};

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

TVertexOut GetVertexAttributes(TMeshletExpanded e, uint iLocVert)
{
    uint iParent = e.m.ParentOffset;
    uint iVert = GetVertexIndex(e.m, iLocVert);
    bool isBorder = (iVert & 0x80000000) != 0;
    iVert &= 0x7FFFFFFF;
    TVertex v = Vertices[iVert];

    TVertexOut vout;
    vout.PositionVS = mul(float4(v.Position, 1), e.MatTransView).xyz;
    vout.PositionHS = mul(float4(v.Position, 1), e.MatTransViewProj);
    vout.Normal = mul(float4(v.Normal, 0), e.MatTransNormal).xyz;
    
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
[numthreads(GROUP_SIZE, 1, 1)]
void main(
    in payload TPayload Payload,
    uint gid : SV_GroupID,
    uint gtid : SV_GroupThreadID,
    out indices uint3 tris[128],
    out vertices TVertexOut verts[256])
{
    float4x4 MatTransform = Payload.MatTransform;
    TMeshletExpanded e;
    e.MatTransView = mul(MatTransform, Camera.MatView);
    e.MatTransViewProj = mul(MatTransform, Camera.MatViewProj);
    e.MatTransNormal = mul(MatTransform, Camera.MatNormal);
    e.AdditionalInfo = Payload.AdditionalInfo[gid];

    e.iMeshlet = Payload.MeshletIndex[gid];
    e.m = Meshlets[e.iMeshlet];
    SetMeshOutputCounts(e.m.VertCount, e.m.PrimCount);

    if (gtid < e.m.PrimCount)
        tris[gtid] = GetPrimitive(e.m, gtid);

    uint iLocVert;
    
    // Повторим код 2 раза, чтобы не было менее предсказуемого цикла
    iLocVert = gtid;
    if (iLocVert < e.m.VertCount)
        verts[iLocVert] = GetVertexAttributes(e, iLocVert);
    
    iLocVert = gtid + 128;
    if (iLocVert < e.m.VertCount)
        verts[iLocVert] = GetVertexAttributes(e, iLocVert);
}
