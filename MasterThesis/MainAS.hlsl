#include "Util.hlsli"

bool IsCulled(TBoundingBox box)
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
            return true;
    }
    
    return false;
}

bool IsEnough(TBoundingBox box)
{
    float3 center = 0.5 * (box.Min + box.Max);
    float diameter = length(box.Max - box.Min);
    float4 hs = mul(float4(center, 1), Camera.MatViewProj);
    if (hs.w <= 0)
        return true;
    float r = diameter / hs.w;
    return r < 1e-6f * MeshInfo.RadiusThresholdMicro;
}

bool ShouldDisplay(uint iMeshlet)
{
    if (iMeshlet >= MeshInfo.MeshletCount)
        return false;
    
    TMeshlet meshlet = Meshlets[iMeshlet];
    uint iParent = meshlet.Parent1;
    bool isRoot = iParent == 0;
    bool isLeaf = meshlet.ChildrenCount == 0;
    if (!isRoot)
    {
        TBoundingBox parentBox = MeshletBoxes[iParent];
        if (IsCulled(parentBox))
            return false;
        if (IsEnough(parentBox))
            return false;
    }
    
    TBoundingBox box = MeshletBoxes[iMeshlet];
    if (IsCulled(box))
        return false;
    return isLeaf || IsEnough(box);
}

[numthreads(GROUP_SIZE_AS, 1, 1)]
void main(
    uint dtid : SV_DispatchThreadID,
    uint gtid : SV_GroupThreadID,
    uint gid : SV_GroupID)
{
    uint iMeshlet = MeshInfo.MeshletOffset + dtid;
    bool shouldDisplay = ShouldDisplay(iMeshlet);
    
    uint current = WavePrefixCountBits(shouldDisplay);
    uint nDispatch = WaveActiveCountBits(shouldDisplay);

    TPayload p;
    if (shouldDisplay)
    {
        p.MeshletIndex[current] = iMeshlet;
    }
    DispatchMesh(nDispatch, 1, 1, p);
}
