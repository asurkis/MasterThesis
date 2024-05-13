#pragma once

#define WAVE_SIZE 32
#define GROUP_SIZE_AS WAVE_SIZE

struct TMainCB
{
    float4x4 MatView;
    float4x4 MatProj;
    float4x4 MatViewProj;
    float4x4 MatNormal;
    float4 CameraPos;
    float4 FloatInfo;
    uint4 IntInfo;
};

struct TVertex
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
};

struct TMeshlet
{
    uint VertCount;
    uint VertOffset;
    uint PrimCount;
    uint PrimOffset;
    uint ParentCount;
    uint ParentOffset;
    uint Height;
    float Error;
};

struct TBoundingBox
{
    float3 Min;
    float3 Max;
};

struct TMesh
{
    uint MeshletCount;
    uint MeshletOffset;
};

struct TPayload
{
    float4 Position;
    TMeshlet Meshlets[GROUP_SIZE_AS];
    uint MeshletIndex[GROUP_SIZE_AS];
    float VisibleRadius[GROUP_SIZE_AS];
};

#define ROOT_SIG                                                                                                       \
    "CBV(b0),"                                                                                                         \
    "RootConstants(b1, num32bitconstants=2),"                                                                          \
    "SRV(t0),"                                                                                                         \
    "SRV(t1),"                                                                                                         \
    "SRV(t2),"                                                                                                         \
    "SRV(t3),"                                                                                                         \
    "SRV(t4),"                                                                                                         \
    "SRV(t5)"

ConstantBuffer<TMainCB> MainCB : register(b0);
ConstantBuffer<TMesh> MeshInfo : register(b1);

StructuredBuffer<TVertex> Vertices : register(t0);
StructuredBuffer<uint> GlobalIndices : register(t1);
StructuredBuffer<uint> Primitives : register(t2);
StructuredBuffer<TMeshlet> Meshlets : register(t3);
StructuredBuffer<TBoundingBox> MeshletBoxesHierarchy : register(t4);
StructuredBuffer<TBoundingBox> MeshletBoxes : register(t5);

float3 PaletteColor(uint idx)
{
    // return 1.xxx;
    // float3(float(meshletIndex & 1), float(meshletIndex & 3) / 4, float(meshletIndex & 7) / 8)
    // 0b_0010_0100_1001_0010_0100_1001_0010_0100_1001 = 0x249249249
    uint3 t = uint3(idx, idx >> 1, idx >> 2);
    uint3 c = 0.xxx;
    for (uint i = 0; i < 11; ++i)
        c |= ((t >> (3 * i)) & 1) << (10 - i);
    return 1.xxx - float3(c) / 2047.0;
}

float3 HeatmapColor(float x)
{
    // blue -> cyan -> green -> yellow -> red
    if (x < 0.25)
        return float3(0.0, 4.0 * x, 1.0);
    else if (x < 0.5)
        return float3(0.0, 1.0, 4.0 * (0.5 - x));
    else if (x < 0.75)
        return float3(4.0 * (x - 0.5), 1.0, 0.0);
    else
        return float3(1.0, 4.0 * (1.0 - x), 0.0);
}

float TriangleArea(float4 oa4d, float4 ob4d, float4 oc4d)
{
    float2 oa2d = float2(MainCB.IntInfo.xy) * oa4d.xy / oa4d.w;
    float2 ob2d = float2(MainCB.IntInfo.xy) * ob4d.xy / ob4d.w;
    float2 oc2d = float2(MainCB.IntInfo.xy) * oc4d.xy / oc4d.w;
    float2 ab = ob2d - oa2d;
    float2 ac = oc2d - oa2d;
    float area = 0.5 * (ab.x * ac.y - ab.y * ac.x);
    return area;
}

float3 TriangleHeatmapColor(float4 oa, float4 ob, float4 oc)
{
    float area = TriangleArea(oa, ob, oc);
    return HeatmapColor(2.0 / (1.0 + area));
}

uint GetZCodeComponent2(uint x)
{
    x = x & 0x55555555;
    x = (x ^ (x >> 1)) & 0x33333333;
    x = (x ^ (x >> 2)) & 0x0F0F0F0F;
    x = (x ^ (x >> 4)) & 0x00FF00FF;
    x = (x ^ (x >> 8)) & 0x0000FFFF;
    return x;
}

uint GetZCodeComponent3(uint x)
{
    x = x & 0x49249249;
    x = (x ^ (x >> 2)) & 0xC30C30C3;
    x = (x ^ (x >> 4)) & 0x0F00F00F;
    x = (x ^ (x >> 8)) & 0xFF0000FF;
    x = (x ^ (x >> 16)) & 0x00000FFF;
    return x;
}
