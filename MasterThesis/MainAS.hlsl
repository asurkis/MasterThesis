#include "Util.hlsli"

groupshared float3 MeshPosition;

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
        
        float4 hs = mul(float4(ogPos + MeshPosition, 1), MainCB.MatViewProj);
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

float AngularRadius(TBoundingBox box)
{
    float3 center = 0.5 * (box.Min + box.Max) + MeshPosition;
    float diameter = length(box.Max - box.Min);
    float4 hs = mul(float4(center, 1), MainCB.MatViewProj);
    if (hs.w <= 0)
        return 1.0f;
    return diameter / hs.w;
}

float PixelRadius(TBoundingBox box)
{
    return AngularRadius(box) * min(MainCB.IntInfo.x, MainCB.IntInfo.y);
}

bool IsEnough(float err, TBoundingBox box)
{
    float r = AngularRadius(box);
    // float r = PixelRadius(box);
    // return err * r < MainCB.FloatInfo.w;
    return r < MainCB.FloatInfo.w;
}

bool ShouldDisplay(uint iMeshlet, out TMeshlet meshlet)
{
    if (iMeshlet >= MeshInfo.MeshletCount)
        return false;
    
    meshlet = Meshlets[iMeshlet];
    TBoundingBox box = MeshletBoxesHierarchy[iMeshlet];
    if (MainCB.IntInfo.z != 0xFFFFFFFF)
    {
        return meshlet.Height == MainCB.IntInfo.z / 3;
    }
    
    uint iParent = meshlet.ParentOffset;
    bool isRoot = meshlet.ParentCount == 0;
    bool isLeaf = meshlet.Height == 0;
    
    if (!isRoot)
    {
        TMeshlet parent = Meshlets[iParent];
        TBoundingBox parentBox = MeshletBoxesHierarchy[iParent];
        if (IsCulled(parentBox))
            return false;
        if (IsEnough(parent.Error, parentBox))
            return false;
    }
    
    if (IsCulled(box))
        return false;
    return isLeaf || IsEnough(meshlet.Error, box);
}

[numthreads(GROUP_SIZE_AS, 1, 1)]
void main(
    uint3 dtid : SV_DispatchThreadID,
    uint3 gtid : SV_GroupThreadID,
    uint3 gid : SV_GroupID)
{
    uint iInstance = dtid.y;
    uint iMeshlet = MeshInfo.MeshletOffset + dtid.x;
    uint3 iPos = uint3(
        GetZCodeComponent3(iInstance >> 0),
        GetZCodeComponent3(iInstance >> 1),
        GetZCodeComponent3(iInstance >> 2));
    float3 pos = iPos * MainCB.FloatInfo.xyz;
    MeshPosition = pos;
    TMeshlet meshlet;
    bool shouldDisplay = ShouldDisplay(iMeshlet, meshlet);
    
    uint current = WavePrefixCountBits(shouldDisplay);
    uint nDispatch = WaveActiveCountBits(shouldDisplay);

    TPayload p;
    p.Position = float4(pos, 1);
    if (shouldDisplay)
    {
        p.Meshlets[current] = meshlet;
        p.MeshletIndex[current] = iMeshlet;
        p.VisibleRadius[current] = PixelRadius(MeshletBoxes[iMeshlet]);
    }
    DispatchMesh(nDispatch, 1, 1, p);
}
