#pragma once

#ifdef __cplusplus
#include <BasicTypes.h>
#endif

#define N_LODS_MAX 1
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
    uint MeshletIndex[GROUP_SIZE_AS];
};

#ifndef __cplusplus
#define ROOT_SIG                                                                                                       \
    "CBV(b0),"                                                                                                         \
    "RootConstants(b1, num32bitconstants=4),"                                                                          \
    "SRV(t0),"                                                                                                         \
    "SRV(t1),"                                                                                                         \
    "SRV(t2),"                                                                                                         \
    "SRV(t3),"                                                                                                         \
    "SRV(t4)"

ConstantBuffer<TCamera> Camera : register(b0);
ConstantBuffer<TMesh>   MeshInfo : register(b1);

StructuredBuffer<TVertex>      Vertices : register(t0);
StructuredBuffer<uint>         GlobalIndices : register(t1);
StructuredBuffer<uint>         Primitives : register(t2);
StructuredBuffer<TMeshlet>     Meshlets : register(t3);
StructuredBuffer<TBoundingBox> MeshletBoxes : register(t4);
#endif
