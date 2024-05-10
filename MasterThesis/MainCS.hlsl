#include "Util.hlsli"

ConstantBuffer<TMainCB> MainCB : register(b0);
StructuredBuffer<TMeshlet> Meshlets : register(t0);
StructuredBuffer<TBoundingBox> MeshletBoxes : register(t1);
StructuredBuffer<TIndexSpan> Parents : register(t2);
StructuredBuffer<uint> Children : register(t3);

// [0] = количество обработанных мешлетов
// [4] = количество мешлетов в очереди
// [8] = следующий индекс для добавления в очередь
// [12] = следующий индекс для взятия из очереди
RWByteAddressBuffer Tasks : register(u0);
RWBuffer<uint2> Queue : register(u1);
RWStructuredBuffer<TInstancedMeshlet> InstancedMeshlets : register(u2);

void ProcessMeshlet(uint iMeshlet, uint iInstance)
{
    
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
    /*
    for (uint iter = 0; iter < 256; ++iter)
    {
        uint DoneCount = Tasks.Load(0);
        uint QueuedCount = Tasks.Load(4);
        if (DoneCount >= QueuedCount)
            break;
        uint iiMeshlet = Tasks.Load(12);
        uint iiMeshletFound;
        Tasks.InterlockedCompareExchange(12, iiMeshlet, iiMeshlet + 1, iiMeshletFound);
        if (iiMeshletFound != iiMeshlet)
            continue;
        uint2 meshletInstance = Queue[iiMeshlet];
        ProcessMeshlet(meshletInstance.x, meshletInstance.y);
        uint dummy;
        Tasks.InterlockedAdd(0, 1, dummy);
    }
    */
}
