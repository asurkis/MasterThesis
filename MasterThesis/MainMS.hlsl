#include "MainCommon.hlsli"

#define GROUP_SIZE 128

uint3 UnpackPrimitive(uint primitive)
{
    // Unpacks a 10 bits per index triangle from a 32-bit uint.
    return uint3(primitive & 0x3FF, (primitive >> 10) & 0x3FF, (primitive >> 20) & 0x3FF);
}

uint3 GetPrimitive(Meshlet m, uint index)
{
    uint prim = PrimitiveIndices[m.PrimOffset + index];
    return UnpackPrimitive(prim);
}

uint GetVertexIndex(Meshlet m, uint localIndex)
{
    localIndex += m.VertOffset;

    if (MeshInfo.IndexBytes == 4) // 32-bit Vertex Indices
    {
        return UniqueVertexIndices.Load(localIndex * 4);
    }
    else // 16-bit Vertex Indices
    {
        // Byte address must be 4-byte aligned.
        uint wordOffset = localIndex % 2 * 16;
        uint byteOffset = localIndex / 2 * 4;

        // Grab the pair of 16-bit indices, shift & mask off proper 16-bits.
        uint indexPair = UniqueVertexIndices.Load(byteOffset);
        uint index = (indexPair >> wordOffset) & 0xFFFF;

        return index;
    }
}

VertexOut GetVertexAttributes(uint meshletIndex, uint vertexIndex)
{
    Vertex v = Vertices[vertexIndex];

    VertexOut vout;
    vout.PositionVS = mul(float4(v.Position, 1), Camera.MatView).xyz;
    vout.PositionHS = mul(float4(v.Position, 1), Camera.MatViewProj);
    vout.Normal = mul(float4(v.Normal, 0), Camera.MatNormal).xyz;
    vout.MeshletIndex = meshletIndex;
    // vout.Normal       = v.Normal;

    return vout;
}

[RootSignature(ROOT_SIG)]
[OutputTopology("triangle")]
[numthreads(GROUP_SIZE, 1, 1)]
void main(
    uint gid : SV_GroupID,
    uint gtid : SV_GroupThreadID,
    out indices uint3 tris[126],
    out vertices VertexOut verts[64])
{
    Meshlet m = Meshlets[gid];
    SetMeshOutputCounts(m.VertCount, m.PrimCount);

    if (gtid < m.PrimCount)
        tris[gtid] = GetPrimitive(m, gtid);

    if (gtid < m.VertCount)
    {
        uint vertexIndex = GetVertexIndex(m, gtid);
        verts[gtid] = GetVertexAttributes(gid, vertexIndex);
    }
}
