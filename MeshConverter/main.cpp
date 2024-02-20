#include <Common.h>

#include <algorithm>
#include <iostream>
#include <map>
#include <vector>

#include <metis.h>
#include <tiny_gltf.h>

using namespace DirectX;

constexpr bool DBGOUT = false;

#if IDXTYPEWIDTH == 32
#define IDX_C(x) INT32_C(x)
#elif IDXTYPEWIDTH == 64
#define IDX_C(x) INT64_C(x)
#else
#error "IDTYPEWIDTH not set"
#endif

using MeshEdge = std::pair<size_t, size_t>;

struct OriginalMesh
{
    std::vector<Vertex> Vertices;
    std::vector<size_t> Indices;
    XMVECTOR            BoxMax = XMVectorZero();
    XMVECTOR            BoxMin = XMVectorZero();

    std::map<MeshEdge, std::vector<size_t>> EdgeTriangles;
    std::vector<idx_t>                      TriangleMeshlet;

    size_t TriangleCount() const noexcept { return Indices.size() / 3; }

    MeshEdge GetEdge(size_t iTriangle, size_t iTriEdge)
    {
        size_t v = Indices[3 * iTriangle + iTriEdge];
        size_t u = Indices[3 * iTriangle + (iTriEdge + 1) % 3];
        if (v > u) std::swap(v, u);
        return {v, u};
    }

