struct CameraData
{
    float4x4 MatView;
    float4x4 MatProj;
    float4x4 MatViewProj;
    float4x4 MatNormalViewProj;
};

struct MeshInfoData
{
    uint IndexBytes;
    uint MeshletOffset;
};

struct Vertex
{
    float3 Position;
    float3 Normal;
};

struct Meshlet
{
    uint VertCount;
    uint VertOffset;
    uint PrimCount;
    uint PrimOffset;
};

struct VertexOut
{
    float4 PositionHS : SV_Position;
    float3 PositionVS : POSITION0;
    float3 Normal : NORMAL0;
    uint   MeshletIndex : MESHLET_INDEX;
};

ConstantBuffer<CameraData>   Camera : register(b0);
ConstantBuffer<MeshInfoData> MeshInfo : register(b1);

StructuredBuffer<Vertex>  Vertices : register(t0);
StructuredBuffer<Meshlet> Meshlets : register(t1);
ByteAddressBuffer         UniqueVertexIndices : register(t2);
StructuredBuffer<uint>    PrimitiveIndices : register(t3);

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
        uint index     = (indexPair >> wordOffset) & 0xFFFF;

        return index;
    }
}

VertexOut GetVertexAttributes(uint meshletIndex, uint vertexIndex)
{
    Vertex v = Vertices[vertexIndex];

    VertexOut vout;
    vout.PositionVS = mul(float4(v.Position, 1), Camera.MatView).xyz;
    vout.PositionHS = mul(float4(v.Position, 1), Camera.MatViewProj);
    // vout.Normal = mul(float4(v.Normal, 0), Globals.World).xyz;
    vout.Normal       = v.Normal;
    vout.MeshletIndex = meshletIndex;

    return vout;
}

[RootSignature("CBV(b0), \
                RootConstants(b1, num32bitconstants=2), \
                SRV(t0), \
                SRV(t1), \
                SRV(t2), \
                SRV(t3)")]
[OutputTopology("triangle")]
[numthreads(128, 1, 1)]
void main(
    uint gid : SV_GroupID,
    uint gtid : SV_GroupThreadID,
    out indices uint3 tris[126],
    out vertices VertexOut verts[64])
{
    Meshlet m = Meshlets[gid];
    SetMeshOutputCounts(m.VertCount, m.PrimCount);

    if (gtid < m.PrimCount) tris[gtid] = GetPrimitive(m, gtid);

    if (gtid < m.VertCount)
    {
        uint vertexIndex = GetVertexIndex(m, gtid);
        verts[gtid]      = GetVertexAttributes(gid, vertexIndex);
    }
}
