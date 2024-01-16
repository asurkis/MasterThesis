#include "Shared.h"

bool ShouldDisplay(uint meshletIndex)
{
    uint i;
    
    if (meshletIndex >= MeshInfo.MeshletCount)
        return false;

    float4 boundingSphere = MeshletCulls[meshletIndex].BoundingSphere;
    float3 centerVS = mul(float4(boundingSphere.xyz, 1), Camera.MatView).xyz;
    
    bool culledByPlane[6];
    for (i = 0; i < 6; ++i)
        culledByPlane[i] = true;
    
    for (i = 0; i < 8; ++i)
    {
        float3 shift;
        shift.x = i & 1 ? 1 : -1;
        shift.y = i & 2 ? 1 : -1;
        shift.z = i & 4 ? 1 : -1;
        
        float3 ogPos = boundingSphere.xyz + boundingSphere.w * shift;
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
