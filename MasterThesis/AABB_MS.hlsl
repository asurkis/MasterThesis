#include "AABB_Common.hlsli"

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
    vout.MeshletIndex = meshletIndex;

    return vout;
}

#define GROUP_SIZE 128
groupshared float3 aabbMin[GROUP_SIZE];
groupshared float3 aabbMax[GROUP_SIZE];

[RootSignature(ROOT_SIG)]
[OutputTopology("line")]
[numthreads(GROUP_SIZE, 1, 1)]
void main(
    uint gid : SV_GroupID,
    uint gtid : SV_GroupThreadID,
    out indices uint2 lines[12],
    out vertices VertexOut verts[8])
{
    Meshlet m = Meshlets[gid];

    if (gtid < m.VertCount)
    {
        uint vid = GetVertexIndex(m, gtid);
        Vertex v = Vertices[vid];
        aabbMin[gtid] = v.Position;
        aabbMax[gtid] = v.Position;
    }
    GroupMemoryBarrier();
    if (gtid >= m.VertCount)
    {
        aabbMin[gtid] = aabbMin[0];
        aabbMax[gtid] = aabbMax[0];
    }
    GroupMemoryBarrier();

    for (int step = 1; step < 128; step *= 2)
    {
        if (gtid % step == 0 && gtid + step < 128)
        {
            aabbMin[gtid] = min(aabbMin[gtid], aabbMin[gtid + step]);
            aabbMax[gtid] = max(aabbMax[gtid], aabbMax[gtid + step]);
        }
        GroupMemoryBarrier();
    }

    SetMeshOutputCounts(8, 12);

    if (gtid < 8)
    {
        /*
        float3 ogPos;
        float3 boxMin = aabbMin[0];
        float3 boxMax = aabbMax[0];
        switch (gtid)
        {
            case 0:
                ogPos = float3(boxMin.x, boxMin.y, boxMin.z);
                break;
            case 1:
                ogPos = float3(boxMax.x, boxMin.y, boxMin.z);
                break;
            case 2:
                ogPos = float3(boxMin.x, boxMax.y, boxMin.z);
                break;
            case 3:
                ogPos = float3(boxMax.x, boxMax.y, boxMin.z);
                break;
            case 4:
                ogPos = float3(boxMin.x, boxMin.y, boxMax.z);
                break;
            case 5:
                ogPos = float3(boxMax.x, boxMin.y, boxMax.z);
                break;
            case 6:
                ogPos = float3(boxMin.x, boxMax.y, boxMax.z);
                break;
            case 7:
                ogPos = float3(boxMax.x, boxMax.y, boxMax.z);
                break;
        }

        VertexOut vout;
        vout.PositionVS = mul(float4(ogPos, 1), Camera.MatView).xyz;
        vout.PositionHS = mul(float4(ogPos, 1), Camera.MatViewProj);
        vout.MeshletIndex = gid;
        verts[gtid] = vout;
        */
        
        float3 xyz;
        switch (gtid)
        {
            case 0:
                xyz = float3(-1, -1, -1);
                break;
            case 1:
                xyz = float3(1, -1, -1);
                break;
            case 2:
                xyz = float3(-1, 1, -1);
                break;
            case 3:
                xyz = float3(1, 1, -1);
                break;
            case 4:
                xyz = float3(-1, -1, 1);
                break;
            case 5:
                xyz = float3(1, -1, 1);
                break;
            case 6:
                xyz = float3(-1, 1, 1);
                break;
            case 7:
                xyz = float3(1, 1, 1);
                break;
        }
        
        float4 sphere = MeshletCulls[gid].BoundingSphere;
        float3 center = sphere.xyz;
        float radius = sphere.w;

        float3 centerVS = mul(float4(center, 1), Camera.MatView).xyz;
        
        VertexOut vout;
        vout.PositionVS = centerVS + radius * xyz;
        vout.PositionHS = mul(float4(vout.PositionVS, 1), Camera.MatProj);
        vout.MeshletIndex = gid;
        verts[gtid] = vout;
    }

    switch (gtid)
    {
        case 0:
            lines[gtid] = uint2(0, 1);
            break;
        case 1:
            lines[gtid] = uint2(0, 2);
            break;
        case 2:
            lines[gtid] = uint2(0, 4);
            break;
        case 3:
            lines[gtid] = uint2(1, 3);
            break;
        case 4:
            lines[gtid] = uint2(1, 5);
            break;
        case 5:
            lines[gtid] = uint2(2, 3);
            break;
        case 6:
            lines[gtid] = uint2(2, 6);
            break;
        case 7:
            lines[gtid] = uint2(3, 7);
            break;
        case 8:
            lines[gtid] = uint2(4, 5);
            break;
        case 9:
            lines[gtid] = uint2(4, 6);
            break;
        case 10:
            lines[gtid] = uint2(5, 7);
            break;
        case 11:
            lines[gtid] = uint2(6, 7);
            break;
    }
}
