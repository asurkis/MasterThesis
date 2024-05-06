#pragma once

#include "stdafx.h"

inline void AssertFn(bool cond, std::string_view text, int line)
{
    if (cond)
        return;
    std::ostringstream oss;
    oss << text << " at line " << line;
    throw std::runtime_error(oss.str());
}

template <typename L, typename R>
inline void AssertEqFn(const L &left, const R &right, std::string_view leftText, std::string_view rightText, int line)
{
    if (left == right)
        return;
    std::ostringstream oss;
    oss << "Assertion failed:\n\tleft:  " << leftText << " = " << left << "\n\tright: " << rightText << " = " << right
        << "\nat line " << line;
    throw std::runtime_error(oss.str());
}

#define ASSERT_TEXT(cond, text) AssertFn(cond, text, __LINE__)
#define ASSERT(cond) AssertFn(cond, "Assertion failed: " #cond, __LINE__)
#define ASSERT_EQ(left, right) AssertEqFn(left, right, #left, #right, __LINE__)

constexpr uint MESHLET_MAX_PRIMITIVES = 128;

struct Vertex
{
    float3 Position;
    float3 Normal;
};

struct MeshletDesc
{
    uint  VertCount;
    uint  VertOffset;
    uint  PrimCount;
    uint  PrimOffset;
    uint  ParentCount;
    uint  ParentOffset;
    uint  Height;
    float Error;
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

struct MonoModelCPU
{
    std::vector<Vertex> Vertices;
    std::vector<uint>   Indices;

    void LoadGLB(const std::string &path);
};

struct MeshletModelCPU
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
