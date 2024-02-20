#pragma once

#include "stdafx.h"

constexpr uint MESHLET_MAX_PRIMITIVES = 128;

struct Vertex
{
    float3 Position;
    float3 Normal;
};

struct MeshletDesc
{
    uint VertCount;
    uint VertOffset;
    uint PrimCount;
    uint PrimOffset;
    uint Parent1;
    uint Parent2;
};

struct BoundingBox
{
    float3 Min;
    float3 Max;
};

struct MeshDesc
{
    uint MeshletCount;
    uint MeshletTriangleOffsets;
};

struct ModelCPU
{
    std::vector<Vertex> Vertices;

    // Блоки с индексами для мешлетов
    std::vector<uint> GlobalIndices;

    // Индексы внутри мешлета, 10 бит на каждую из компонент
    std::vector<uint> Primitives;

    std::vector<MeshletDesc> Meshlets;
    std::vector<BoundingBox> MeshletBoxes;

    std::vector<MeshDesc> Meshes;

    void SaveToFile(const std::filesystem::path &path) const;
    void LoadFromFile(const std::filesystem::path &path);
};
