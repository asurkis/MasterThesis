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

TVertexOut GetVertexAttributes(uint meshletIndex, uint vertexIndex)
{
    TVertex v = Vertices[vertexIndex];

    TVertexOut vout;
    vout.PositionVS = mul(float4(v.Position, 1), Camera.MatView).xyz;
    vout.PositionHS = mul(float4(v.Position, 1), Camera.MatViewProj);
    //vout.Normal = mul(float4(v.Normal, 0), Camera.MatNormal).xyz;
    vout.Normal = 0.xxx;
    vout.MeshletIndex = meshletIndex;
    // vout.Normal       = v.Normal;

    return vout;
}

[RootSignature(ROOT_SIG)]
[OutputTopology("triangle")]
[numthreads(GROUP_SIZE, 1, 1)]
void main(
    in payload TPayload Payload,
    uint gid : SV_GroupID,
    uint gtid : SV_GroupThreadID,
    out indices uint3 tris[126],
    out vertices TVertexOut verts[64])
{
    uint meshletIndex = Payload.MeshletIndex[gid];
    TMeshlet m = Meshlets[meshletIndex];
    SetMeshOutputCounts(m.VertCount, m.PrimCount);

    if (gtid < m.PrimCount)
        tris[gtid] = GetPrimitive(m, gtid);

    if (gtid < m.VertCount)
    {
        uint vertexIndex = GetVertexIndex(m, gtid);
        verts[gtid] = GetVertexAttributes(meshletIndex, vertexIndex);
    }
}
