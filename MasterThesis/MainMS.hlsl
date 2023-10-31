struct VertexOut
{
    float4 pos : SV_Position;
    float3 col : COLOR;
};

[RootSignature("")]
[OutputTopology("triangle")]
[numthreads(1, 1, 1)]
void main(
    out indices uint3 tris[128],
    out vertices VertexOut verts[128])
{
    SetMeshOutputCounts(3, 1);
    
    verts[0].pos = float4(-1, -1, 0, 1);
    verts[1].pos = float4(0, 1, 0, 1);
    verts[2].pos = float4(1, 0, 0, 1);
    
    verts[0].col = float3(0, 0, 1);
    verts[1].col = float3(0, 1, 0);
    verts[2].col = float3(1, 0, 0);
    
    tris[0] = uint3(0, 1, 2);
}
