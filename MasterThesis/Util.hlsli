#pragma once

struct TIndexSpan
{
    uint Count;
    uint Offset;
};

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
    TIndexSpan Vert;
    TIndexSpan Prim;
    TIndexSpan Parent;
    uint Height;
    float Error;
};

struct TInstancedMeshlet
{
    TIndexSpan Vert;
    TIndexSpan Prim;
    float3 Position;
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

float3 PaletteColor(uint idx)
{
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
