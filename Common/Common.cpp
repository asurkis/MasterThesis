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
    sin.read(reinterpret_cast<char *>(data.data()), size * sizeof(T));
}

void ModelCPU::SaveToFile(const std::filesystem::path &path) const
{
    std::ofstream fout(path, std::ios::binary);
    WriteVec(fout, Vertices);
    WriteVec(fout, GlobalIndices);
    WriteVec(fout, Primitives);
    WriteVec(fout, Meshlets);
    WriteVec(fout, Meshes);
}

void ModelCPU::LoadFromFile(const std::filesystem::path &path)
{
    using namespace DirectX;

    std::ifstream fin(path, std::ios::binary);
    size_t        pos1 = fin.tellg();
    ReadVec(fin, Vertices);
    ReadVec(fin, GlobalIndices);
    ReadVec(fin, Primitives);
    ReadVec(fin, Meshlets);
    ReadVec(fin, Meshes);

    // Восстанавливаем AABB мешлетов, имеет смысл это сразу сделать на процессоре
    MeshletBoxes.resize(Meshlets.size());
    for (uint iMeshlet = 0; iMeshlet < Meshlets.size(); ++iMeshlet)
    {
        const MeshletDesc &meshlet = Meshlets[iMeshlet];
        BoundingBox       &aabb    = MeshletBoxes[iMeshlet];

        aabb.Min = float3(INFINITY, INFINITY, INFINITY);
        aabb.Max = float3(-INFINITY, -INFINITY, -INFINITY);

        for (uint iMeshletVert = 0; iMeshletVert < meshlet.VertCount; ++iMeshletVert)
        {
            uint          iVert = GlobalIndices[meshlet.VertOffset + iMeshletVert];
            const Vertex &vert  = Vertices[iVert & UINT32_C(0x7FFFFFFF)];
            const float3 &pos   = vert.Position;

            aabb.Min.x = XMMin(aabb.Min.x, pos.x);
            aabb.Min.y = XMMin(aabb.Min.y, pos.y);
            aabb.Min.z = XMMin(aabb.Min.z, pos.z);
            aabb.Max.x = XMMax(aabb.Max.x, pos.x);
            aabb.Max.y = XMMax(aabb.Max.y, pos.y);
            aabb.Max.z = XMMax(aabb.Max.z, pos.z);
        }
    }

    for (size_t iMeshlet = 0; iMeshlet < Meshlets.size(); ++iMeshlet)
    {
        const MeshletDesc &meshlet = Meshlets[iMeshlet];
        const BoundingBox &aabb    = MeshletBoxes[iMeshlet];
        // Восстанавливаем AABB родителей
        for (uint iiParent = 0; iiParent < meshlet.ParentCount; ++iiParent)
        {
            uint iParent = meshlet.ParentOffset + iiParent;
            if (iParent != 0)
            {
                if (iParent <= iMeshlet || iParent >= Meshlets.size())
                    throw std::runtime_error("Incorrect Parent1");
                BoundingBox &aabbParent = MeshletBoxes[iParent];

                aabbParent.Min.x = XMMin(aabbParent.Min.x, aabb.Min.x);
                aabbParent.Min.y = XMMin(aabbParent.Min.y, aabb.Min.y);
                aabbParent.Min.z = XMMin(aabbParent.Min.z, aabb.Min.z);
                aabbParent.Max.x = XMMax(aabbParent.Max.x, aabb.Max.x);
                aabbParent.Max.y = XMMax(aabbParent.Max.y, aabb.Max.y);
                aabbParent.Max.z = XMMax(aabbParent.Max.z, aabb.Max.z);

                Meshlets[iParent].Height = std::max(Meshlets[iParent].Height, meshlet.Height + 1);
            }
        }
    }
}
