#include "Shared.h"

bool ShouldDisplay(uint iMeshlet)
{
    uint i;
    
    if (iMeshlet >= MeshInfo.MeshletCount)
        return false;

    TBoundingBox box = MeshletBoxes[iMeshlet];
    
    bool culledByPlane[6];
    for (i = 0; i < 6; ++i)
        culledByPlane[i] = true;
    
    for (i = 0; i < 8; ++i)
    {
        float3 ogPos;
        ogPos.x = i & 1 ? box.Max.x : box.Min.x;
        ogPos.y = i & 2 ? box.Max.y : box.Min.y;
        ogPos.z = i & 4 ? box.Max.z : box.Min.z;
        
        float4 hs = mul(float4(ogPos, 1), Camera.MatViewProj);
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
            return false;
    }
    
    return true;
}

[numthreads(GROUP_SIZE_AS, 1, 1)]
void main(
    uint dtid : SV_DispatchThreadID,
    uint gtid : SV_GroupThreadID,
    uint gid : SV_GroupID)
{
    uint meshletIndex = MeshInfo.MeshletOffset + dtid;
    bool shouldDisplay = ShouldDisplay(meshletIndex);
    
    uint current = WavePrefixCountBits(shouldDisplay);
    uint nDispatch = WaveActiveCountBits(shouldDisplay);

    TPayload p;
    if (shouldDisplay)
    {
        p.MeshletIndex[current] = meshletIndex;
    }
    DispatchMesh(nDispatch, 1, 1, p);
}
