#pragma once

#define N_LODS_MAX 6

#ifdef __cplusplus

#include <DirectXMath.h>

using uint  = uint32_t;
using float4 = DirectX::XMVECTOR;
using float4x4 = DirectX::XMMATRIX;

using float3 = DirectX::XMFLOAT3;

#endif

struct TCamera
{
    float4x4 MatView;
    float4x4 MatProj;
    float4x4 MatViewProj;
    float4x4 MatNormal;
};

struct TMesh
{
    uint IndexBytes;
    uint MeshletOffset;
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

struct TMeshletCull
{
    float4 BoundingSphere;   // xyz = center, w = radius
    uint   NormalConePacked; // xyz = axis, w = -cos(a + 90)
    float  ApexOffset;       // apex = center - axis * offset
};

#ifndef __cplusplus
#define ROOT_SIG                                                                                                       \
    "CBV(b0),"                                                                                                         \
    "RootConstants(b1, num32bitconstants=2),"                                                                          \
    "SRV(t0),"                                                                                                         \
    "SRV(t1),"                                                                                                         \
    "SRV(t2),"                                                                                                         \
    "SRV(t3),"                                                                                                         \
    "SRV(t4)"

ConstantBuffer<TCamera> Camera : register(b0);
ConstantBuffer<TMesh>   MeshInfo : register(b1);

StructuredBuffer<TVertex>      Vertices : register(t0);
StructuredBuffer<TMeshlet>     Meshlets : register(t1);
ByteAddressBuffer              UniqueVertexIndices : register(t2);
StructuredBuffer<uint>         PrimitiveIndices : register(t3);
StructuredBuffer<TMeshletCull> MeshletCulls : register(t4);
#endif
