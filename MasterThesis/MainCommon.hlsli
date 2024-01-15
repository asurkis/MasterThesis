#pragma once

#define ROOT_SIG \
    "CBV(b0)," \
    "RootConstants(b1, num32bitconstants=2)," \
    "SRV(t0)," \
    "SRV(t1)," \
    "SRV(t2)," \
    "SRV(t3)," \
    "SRV(t4)"

struct CameraData
{
    float4x4 MatView;
    float4x4 MatProj;
    float4x4 MatViewProj;
    float4x4 MatNormal;
};

struct MeshInfoData
{
    uint IndexBytes;
    uint MeshletOffset;
};

struct Vertex
{
    float3 Position;
    float3 Normal;
};

struct Meshlet
{
    uint VertCount;
    uint VertOffset;
    uint PrimCount;
    uint PrimOffset;
};

struct CullData
{
    float4 BoundingSphere; // xyz = center, w = radius
    uint NormalConePacked; // xyz = axis, w = -cos(a + 90)
    float ApexOffset; // apex = center - axis * offset
};

ConstantBuffer<CameraData> Camera : register(b0);
ConstantBuffer<MeshInfoData> MeshInfo : register(b1);

StructuredBuffer<Vertex> Vertices : register(t0);
StructuredBuffer<Meshlet> Meshlets : register(t1);
ByteAddressBuffer UniqueVertexIndices : register(t2);
StructuredBuffer<uint> PrimitiveIndices : register(t3);
StructuredBuffer<CullData> MeshletCulls : register(t4);

struct VertexOut
{
    float4 PositionHS : SV_Position;
    float3 PositionVS : POSITION0;
    float3 Normal : NORMAL0;
    uint MeshletIndex : MESHLET_INDEX;
};