    void LoadGLB(const std::string &path)
    {
        tinygltf::Model    inModel;
        tinygltf::TinyGLTF tinyGltfCtx;
        std::string        err;
        std::string        warn;
        bool               success = tinyGltfCtx.LoadBinaryFromFile(&inModel, &err, &warn, path);
        if (!err.empty()) std::cerr << err << '\n';
        if (!warn.empty()) std::cerr << warn << '\n';
        if (!success) throw std::runtime_error("Fail");

        if (inModel.meshes.size() != 1) throw std::runtime_error("Should have one mesh");
        tinygltf::Mesh &mesh = inModel.meshes[0];

        if (mesh.primitives.size() != 1) throw std::runtime_error("Should be one primitive");
        tinygltf::Primitive &primitive = mesh.primitives[0];

        if (primitive.mode != TINYGLTF_MODE_TRIANGLES) throw std::runtime_error("Only triangles are supported");

        int positionIdx = -1;
        int normalIdx   = -1;
        for (auto &&pair : primitive.attributes)
        {
            if (pair.first == "POSITION")
            {
                if (positionIdx != -1) throw std::runtime_error("Two positions found");
                positionIdx = pair.second;
            }
            else if (pair.first == "NORMAL")
            {
                if (normalIdx != -1) throw std::runtime_error("Two normals found");
                normalIdx = pair.second;
            }
        }
        if (positionIdx == -1) throw std::runtime_error("No position found");

        tinygltf::Accessor   &positionAccessor   = inModel.accessors[positionIdx];
        tinygltf::BufferView &positionBufferView = inModel.bufferViews[positionAccessor.bufferView];
        tinygltf::Buffer     &positionBuffer     = inModel.buffers[positionBufferView.buffer];

        tinygltf::Accessor   &normalAccessor   = inModel.accessors[normalIdx];
        tinygltf::BufferView &normalBufferView = inModel.bufferViews[normalAccessor.bufferView];
        tinygltf::Buffer     &normalBuffer     = inModel.buffers[normalBufferView.buffer];

        size_t positionStride = positionAccessor.ByteStride(positionBufferView);
        size_t normalStride   = normalAccessor.ByteStride(normalBufferView);

        if constexpr (DBGOUT) std::cout << "nPositions = " << positionAccessor.count << std::endl;
        if (positionAccessor.type != TINYGLTF_TYPE_VEC3) throw std::runtime_error("Position is not vec3");
        if (positionAccessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
            throw std::runtime_error("Position component type is not float");

        if (normalAccessor.type != TINYGLTF_TYPE_VEC3) throw std::runtime_error("Normal is not vec3");
        if (normalAccessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
            throw std::runtime_error("Normal component type is not float");

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

            Vertex vert   = {};
            vert.Position = *reinterpret_cast<float3 *>(pPosition);
            vert.Normal   = *reinterpret_cast<float3 *>(pNormal);
            Vertices.push_back(vert);

            XMVECTOR pos = XMLoadFloat3(&vert.Position);
            if (iVert == 0)
            {
                BoxMax = pos;
                BoxMin = pos;
            }
            else
            {
                BoxMax = XMVectorMax(BoxMax, pos);
                BoxMin = XMVectorMin(BoxMin, pos);
            }
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

        if (indexComponentSize != indexAccessor.ByteStride(indexBufferView))
            throw std::runtime_error("Non packed indices");

        Indices.reserve(indexAccessor.count);
        for (size_t iiVert = 0; iiVert < indexAccessor.count; ++iiVert)
        {
            size_t iVert = 0;
            // little endian
            for (int indexByteOff = 0; indexByteOff < indexComponentSize; ++indexByteOff)
                iVert |= size_t(indexBytes[indexComponentSize * iiVert + indexByteOff]) << (8 * indexByteOff);
            Indices.push_back(iVert);
        }
    }

    void BuildTriangleEdgeIndex()
    {
        size_t nTriangles = TriangleCount();
        for (size_t iTriangle = 0; iTriangle < nTriangles; ++iTriangle)
        {
            for (size_t iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
            {
                MeshEdge edge              = GetEdge(iTriangle, iTriEdge);
                auto     iterInsertionPair = EdgeTriangles.try_emplace(edge);
                auto    &vec               = iterInsertionPair.first->second;
                if (!vec.empty() && vec[vec.size() - 1] == iTriangle)
                    throw std::runtime_error("Triangle with repeated edge?!");
                vec.push_back(iTriangle);
            }
        }
    }

    void PartitionIntoMeshlets(idx_t nMeshlets)
    {
        size_t nTriangles = TriangleCount();
        TriangleMeshlet.resize(nTriangles, 0);
        if (nMeshlets == 1)
        {
            std::fill(TriangleMeshlet.begin(), TriangleMeshlet.end(), IDX_C(0));
            return;
        }

        idx_t nvtxs = nTriangles;

        idx_t              ncon = 1;
        std::vector<idx_t> xadj;
        xadj.reserve(size_t(nvtxs) + 1);
        xadj.push_back(0);
        std::vector<idx_t> adjncy;
        std::vector<idx_t> adjwgt;

        XMVECTOR maxDiff = BoxMax - BoxMin;
        float    maxLen  = XMVectorGetX(XMVector3Length(maxDiff)) * (1.0f + FLT_EPSILON);

        for (idx_t iTriangle = 0; iTriangle < nTriangles; ++iTriangle)
        {
            idx_t vertexIndices[3] = {};
            for (idx_t iTriVert = 0; iTriVert < 3; ++iTriVert)
                vertexIndices[iTriVert] = Indices[3 * iTriangle + iTriVert];
            for (idx_t iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
            {
                idx_t iVert = vertexIndices[iTriEdge];
                idx_t jVert = vertexIndices[(iTriEdge + 1) % 3];
                if (iVert > jVert) std::swap(iVert, jVert);
                MeshEdge edge = {iVert, jVert};
                auto    &vec  = EdgeTriangles[edge];

                float3   posi  = Vertices[iVert].Position;
                float3   posj  = Vertices[jVert].Position;
                XMVECTOR posiv = XMLoadFloat3(&posi);
                XMVECTOR posjv = XMLoadFloat3(&posj);
                float    len   = XMVectorGetX(XMVector3Length(posjv - posiv));

                for (idx_t iOtherTriangle : vec)
                {
                    if (iOtherTriangle == iTriangle) continue;
                    adjncy.push_back(iOtherTriangle);
                    adjwgt.push_back(idx_t(IDX_C(0x7FFFFFFF) * len / maxLen));
                }
            }
            xadj.push_back(idx_t(adjncy.size()));
        }
        for (idx_t i = nTriangles; i < nvtxs; ++i)
            xadj.push_back(idx_t(adjncy.size()));

        idx_t edgecut = 0;

        if constexpr (DBGOUT)
        {
            std::cout << "\nAdjacency:\n";
            for (idx_t iTriangle = 0; iTriangle < nTriangles; ++iTriangle)
            {
                std::cout << iTriangle << ':';
                idx_t beg = xadj[iTriangle];
                idx_t end = xadj[size_t(iTriangle) + 1];
                for (idx_t i = beg; i < end; ++i)
                    std::cout << ' ' << adjncy[i];
                std::cout << '\n';
            }
        }

        idx_t options[METIS_NOPTIONS] = {};
        METIS_SetDefaultOptions(options);
        options[METIS_OPTION_NUMBERING] = 0;

        std::cout << "Partitioning meshlets...\n";
        // Для разделения кроме первой фазы нужно будет
        // использовать графы
        int metisResult = METIS_PartGraphKway(&nvtxs,
                                              &ncon,
                                              xadj.data(),
                                              adjncy.data(),
                                              nullptr,
                                              nullptr,
                                              adjwgt.data(),
                                              &nMeshlets,
                                              nullptr,
                                              nullptr,
                                              options,
                                              &edgecut,
                                              TriangleMeshlet.data());
        if (metisResult != METIS_OK) throw std::runtime_error("METIS failed");
    }
};

int main()
{
    OriginalMesh ogMesh;

    std::cout << "Loading model...\n";
    ogMesh.LoadGLB("input.glb");
    std::cout << "Loading model done\n";

    size_t nVertices  = ogMesh.Vertices.size();
    size_t nTriangles = ogMesh.TriangleCount();

    std::cout << "Building triangle edge index...\n";
    ogMesh.BuildTriangleEdgeIndex();
    std::cout << "Building triangle edge index done\n";

    if constexpr (DBGOUT)
    {
        std::cout << "\nBy edge:\n";
        for (auto &[edge, tris] : ogMesh.EdgeTriangles)
        {
            std::cout << "V[" << edge.first << ", " << edge.second << "]: T[";
            for (size_t i = 0; i < tris.size(); ++i)
            {
                if (i != 0) std::cout << ", ";
                std::cout << tris[i];
            }
            std::cout << "]\n";
        }
    }

    std::cout << "Preparing METIS structure...\n";
    // Готовим структуру для первого вызова METIS
    constexpr uint TARGET_PRIMITIVES = MESHLET_MAX_PRIMITIVES * 3 / 4;

    idx_t nMeshlets = idx_t((nTriangles + TARGET_PRIMITIVES - 1) / TARGET_PRIMITIVES);
    ogMesh.PartitionIntoMeshlets(nMeshlets);
    std::cout << "Partitioning meshlets done\n";

    if constexpr (DBGOUT)
    {
        std::cout << "\nClusterization:\n";
        for (idx_t iTriangle = 0; iTriangle < nTriangles; ++iTriangle)
            std::cout << "T[" << iTriangle << "] => " << ogMesh.TriangleMeshlet[iTriangle] << '\n';
    }

    std::cout << "Converting out model...\n";
    ModelCPU outModel;

    // Вершины остаются без изменений
    outModel.Vertices = std::move(ogMesh.Vertices);

    // Разбиваем на диапазоны offset[k]..offset[k + 1]
    std::vector<size_t> meshletOffset(nMeshlets + 1, 0);
    for (size_t iTriangle = 0; iTriangle < nTriangles; ++iTriangle)
    {
        idx_t iMeshlet = ogMesh.TriangleMeshlet[iTriangle];
        ++meshletOffset[iMeshlet + 1];
    }
    for (size_t iMeshlet = 0; iMeshlet < nMeshlets; ++iMeshlet)
        meshletOffset[iMeshlet + 1] += meshletOffset[iMeshlet];

    std::vector<size_t> meshletTriangles(nTriangles, 0);
    for (size_t iTriangle = 0; iTriangle < nTriangles; ++iTriangle)
    {
        idx_t  iMeshlet       = ogMesh.TriangleMeshlet[iTriangle];
        size_t off            = meshletOffset[iMeshlet]++;
        meshletTriangles[off] = iTriangle;
    }
    if (meshletOffset[nMeshlets - 1] != meshletOffset[nMeshlets]) throw std::runtime_error("Assert failed");
    for (size_t iMeshlet = nMeshlets; iMeshlet > 0; --iMeshlet)
        meshletOffset[iMeshlet] = meshletOffset[iMeshlet - 1];
    meshletOffset[0] = 0;

    if constexpr (true)
    {
        for (size_t iMeshlet = 0; iMeshlet < nMeshlets; ++iMeshlet)
        {
            size_t meshletSize = meshletOffset[iMeshlet + 1] - meshletOffset[iMeshlet];
            if (meshletSize > MESHLET_MAX_PRIMITIVES)
                std::cout << "Meshlet[" << iMeshlet << "].Size = " << meshletSize << "\n";
        }
    }

    outModel.Meshlets.reserve(nMeshlets);
    outModel.Primitives.reserve(nTriangles);

    // Используем вектор вместо словаря, т.к. ключи --- это индексы вершин
    std::vector<uint> vertLocalIndex(nVertices);

    for (idx_t iMeshlet = 0; iMeshlet < nMeshlets; ++iMeshlet)
    {
        size_t currOffset = meshletOffset[iMeshlet];
        size_t nextOffset = meshletOffset[iMeshlet + 1];

        // Помечаем каждую вершину мешлета как ещё не использованную в этом мешлете
        for (size_t iiTriangle = currOffset; iiTriangle < nextOffset; ++iiTriangle)
        {
            size_t iTriangle = meshletTriangles[iiTriangle];
            for (size_t iTriVert = 0; iTriVert < 3; ++iTriVert)
            {
                size_t iVert          = ogMesh.Indices[3 * iTriangle + iTriVert];
                vertLocalIndex[iVert] = UINT32_MAX;
            }
        }

        MeshletDesc meshlet = {};
        meshlet.VertOffset  = outModel.GlobalIndices.size();
        meshlet.VertCount   = 0;
        meshlet.PrimOffset  = currOffset;
        meshlet.PrimCount   = nextOffset - currOffset;

        for (size_t iiTriangle = currOffset; iiTriangle < nextOffset; ++iiTriangle)
        {
            size_t iTriangle       = meshletTriangles[iiTriangle];
            uint   encodedTriangle = 0;
            for (size_t iTriVert = 0; iTriVert < 3; ++iTriVert)
            {
                size_t iVert    = ogMesh.Indices[3 * iTriangle + iTriVert];
                uint   iLocVert = vertLocalIndex[iVert];
                // Если вершина ещё не использована в этом мешлете,
                // назначим ей новый индекс
                if (iLocVert >= meshlet.VertCount)
                {
                    iLocVert              = meshlet.VertCount++;
                    vertLocalIndex[iVert] = iLocVert;
                    outModel.GlobalIndices.push_back(iVert);
                }
                encodedTriangle |= iLocVert << (10 * iTriVert);
            }
            outModel.Primitives.push_back(encodedTriangle);
        }

        outModel.Meshlets.push_back(meshlet);
    }

    MeshDesc outMesh      = {};
    outMesh.MeshletCount  = nMeshlets;
    outMesh.MeshletOffset = 0;
    outModel.Meshes.push_back(outMesh);

    std::cout << "Saving model...\n";
    outModel.SaveToFile("../MasterThesis/model.bin");
    std::cout << "Saving model done\n";

    if constexpr (DBGOUT)
    {
        std::cout << "\nOut model:\nVertices:\n";
        for (size_t iVert = 0; iVert < outModel.Vertices.size(); ++iVert)
        {
            auto &vert = outModel.Vertices[iVert];
            std::cout << "V[" << iVert << "] = (" << vert.Position.x << ", " << vert.Position.y << ", "
                      << vert.Position.z << ")\n";
        }

        std::cout << "\nGlobal indices:\n";
        for (size_t ii = 0; ii < outModel.GlobalIndices.size(); ++ii)
        {
            uint i = outModel.GlobalIndices[ii];
            std::cout << "GI[" << ii << "] = " << i << "\n";
        }

        std::cout << "\nTriangles:\n";
        for (size_t iTriangle = 0; iTriangle < outModel.Primitives.size(); ++iTriangle)
        {
            uint triCode = outModel.Primitives[iTriangle];
            std::cout << "T[" << iTriangle << "] = L[";
            for (uint iTriVert = 0; iTriVert < 3; ++iTriVert)
            {
                if (iTriVert != 0) std::cout << ", ";
                std::cout << ((triCode >> (10 * iTriVert)) & 0x3FF);
            }
            std::cout << "]\n";
        }

        std::cout << "\nMeshlets:\n";
        for (size_t iMeshlet = 0; iMeshlet < outModel.Meshlets.size(); ++iMeshlet)
        {
            MeshletDesc &meshlet = outModel.Meshlets[iMeshlet];
            std::cout << "M[" << iMeshlet << "] = { VertOffset: " << meshlet.VertOffset
                      << ", VertCount: " << meshlet.VertCount << ", PrimOffset: " << meshlet.PrimOffset
                      << ", PrimCount: " << meshlet.PrimCount << "}\n";
        }

        std::cout << "\nMeshlets unwrapped:\n";
        for (size_t iMeshlet = 0; iMeshlet < outModel.Meshlets.size(); ++iMeshlet)
        {
            std::cout << "M[" << iMeshlet << "]:\n";
            MeshletDesc &meshlet = outModel.Meshlets[iMeshlet];
            for (uint iMeshletTriangle = 0; iMeshletTriangle < meshlet.PrimCount; ++iMeshletTriangle)
            {
                uint iTriangle = meshlet.PrimOffset + iMeshletTriangle;
                std::cout << "    T[" << iTriangle << "] = V[";
                uint triangleCode = outModel.Primitives[iTriangle];
                for (uint iTriVert = 0; iTriVert < 3; ++iTriVert)
                {
                    if (iTriVert != 0) std::cout << ", ";
                    uint iLocVert = (triangleCode >> (10 * iTriVert)) & 0x3FF;
                    uint iVert    = outModel.GlobalIndices[size_t(meshlet.VertOffset) + iLocVert];
                    std::cout << iVert;
                }
                std::cout << "]\n";
            }
        }
    }

    return 0;
}
