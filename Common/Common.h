#pragma once

#include "stdafx.h"

struct Vertex
{
    float3 Position;
};

struct MeshletDesc
{
    uint VertCount;
    uint VertOffset;
    uint PrimCount;
    uint PrimOffset;
};

struct BoundingBox
{
    float3 Min;
    float3 Max;
};

struct MeshDesc
{
    uint MeshletCount;
    uint MeshletOffset;
};

struct ModelCPU
{
    std::vector<Vertex> Vertices;

    // Блоки с индексами для мешлетов
    std::vector<uint> GlobalIndices;

    // Индексы внутри мешлета, 10 бит на каждую из компонент
    std::vector<uint> LocalIndices;

    std::vector<MeshletDesc> Meshlets;
    std::vector<BoundingBox> MeshletBoxes;

    std::vector<MeshDesc> Meshes;

    void SaveToFile(const std::filesystem::path &path) const;
    void LoadFromFile(const std::filesystem::path &path);
};
