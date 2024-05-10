#include "Util.hlsli"

ConstantBuffer<TMainCB> MainCB : register(b0);
StructuredBuffer<TMeshlet> Meshlets : register(t0);
StructuredBuffer<TBoundingBox> MeshletBoxes : register(t1);
StructuredBuffer<TIndexSpan> Parents : register(t2);
StructuredBuffer<uint> Children : register(t3);

// [0] = количество обработанных мешлетов
// [4] = следующий индекс для добавления в очередь
// [8] = количество мешлетов в очереди
// [12] = следующий индекс для взятия из очереди
// [16] = следующий мешлет для добавления
RWByteAddressBuffer Tasks : register(u0);
RWStructuredBuffer<uint2> Queue : register(u1);
RWStructuredBuffer<TInstancedMeshlet> InstancedMeshlets : register(u2);

bool IsCulled(TBoundingBox box, float3 off)
{
    uint i;
    
    bool culledByPlane[6];
    for (i = 0; i < 6; ++i)
        culledByPlane[i] = true;
    
    for (i = 0; i < 8; ++i)
    {
        float3 ogPos;
        ogPos.x = i & 1 ? box.Max.x : box.Min.x;
        ogPos.y = i & 2 ? box.Max.y : box.Min.y;
        ogPos.z = i & 4 ? box.Max.z : box.Min.z;
        
        float4 hs = mul(float4(ogPos + off, 1), MainCB.MatViewProj);
        culledByPlane[0] &= hs.w <= 0;
        culledByPlane[1] &= hs.z > hs.w;
        culledByPlane[2] &= hs.x < -hs.w;
        culledByPlane[3] &= hs.x > +hs.w;
        culledByPlane[4] &= hs.y < -hs.w;
        culledByPlane[5] &= hs.y > +hs.w;
    }
    
    for (i = 0; i < 6; ++i)
    {
        if (culledByPlane[i])
            return true;
    }
    
    return false;
}

bool IsEnough(float err, TBoundingBox box, float3 off)
{
    float3 center = 0.5 * (box.Min + box.Max) + off;
    float diameter = length(box.Max - box.Min);
    float4 hs = mul(float4(center, 1), MainCB.MatViewProj);
    if (hs.w <= 0)
        return true;
    float r = diameter / hs.w;
    // return err * r < MainCB.FloatInfo.w;
    return r < MainCB.FloatInfo.w;
}

void Instantiate(TMeshlet meshlet, float3 pos)
{
    uint jMeshlet;
    Tasks.InterlockedAdd(16, 1, jMeshlet);
    TInstancedMeshlet instanced;
    instanced.Vert = meshlet.Vert;
    instanced.Prim = meshlet.Prim;
    instanced.Position = pos;
    InstancedMeshlets[jMeshlet] = instanced;
}

void QueueChildren(TIndexSpan childrenIdx)
{
    for (uint iiChild = 0; iiChild < childrenIdx.Count; ++iiChild)
    {
        uint iChild = Children[childrenIdx.Offset + iiChild];
        uint pos;
        Tasks.InterlockedAdd(4, 1, pos);
        Queue[pos] = iChild;
        Tasks.InterlockedAdd(8, 1, pos);
    }
}

void ProcessMeshlet(uint iMeshlet, uint iInstance)
{
    TMeshlet meshlet = Meshlets[iMeshlet];
    TBoundingBox box = MeshletBoxes[iMeshlet];
    uint3 iPos = uint3(
        GetZCodeComponent3(iInstance >> 0),
        GetZCodeComponent3(iInstance >> 1),
        GetZCodeComponent3(iInstance >> 2));
    float3 pos = iPos * MainCB.FloatInfo.xyz;

    if (IsCulled(box, pos))
        return;

    TIndexSpan childrenIdx = Parents[iMeshlet];
    if (childrenIdx.Count == 0 || IsEnough(meshlet.Error, box, pos))
        Instantiate(meshlet, pos);
    else
        QueueChildren(childrenIdx);
}

[RootSignature(
    "CBV(b0),"
    "SRV(t0),"
    "SRV(t1),"
    "SRV(t2),"
    "SRV(t3),"
    "UAV(u0),"
    "UAV(u1),"
    "UAV(u2)"
)]
[numthreads(256, 1, 1)]
void main()
{
    for (;;)
    {
        uint nDone = Tasks.Load(0);
        uint nQueued = Tasks.Load(4);
        if (nDone >= nQueued)
            break;
        uint nReady = Tasks.Load(8);
        uint iiNextMeshlet = Tasks.Load(12);
        if (iiNextMeshlet >= nReady)
            continue;
        uint iiNextMeshletCmp;
        Tasks.InterlockedCompareExchange(12, iiNextMeshlet, iiNextMeshlet + 1, iiNextMeshletCmp);
        if (iiNextMeshlet != iiNextMeshletCmp)
            continue;
        uint2 meshletInstance = Queue[iiNextMeshlet];
        ProcessMeshlet(meshletInstance.x, meshletInstance.y);
        Tasks.InterlockedAdd(0, 1, nDone);
    }
}
