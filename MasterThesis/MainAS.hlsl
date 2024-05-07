#include "Util.hlsli"

bool IsCulled(float4x4 MatFull, TBoundingBox box)
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
        
        float4 hs = mul(float4(ogPos, 1), MatFull);
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

bool IsEnough(float4x4 MatFull, float err, TBoundingBox box, out float VisibleRadius)
{
    float3 center = 0.5 * (box.Min + box.Max);
    float diameter = length(box.Max - box.Min);
    float4 hs = mul(float4(center, 1), MatFull);
    if (hs.w <= 0)
        return true;
    float r = diameter / hs.w;
    VisibleRadius = r;
    return err * r < MainCB.FloatInfo.w;
    // return r < Camera.FloatInfo.w;
}

bool ShouldDisplay(float4x4 MatFull, uint iMeshlet, out float VisibleRadius)
{
    if (iMeshlet >= MeshInfo.MeshletCount)
        return false;
    
    TMeshlet meshlet = Meshlets[iMeshlet];
    TBoundingBox box = MeshletBoxes[iMeshlet];
    if (MainCB.IntInfo.x != 0xFFFFFFFF)
    {
        IsEnough(MatFull, meshlet.Error, box, VisibleRadius);
        return meshlet.Height == MainCB.IntInfo.x / 3;
    }
    
    uint iParent = meshlet.ParentOffset;
    bool isRoot = meshlet.ParentCount == 0;
    bool isLeaf = meshlet.Height == 0;
    
    if (!isRoot)
    {
        TMeshlet parent = Meshlets[iParent];
        TBoundingBox parentBox = MeshletBoxes[iParent];
        if (IsCulled(MatFull, parentBox))
            return false;
        float blank;
        if (IsEnough(MatFull, parent.Error, parentBox, blank))
            return false;
    }
    
    if (IsCulled(MatFull, box))
        return false;
    return isLeaf || IsEnough(MatFull, meshlet.Error, box, VisibleRadius);
}

[numthreads(GROUP_SIZE_AS, 1, 1)]
void main(
    uint3 dtid : SV_DispatchThreadID,
    uint3 gtid : SV_GroupThreadID,
    uint3 gid : SV_GroupID)
{
    uint iInstance = dtid.y;
    uint iMeshlet = MeshInfo.MeshletOffset + dtid.x;
    uint3 iOff = uint3(
        GetZCodeComponent3(iInstance >> 0),
        GetZCodeComponent3(iInstance >> 1),
        GetZCodeComponent3(iInstance >> 2));
    float3 off = iOff * MainCB.FloatInfo.xyz;
    float4x4 MatTranslate = float4x4(
        float4(1.0f, 0.0f, 0.0f, 0.0f),
        float4(0.0f, 1.0f, 0.0f, 0.0f),
        float4(0.0f, 0.0f, 1.0f, 0.0f),
        float4(off, 1.0f)
    );
    float4x4 MatFull = mul(MatTranslate, MainCB.MatViewProj);
    float VisibleRadius = 0.0f;
    bool shouldDisplay = ShouldDisplay(MatFull, iMeshlet, VisibleRadius);
    
    uint current = WavePrefixCountBits(shouldDisplay);
    uint nDispatch = WaveActiveCountBits(shouldDisplay);

    TPayload p;
    p.MatTransform = MatTranslate;
    if (shouldDisplay)
    {
        p.MeshletIndex[current] = iMeshlet;
        p.AdditionalInfo[current].x = VisibleRadius;
        p.AdditionalInfo[current].y = MainCB.FloatInfo.w;
        p.AdditionalInfo[current].z = VisibleRadius < MainCB.FloatInfo.w ? 0.0f : 1.0f;
        p.AdditionalInfo[current].w = 0.0f;
    }
    DispatchMesh(nDispatch, 1, 1, p);
}
