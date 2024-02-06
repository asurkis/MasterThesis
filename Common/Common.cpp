#include "stdafx.h"

#include "Common.h"

template <typename T> static void WriteVec(std::ostream &sout, const std::vector<T> &data)
{
    static_assert(std::is_trivially_constructible_v<T>);
    static_assert(std::is_trivially_copyable_v<T>);
    uint size = data.size();
    sout.write(reinterpret_cast<const char *>(&size), sizeof(uint));
    sout.write(reinterpret_cast<const char *>(data.data()), size * sizeof(T));
}

template <typename T> static void ReadVec(std::istream &sin, std::vector<T> &data)
{
    static_assert(std::is_trivially_constructible_v<T>);
    static_assert(std::is_trivially_copyable_v<T>);

    uint size = 0;
    sin.read(reinterpret_cast<char *>(&size), sizeof(uint));

    data.resize(size);
    sin.read(reinterpret_cast<char *>(data.data()), sizeof(T));
}

void ModelCPU::SaveToFile(const std::filesystem::path &path) const
{
    std::ofstream fout(path, std::ios::binary);
    WriteVec(fout, Vertices);
    WriteVec(fout, GlobalIndices);
    WriteVec(fout, LocalIndices);
    WriteVec(fout, Meshlets);
    WriteVec(fout, Meshes);
}

void ModelCPU::LoadFromFile(const std::filesystem::path &path)
{
    std::ifstream fin(path, std::ios::binary);
    ReadVec(fin, Vertices);
    ReadVec(fin, GlobalIndices);
    ReadVec(fin, LocalIndices);
    ReadVec(fin, Meshlets);
    ReadVec(fin, Meshes);

    // Восстанавливаем AABB мешлетов, имеет смысл это сразу сделать на процессоре
    MeshletBoxes.resize(Meshlets.size());
    for (uint meshletId = 0; meshletId < Meshlets.size(); ++meshletId)
    {
        using namespace DirectX;
        const MeshletDesc &meshlet = Meshlets[meshletId];
        BoundingBox       &aabb    = MeshletBoxes[meshletId];
        if (meshlet.VertCount <= 0)
        {
            aabb.Min = float3(0.0f, 0.0f, 0.0f);
            aabb.Max = float3(0.0f, 0.0f, 0.0f);
            continue;
        }

        aabb.Min = Vertices[meshlet.VertOffset].Position;
        aabb.Max = aabb.Min;
        for (uint vertId = 1; vertId < meshlet.VertCount; ++vertId)
        {
            const Vertex &vert = Vertices[meshlet.VertOffset + vertId];
            const float3 &pos  = vert.Position;

            aabb.Min.x = XMMin(aabb.Min.x, pos.x);
            aabb.Min.y = XMMin(aabb.Min.y, pos.y);
            aabb.Min.z = XMMin(aabb.Min.z, pos.z);
            aabb.Max.x = XMMax(aabb.Max.x, pos.x);
            aabb.Max.y = XMMax(aabb.Max.y, pos.y);
            aabb.Max.z = XMMax(aabb.Max.z, pos.z);
        }
    }
}
