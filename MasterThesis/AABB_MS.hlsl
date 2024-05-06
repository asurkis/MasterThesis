#include "AABB_Common.hlsli"

uint GetVertexIndex(TMeshlet m, uint localIndex)
{
    return GlobalIndices[m.VertOffset + localIndex];
}

VertexOut GetVertexAttributes(uint meshletIndex, uint vertexIndex)
{
    TVertex v = Vertices[vertexIndex];

    VertexOut vout;
    vout.PositionVS = mul(float4(v.Position, 1), MainCB.MatView).xyz;
    vout.PositionHS = mul(float4(v.Position, 1), MainCB.MatViewProj);
    vout.MeshletIndex = meshletIndex;

    return vout;
}

[RootSignature(ROOT_SIG)]
[OutputTopology("line")]
[numthreads(12, 1, 1)]
void main(
    in payload TPayload payload,
    uint gid : SV_GroupID,
    uint gtid : SV_GroupThreadID,
    out indices uint2 lines[12],
    out vertices VertexOut verts[8])
{
    uint iMeshlet = payload.MeshletIndex[gid];
    TMeshlet m = Meshlets[iMeshlet];
    TBoundingBox box = MeshletBoxes[iMeshlet];

    SetMeshOutputCounts(8, 12);

    if (gtid < 8)
    {
        float3 ogPos;
        ogPos.x = gtid & 1 ? box.Max.x : box.Min.x;
        ogPos.y = gtid & 2 ? box.Max.y : box.Min.y;
        ogPos.z = gtid & 4 ? box.Max.z : box.Min.z;

        VertexOut vout;
        vout.PositionVS = mul(float4(ogPos, 1), MainCB.MatView).xyz;
        vout.PositionHS = mul(float4(ogPos, 1), MainCB.MatViewProj);
        vout.MeshletIndex = iMeshlet;
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
