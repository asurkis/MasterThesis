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
    float4x4 MatTransform;
    float4 AdditionalInfo[GROUP_SIZE_AS];
    uint MeshletIndex[GROUP_SIZE_AS];
};

#define ROOT_SIG                                                                                                       \
    "CBV(b0),"                                                                                                         \
    "RootConstants(b1, num32bitconstants=2),"                                                                          \
    "SRV(t0),"                                                                                                         \
    "SRV(t1),"                                                                                                         \
    "SRV(t2),"                                                                                                         \
    "SRV(t3),"                                                                                                         \
    "SRV(t4)"

ConstantBuffer<TMainCB> MainCB : register(b0);
ConstantBuffer<TMesh> MeshInfo : register(b1);

StructuredBuffer<TVertex> Vertices : register(t0);
StructuredBuffer<uint> GlobalIndices : register(t1);
StructuredBuffer<uint> Primitives : register(t2);
StructuredBuffer<TMeshlet> Meshlets : register(t3);
StructuredBuffer<TBoundingBox> MeshletBoxes : register(t4);

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
