struct CameraData
{
    float4x4 MatView;
    float4x4 MatProj;
    float4x4 MatViewProj;
    float4x4 MatNormalViewProj;
};

ConstantBuffer<CameraData> Camera : register(b0);

struct VertexOut
{
    float4 pos : SV_Position;
    float3 col : COLOR;
};

[RootSignature("CBV(b0)")]
[OutputTopology("triangle")]
[numthreads(32, 1, 1)]
void main(
    uint gtid : SV_GroupThreadID,
    uint gid : SV_GroupID,
    out indices uint3 tris[128],
    out vertices VertexOut verts[128])
{
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
        vert.pos = float4(2 * float3(CubeIdx[gtid]) - float3(1, 1, 1), 1);
        vert.col = float3(CubeIdx[gtid]);
    }
        
    vert.pos = mul(vert.pos, transpose(Camera.MatViewProj));
    verts[gtid] = vert;
    
    if (gtid < 12)
        tris[gtid] = Prims[gtid];
}
