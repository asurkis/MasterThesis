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
    uint MeshletIndex : COLOR0;
};

ConstantBuffer<CameraData> Camera : register(b0);
ConstantBuffer<MeshInfoData> MeshInfo : register(b1);

StructuredBuffer<Vertex> Vertices : register(t0);
StructuredBuffer<Meshlet> Meshlets : register(t1);
ByteAddressBuffer UniqueVertexIndices : register(t2);
StructuredBuffer<uint> PrimitiveIndices : register(t3);

uint3 UnpackPrimitive(uint primitive)
{
    // Unpacks a 10 bits per index triangle from a 32-bit uint.
    return uint3(primitive & 0x3FF, (primitive >> 10) & 0x3FF, (primitive >> 20) & 0x3FF);
}

uint3 GetPrimitive(Meshlet m, uint index)
{
    return UnpackPrimitive(PrimitiveIndices[m.PrimOffset + index]);
}

uint GetVertexIndex(Meshlet m, uint localIndex)
{
    localIndex = m.VertOffset + localIndex;

    if (MeshInfo.IndexBytes == 4) // 32-bit Vertex Indices
    {
        return UniqueVertexIndices.Load(localIndex * 4);
    }
    else // 16-bit Vertex Indices
    {
        // Byte address must be 4-byte aligned.
        uint wordOffset = (localIndex & 0x1);
        uint byteOffset = (localIndex / 2) * 4;

        // Grab the pair of 16-bit indices, shift & mask off proper 16-bits.
        uint indexPair = UniqueVertexIndices.Load(byteOffset);
        uint index = (indexPair >> (wordOffset * 16)) & 0xffff;

        return index;
    }
}

VertexOut GetVertexAttributes(uint meshletIndex, uint vertexIndex)
{
    Vertex v = Vertices[vertexIndex];

    VertexOut vout;
    vout.PositionVS = mul(float4(v.Position, 1), Camera.MatView).xyz;
    vout.PositionHS = mul(float4(v.Position, 1), Camera.MatViewProj);
    //vout.Normal = mul(float4(v.Normal, 0), Globals.World).xyz;
    vout.Normal = v.Normal;
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
    uint gtid : SV_GroupThreadID,
    uint gid : SV_GroupID,
    out indices uint3 tris[126],
    out vertices VertexOut verts[64])
{
    Meshlet m = Meshlets[gid];
    SetMeshOutputCounts(m.VertCount, m.PrimCount);
    
    if (gtid < m.PrimCount)
    {
        tris[gtid] = GetPrimitive(m, gtid);
    }
    
    if (gtid < m.VertCount)
    {
        uint vertexIndex = GetVertexIndex(m, gtid);
        verts[gtid] = GetVertexAttributes(gid, vertexIndex);
    }
    // */
    
    /*
    SetMeshOutputCounts(8, 12);
    
    static const uint3 CubeIdx[] =
    {
        uint3(0, 0, 0), // 0
        uint3(1, 0, 0), // 1
        uint3(0, 1, 0), // 2
        uint3(1, 1, 0), // 3
        uint3(0, 0, 1), // 4
        uint3(1, 0, 1), // 5
        uint3(0, 1, 1), // 6
        uint3(1, 1, 1), // 7
    };
    
    static const uint3 Prims[] =
    {
        // -Z
        uint3(0, 1, 2),
        uint3(3, 2, 1),
        // +Z
        uint3(6, 5, 4),
        uint3(5, 6, 7),
        // -Y
        uint3(4, 1, 0),
        uint3(1, 4, 5),
        // +Y
        uint3(2, 3, 6),
        uint3(7, 6, 3),
        // -X
        uint3(0, 2, 4),
        uint3(6, 4, 2),
        // +X
        uint3(5, 3, 1),
        uint3(3, 5, 7),
    };
    
    VertexOut vert;
    if (gtid < 8)
    {
        float3 pos = 2 * float3(CubeIdx[gtid]) - 1;
        float3 col = float3(CubeIdx[gtid]);

        vert.PositionHS = mul(float4(pos, 1), Camera.MatViewProj);
        vert.PositionVS = mul(float4(pos, 1), Camera.MatView).xyz;
        vert.Normal = normalize(pos);
        verts[gtid] = vert;
    }
    
    if (gtid < 12)
        tris[gtid] = Prims[gtid];
    // */
}
