#include "stdafx.h"

#include "Common.h"

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "tiny_gltf.h"

#include <iostream>

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

void TMonoLodCPU::LoadGLB(const std::string &path)
{
    tinygltf::Model    inModel;
    tinygltf::TinyGLTF tinyGltfCtx;
    std::string        err;
    std::string        warn;
    bool               success = tinyGltfCtx.LoadBinaryFromFile(&inModel, &err, &warn, path);
    if (!err.empty())
        std::cerr << err << '\n';
    if (!warn.empty())
        std::cerr << warn << '\n';
    ASSERT(success);

    ASSERT_EQ(inModel.meshes.size(), 1);
    tinygltf::Mesh &mesh = inModel.meshes[0];

    ASSERT_EQ(mesh.primitives.size(), 1);
    tinygltf::Primitive &primitive = mesh.primitives[0];

    ASSERT_EQ(primitive.mode, TINYGLTF_MODE_TRIANGLES);

    int positionIdx = -1;
    int normalIdx   = -1;
    for (const auto &[name, idx] : primitive.attributes)
    {
        if (name == "POSITION")
        {
            ASSERT_EQ(positionIdx, -1);
            positionIdx = idx;
        }
        else if (name == "NORMAL")
        {
            ASSERT_EQ(normalIdx, -1);
            normalIdx = idx;
        }
    }
    ASSERT(positionIdx != -1);
    ASSERT(normalIdx != -1);

    tinygltf::Accessor   &positionAccessor   = inModel.accessors[positionIdx];
    tinygltf::BufferView &positionBufferView = inModel.bufferViews[positionAccessor.bufferView];
    tinygltf::Buffer     &positionBuffer     = inModel.buffers[positionBufferView.buffer];

    tinygltf::Accessor   &normalAccessor   = inModel.accessors[normalIdx];
    tinygltf::BufferView &normalBufferView = inModel.bufferViews[normalAccessor.bufferView];
    tinygltf::Buffer     &normalBuffer     = inModel.buffers[normalBufferView.buffer];

    size_t positionStride = positionAccessor.ByteStride(positionBufferView);
    size_t normalStride   = normalAccessor.ByteStride(normalBufferView);

    if constexpr (false)
        std::cout << "nPositions = " << positionAccessor.count << std::endl;
    ASSERT_EQ(positionAccessor.type, TINYGLTF_TYPE_VEC3);
    ASSERT_EQ(positionAccessor.componentType, TINYGLTF_COMPONENT_TYPE_FLOAT);
    ASSERT_EQ(normalAccessor.type, TINYGLTF_TYPE_VEC3);
    ASSERT_EQ(normalAccessor.componentType, TINYGLTF_COMPONENT_TYPE_FLOAT);

    Vertices.clear();
    Vertices.reserve(positionAccessor.count);

    for (size_t iVert = 0; iVert < positionAccessor.count; ++iVert)
    {
        size_t offPosition = positionBufferView.byteOffset;
        offPosition += positionAccessor.byteOffset;
        offPosition += positionStride * iVert;

        size_t offNormal = normalBufferView.byteOffset;
        offNormal += normalAccessor.byteOffset;
        offNormal += normalStride * iVert;
        unsigned char *pPosition = &positionBuffer.data[offPosition];
        unsigned char *pNormal   = &normalBuffer.data[offNormal];

        TVertex vert   = {};
        vert.Position = *reinterpret_cast<float3 *>(pPosition);
        vert.Normal   = *reinterpret_cast<float3 *>(pNormal);
        Vertices.push_back(vert);
    }

    tinygltf::Accessor   &indexAccessor   = inModel.accessors[primitive.indices];
    tinygltf::BufferView &indexBufferView = inModel.bufferViews[indexAccessor.bufferView];
    tinygltf::Buffer     &indexBuffer     = inModel.buffers[indexBufferView.buffer];

    unsigned char *indexBytes = &indexBuffer.data[indexBufferView.byteOffset + indexAccessor.byteOffset];

    int indexComponentSize = 0;
    switch (indexAccessor.componentType)
    {
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: indexComponentSize = 1; break;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: indexComponentSize = 2; break;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: indexComponentSize = 4; break;
    default: throw std::runtime_error("Unknown component type");
    }

    ASSERT_EQ(indexComponentSize, indexAccessor.ByteStride(indexBufferView));

    Indices.clear();
    Indices.reserve(indexAccessor.count);

    for (size_t iiVert = 0; iiVert < indexAccessor.count; ++iiVert)
    {
        size_t idxOffset = indexComponentSize * iiVert;
        size_t iVert     = 0;
        // little endian
        for (int iByte = 0; iByte < indexComponentSize; ++iByte)
            iVert |= size_t(indexBytes[idxOffset + iByte]) << (8 * iByte);
        Indices.push_back(iVert);
    }
}

void TMeshletModelCPU::SaveToFile(const std::filesystem::path &path) const
{
    std::ofstream fout(path, std::ios::binary);
    WriteVec(fout, Vertices);
    WriteVec(fout, GlobalIndices);
    WriteVec(fout, Primitives);
    WriteVec(fout, Meshlets);
    WriteVec(fout, Meshes);
}

void TMeshletModelCPU::LoadFromFile(const std::filesystem::path &path)
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
        const TMeshletDesc &meshlet = Meshlets[iMeshlet];
        TBoundingBox       &aabb    = MeshletBoxes[iMeshlet];

        aabb.Min = float3(INFINITY, INFINITY, INFINITY);
        aabb.Max = float3(-INFINITY, -INFINITY, -INFINITY);

        for (uint iMeshletVert = 0; iMeshletVert < meshlet.VertCount; ++iMeshletVert)
        {
            uint          iVert = GlobalIndices[meshlet.VertOffset + iMeshletVert];
            const TVertex &vert  = Vertices[iVert & UINT32_C(0x7FFFFFFF)];
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
        const TMeshletDesc &meshlet = Meshlets[iMeshlet];
        const TBoundingBox &aabb    = MeshletBoxes[iMeshlet];
        // Восстанавливаем AABB родителей
        float maxParentError = 0.0f;
        for (uint iiParent = 0; iiParent < meshlet.ParentCount; ++iiParent)
        {
            uint iParent = meshlet.ParentOffset + iiParent;
            if (iParent <= iMeshlet || iParent >= Meshlets.size())
                throw std::runtime_error("Incorrect Parent1");
            TBoundingBox &aabbParent = MeshletBoxes[iParent];

            aabbParent.Min.x = XMMin(aabbParent.Min.x, aabb.Min.x);
            aabbParent.Min.y = XMMin(aabbParent.Min.y, aabb.Min.y);
            aabbParent.Min.z = XMMin(aabbParent.Min.z, aabb.Min.z);
            aabbParent.Max.x = XMMax(aabbParent.Max.x, aabb.Max.x);
            aabbParent.Max.y = XMMax(aabbParent.Max.y, aabb.Max.y);
            aabbParent.Max.z = XMMax(aabbParent.Max.z, aabb.Max.z);

            Meshlets[iParent].Height = std::max(Meshlets[iParent].Height, meshlet.Height + 1);
            maxParentError           = std::max(maxParentError, Meshlets[iParent].Error);
        }

        maxParentError += meshlet.Error;
        for (uint iiParent = 0; iiParent < meshlet.ParentCount; ++iiParent)
        {
            uint iParent            = meshlet.ParentOffset + iiParent;
            Meshlets[iParent].Error = maxParentError;
        }
    }
}
