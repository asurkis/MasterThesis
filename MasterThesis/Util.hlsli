#pragma once

#define WAVE_SIZE 32
#define GROUP_SIZE_AS WAVE_SIZE

struct TCamera
{
    float4x4 MatView;
    float4x4 MatProj;
    float4x4 MatViewProj;
    float4x4 MatNormal;
};

struct TVertex
{
    float3 Position;
    float3 Normal;
};

struct TMeshlet
{
    uint VertCount;
    uint VertOffset;
    uint PrimCount;
    uint PrimOffset;
    uint ChildrenOffset;
    uint ChildrenCount;
    uint Parent1;
    uint Parent2;
};

struct TMeshletNode
{
    uint ChildrenOffset;
    uint ChildrenCount;
    uint Parent1;
    uint Parent2;
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
    uint RadiusThresholdMicro;
};

struct TPayload
{
    uint MeshletIndex[GROUP_SIZE_AS];
};

#define ROOT_SIG                                                                                                       \
    "CBV(b0),"                                                                                                         \
    "RootConstants(b1, num32bitconstants=4),"                                                                          \
    "SRV(t0),"                                                                                                         \
    "SRV(t1),"                                                                                                         \
    "SRV(t2),"                                                                                                         \
    "SRV(t3),"                                                                                                         \
    "SRV(t4)"

ConstantBuffer<TCamera> Camera : register(b0);
ConstantBuffer<TMesh> MeshInfo : register(b1);

StructuredBuffer<TVertex> Vertices : register(t0);
StructuredBuffer<uint> GlobalIndices : register(t1);
StructuredBuffer<uint> Primitives : register(t2);
StructuredBuffer<TMeshlet> Meshlets : register(t3);
StructuredBuffer<TBoundingBox> MeshletBoxes : register(t4);

float3 PaletteColor(uint idx)
{
    // float3(float(meshletIndex & 1), float(meshletIndex & 3) / 4, float(meshletIndex & 7) / 8)
    // 0b_0010_0100_1001_0010_0100_1001_0010_0100_1001 = 0x249249249
    uint3 t = uint3(idx, idx >> 1, idx >> 2);
    uint3 c = 0.xxx;
    for (uint i = 0; i < 11; ++i)
        c |= ((t >> (3 * i)) & 1) << (10 - i);
    return 1.xxx - float3(c) / 2047.0;
}
