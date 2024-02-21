﻿#include <Common.h>

#include <algorithm>
#include <iostream>
#include <unordered_map>
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

struct MeshEdgeHasher
{
    uint64_t operator()(MeshEdge edge) const noexcept
    {
        return std::hash<uint64_t>{}((edge.first & UINT64_C(0xFFFFFFFF))
                                     | ((edge.second & UINT64_C(0xFFFFFFFF)) << 32));
    }
};

static void GroupWithOffsets(size_t                    nparts,
                             const std::vector<idx_t> &parts,
                             std::vector<size_t>      &offsets,
                             std::vector<size_t>      &grouped)
{
    size_t n = parts.size();
    offsets  = std::vector<size_t>(nparts + 1, 0);
    grouped.resize(n);
    for (size_t i = 0; i < n; ++i)
    {
        size_t iPart = parts[i];
        offsets[iPart + 1]++;
    }
    for (size_t iPart = 1; iPart < nparts; ++iPart)
        offsets[iPart + 1] += offsets[iPart];
    for (size_t i = 0; i < n; ++i)
    {
        size_t iPart = parts[i];
        size_t ii    = offsets[iPart]++;
        grouped[ii]  = i;
    }
    for (size_t iPart = nparts; iPart > 0; --iPart)
        offsets[iPart] = offsets[iPart - 1];
    offsets[0] = 0;
}

struct IntermediateVertex
{
    Vertex m;
    size_t MeshletCount = 0;
    bool   Visited      = false;
};

struct IndexTriangle
{
    size_t idx[3];

    constexpr MeshEdge EdgeKey(size_t iEdge) const
    {
        size_t v = idx[iEdge];
        size_t u = idx[(iEdge + 1) % 3];
        if (v > u)
            std::swap(v, u);
        return {v, u};
    }
};

// T должен быть тривиальным, напр. int
template <typename T, size_t CAPACITY> struct TrivialVector
{
    T      Data[CAPACITY] = {};
    size_t Size           = 0;

    bool IsEmpty() const noexcept { return Size == 0; }

    T Last() const
    {
        if (IsEmpty())
            throw std::runtime_error("Empty");
        return Data[Size - 1];
    }

    bool LastEquals(T x) const noexcept { return !IsEmpty() && Last() == x; }

    void Clear() noexcept { Size = 0; }

    void Push(T x)
    {
        if (Size >= CAPACITY)
            throw std::runtime_error("Overflow");
        Data[Size++] = x;
    }

    T Pop()
    {
        if (IsEmpty())
            throw std::runtime_error("Empty");
        return Data[--Size];
    }

    // Имена в нижнем регистре связаны с синтаксисом for-each в C++
    T       *begin() noexcept { return Data; }
    T       *end() noexcept { return Data + Size; }
    const T *begin() const noexcept { return Data; }
    const T *end() const noexcept { return Data + Size; }
};

struct IntermediateMesh
{
    std::vector<IntermediateVertex> Vertices;
    std::vector<IndexTriangle>      Triangles;
    XMVECTOR                        BoxMax = XMVectorZero();
    XMVECTOR                        BoxMin = XMVectorZero();

    std::unordered_map<MeshEdge, TrivialVector<size_t, 2>, MeshEdgeHasher> EdgeTriangles;

    std::vector<idx_t>  TriangleMeshlet;
    std::vector<size_t> MeshletLayerOffsets;

    std::vector<size_t> MeshletTriangles;
    std::vector<size_t> MeshletTriangleOffsets;

    std::vector<size_t> MeshletParents1;

    std::vector<MeshEdge> MeshletEdges;
    std::vector<size_t>   MeshletEdgeOffsets;

    std::unordered_map<MeshEdge, TrivialVector<size_t, 2>, MeshEdgeHasher> EdgeMeshlets;

    size_t MeshletSize(size_t iMeshlet) const noexcept
    {
        return MeshletTriangleOffsets[iMeshlet + 1] - MeshletTriangleOffsets[iMeshlet];
    }

    size_t LayerMeshletCount(size_t iLayer) const noexcept
    {
        return MeshletLayerOffsets[iLayer + 1] - MeshletLayerOffsets[iLayer];
    }

    void LoadGLB(const std::string &path)
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
        if (!success)
            throw std::runtime_error("Fail");

        if (inModel.meshes.size() != 1)
            throw std::runtime_error("Should have one mesh");
        tinygltf::Mesh &mesh = inModel.meshes[0];

        if (mesh.primitives.size() != 1)
            throw std::runtime_error("Should be one primitive");
        tinygltf::Primitive &primitive = mesh.primitives[0];

        if (primitive.mode != TINYGLTF_MODE_TRIANGLES)
            throw std::runtime_error("Only triangles are supported");

        int positionIdx = -1;
        int normalIdx   = -1;
        for (auto &&pair : primitive.attributes)
        {
            if (pair.first == "POSITION")
            {
                if (positionIdx != -1)
                    throw std::runtime_error("Two positions found");
                positionIdx = pair.second;
            }
            else if (pair.first == "NORMAL")
            {
                if (normalIdx != -1)
                    throw std::runtime_error("Two normals found");
                normalIdx = pair.second;
            }
        }
        if (positionIdx == -1)
            throw std::runtime_error("No position found");

        tinygltf::Accessor   &positionAccessor   = inModel.accessors[positionIdx];
        tinygltf::BufferView &positionBufferView = inModel.bufferViews[positionAccessor.bufferView];
        tinygltf::Buffer     &positionBuffer     = inModel.buffers[positionBufferView.buffer];

        tinygltf::Accessor   &normalAccessor   = inModel.accessors[normalIdx];
        tinygltf::BufferView &normalBufferView = inModel.bufferViews[normalAccessor.bufferView];
        tinygltf::Buffer     &normalBuffer     = inModel.buffers[normalBufferView.buffer];

        size_t positionStride = positionAccessor.ByteStride(positionBufferView);
        size_t normalStride   = normalAccessor.ByteStride(normalBufferView);

        if constexpr (DBGOUT)
            std::cout << "nPositions = " << positionAccessor.count << std::endl;
        if (positionAccessor.type != TINYGLTF_TYPE_VEC3)
            throw std::runtime_error("Position is not vec3");
        if (positionAccessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
            throw std::runtime_error("Position component type is not float");

        if (normalAccessor.type != TINYGLTF_TYPE_VEC3)
            throw std::runtime_error("Normal is not vec3");
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

            IntermediateVertex vert = {};
            vert.m.Position         = *reinterpret_cast<float3 *>(pPosition);
            vert.m.Normal           = *reinterpret_cast<float3 *>(pNormal);
            Vertices.push_back(vert);

            XMVECTOR pos = XMLoadFloat3(&vert.m.Position);
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

        size_t nTriangles = indexAccessor.count / 3;
        Triangles.reserve(nTriangles);
        for (size_t iTriangle = 0; iTriangle < nTriangles; ++iTriangle)
        {
            IndexTriangle tri = {};
            for (size_t iTriVert = 0; iTriVert < 3; ++iTriVert)
            {
                size_t idxOffset = indexComponentSize * (3 * iTriangle + iTriVert);
                size_t iVert     = 0;
                // little endian
                for (int iByte = 0; iByte < indexComponentSize; ++iByte)
                    iVert |= size_t(indexBytes[idxOffset + iByte]) << (8 * iByte);
                tri.idx[iTriVert] = iVert;
            }
            Triangles.push_back(tri);
        }
    }

    void BuildTriangleEdgeIndex()
    {
        size_t nTriangles = Triangles.size();
        for (size_t iTriangle = 0; iTriangle < nTriangles; ++iTriangle)
        {
            for (size_t iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
            {
                MeshEdge edge        = Triangles[iTriangle].EdgeKey(iTriEdge);
                auto [iter, isFirst] = EdgeTriangles.try_emplace(edge);
                auto &vec            = iter->second;
                if (vec.LastEquals(iTriangle))
                    throw std::runtime_error("Triangle with repeated edge?!");
                vec.Push(iTriangle);
            }
        }
    }

    void DoFirstPartition(idx_t nMeshlets)
    {
        MeshletLayerOffsets = {0, size_t(nMeshlets)};

        size_t nTriangles = Triangles.size();
        TriangleMeshlet.resize(nTriangles, 0);
        if (nMeshlets == 1)
        {
            std::fill(TriangleMeshlet.begin(), TriangleMeshlet.end(), IDX_C(0));
            return;
        }

        std::cout << "Preparing METIS structure...\n";
        idx_t nvtxs = nTriangles;
        idx_t ncon  = 1;

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
                vertexIndices[iTriVert] = Triangles[iTriangle].idx[iTriVert];
            for (idx_t iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
            {
                idx_t iVert = vertexIndices[iTriEdge];
                idx_t jVert = vertexIndices[(iTriEdge + 1) % 3];
                if (iVert > jVert)
                    std::swap(iVert, jVert);
                MeshEdge edge = {iVert, jVert};
                auto    &vec  = EdgeTriangles[edge];

                float3   posi   = Vertices[iVert].m.Position;
                float3   posj   = Vertices[jVert].m.Position;
                XMVECTOR posiv  = XMLoadFloat3(&posi);
                XMVECTOR posjv  = XMLoadFloat3(&posj);
                float    len    = XMVectorGetX(XMVector3Length(posjv - posiv));
                idx_t    weight = idx_t(IDX_C(0x7FFFFFFF) * len / maxLen);

                for (size_t jTriangle : vec)
                {
                    if (jTriangle == iTriangle)
                        continue;
                    adjncy.push_back(jTriangle);
                    adjwgt.push_back(weight);
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

        // Для разделения кроме первой фазы нужно будет
        // использовать графы
        int metisResult = METIS_PartGraphKway(&nvtxs,
                                              &ncon,
                                              xadj.data(),
                                              adjncy.data(),
                                              nullptr /* vwgt */,
                                              nullptr /* vsize */,
                                              adjwgt.data(),
                                              &nMeshlets /* nparts */,
                                              nullptr /* tpwgts */,
                                              nullptr /* ubvec */,
                                              options /* options */,
                                              &edgecut /* edgecut */,
                                              TriangleMeshlet.data() /* part */);
        if (metisResult != METIS_OK)
            throw std::runtime_error("METIS failed");

        GroupWithOffsets(nMeshlets, TriangleMeshlet, MeshletTriangleOffsets, MeshletTriangles);
        MeshletParents1 = std::vector<size_t>(nMeshlets, 0);
    }

    void BuildMeshletEdgeIndex(size_t iLayer)
    {
        size_t layerBeg  = MeshletLayerOffsets[iLayer];
        size_t layerEnd  = MeshletLayerOffsets[iLayer + 1];
        size_t nMeshlets = layerEnd - layerBeg;

        MeshletEdges.clear();
        MeshletEdgeOffsets.clear();
        EdgeMeshlets.clear();

        MeshletEdgeOffsets.push_back(0);
        for (size_t iMeshlet = layerBeg; iMeshlet < layerEnd; ++iMeshlet)
        {
            size_t                iiMeshlet  = iMeshlet - layerBeg;
            size_t                meshletBeg = MeshletTriangleOffsets[iMeshlet];
            size_t                meshletEnd = MeshletTriangleOffsets[iMeshlet + 1];
            std::vector<MeshEdge> seenEdges;
            for (size_t iiTriangle = meshletBeg; iiTriangle < meshletEnd; ++iiTriangle)
            {
                size_t iTriangle = MeshletTriangles[iiTriangle];
                for (size_t iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
                {
                    MeshEdge edge        = Triangles[iTriangle].EdgeKey(iTriEdge);
                    auto [iter, isFirst] = EdgeMeshlets.try_emplace(edge);
                    auto &vec            = iter->second;
                    if (!vec.LastEquals(iiMeshlet))
                        vec.Push(iiMeshlet);
                    seenEdges.push_back(edge);
                }
            }

            std::sort(seenEdges.begin(), seenEdges.end());
            seenEdges.erase(std::unique(seenEdges.begin(), seenEdges.end()), seenEdges.end());
            MeshletEdges.insert(MeshletEdges.end(), seenEdges.begin(), seenEdges.end());
            MeshletEdgeOffsets.push_back(MeshletEdges.size());
        }
    }

    void PartitionMeshlets(size_t iLayer)
    {
        size_t layerBeg = MeshletLayerOffsets[iLayer];
        size_t layerEnd = MeshletLayerOffsets[iLayer + 1];

        idx_t              nMeshlets = layerEnd - layerBeg;
        idx_t              nParts    = (nMeshlets + 3) / 4;
        std::vector<idx_t> meshletPart(nMeshlets, 0);

        if (nParts > 1)
        {
            idx_t ncon = 1;

            std::vector<idx_t> xadj;
            xadj.reserve(nMeshlets + 1);
            xadj.push_back(0);

            std::vector<idx_t> adjncy;
            std::vector<idx_t> adjwgt;

            std::vector<idx_t> commonEdgesWith(nMeshlets, 0);
            for (size_t iMeshlet = layerBeg; iMeshlet < layerEnd; ++iMeshlet)
            {
                size_t iiMeshlet = iMeshlet - layerBeg;
                size_t edgesBeg  = MeshletEdgeOffsets[iiMeshlet];
                size_t edgesEnd  = MeshletEdgeOffsets[iiMeshlet];

                // Пометим все смежные мешлеты как непосещённые
                for (size_t iEdge = edgesBeg; iEdge < edgesEnd; ++iEdge)
                {
                    MeshEdge edge = MeshletEdges[iEdge];
                    for (size_t jjMeshlet : EdgeMeshlets[edge])
                    {
                        if (jjMeshlet != iiMeshlet)
                            commonEdgesWith[jjMeshlet] = 0;
                    }
                }

                // Посчитаем веса рёбер как количество смежных рёбер сетки
                for (size_t iEdge = edgesBeg; iEdge < edgesEnd; ++iEdge)
                {
                    MeshEdge edge = MeshletEdges[iEdge];
                    for (size_t jjMeshlet : EdgeMeshlets[edge])
                    {
                        if (jjMeshlet != iiMeshlet)
                            commonEdgesWith[jjMeshlet]++;
                    }
                }

                // Посчитаем веса рёбер как количество смежных рёбер сетки
                for (size_t iEdge = edgesBeg; iEdge < edgesEnd; ++iEdge)
                {
                    MeshEdge edge = MeshletEdges[iEdge];
                    for (size_t jjMeshlet : EdgeMeshlets[edge])
                    {
                        if (jjMeshlet != iiMeshlet && commonEdgesWith[jjMeshlet] != 0)
                        {
                            adjncy.push_back(jjMeshlet);
                            adjwgt.push_back(commonEdgesWith[jjMeshlet]);
                            commonEdgesWith[jjMeshlet] = 0;
                        }
                    }
                }

                xadj.push_back(adjncy.size());
            }

            idx_t options[METIS_NOPTIONS] = {};
            METIS_SetDefaultOptions(options);
            options[METIS_OPTION_NUMBERING] = 0;

            idx_t edgecut = 0;

            int metisResult = METIS_PartGraphKway(&nMeshlets /* nvtxs */,
                                                  &ncon /* ncon */,
                                                  xadj.data(),
                                                  adjncy.data(),
                                                  nullptr /* vwgt */,
                                                  nullptr /* vsize */,
                                                  adjwgt.data(),
                                                  &nParts,
                                                  nullptr /* tpwgts */,
                                                  nullptr /* ubvec */,
                                                  options,
                                                  &edgecut,
                                                  meshletPart.data() /* part */);
            if (metisResult != METIS_OK)
                throw std::runtime_error("METIS failed");
        }

        std::vector<size_t> partOffsets;
        std::vector<size_t> partMeshlets;
        GroupWithOffsets(nParts, meshletPart, partOffsets, partMeshlets);

        for (size_t iMeshlet = layerBeg; iMeshlet < layerEnd; ++iMeshlet)
        {
            size_t iiMeshlet          = iMeshlet - layerBeg;
            size_t iPart              = meshletPart[iiMeshlet];
            MeshletParents1[iMeshlet] = layerEnd + iPart;
        }

        for (size_t iPart = 0; iPart < nParts; ++iPart)
        {
            // TODO: Properly decimate parent
            // Сейчас просто берём часть треугольников от каждого из детей
            size_t partBeg  = partOffsets[iPart];
            size_t partEnd  = partOffsets[iPart + 1];
            size_t partSize = partEnd - partBeg;
            for (size_t iiiTriangle = 0; iiiTriangle < MESHLET_MAX_PRIMITIVES; ++iiiTriangle)
            {
                size_t iMeshlet   = partMeshlets[partBeg + iiiTriangle % partSize];
                size_t meshletBeg = MeshletTriangleOffsets[iMeshlet];
                size_t meshletEnd = MeshletTriangleOffsets[iMeshlet + 1];
                size_t iiTriangle = meshletBeg + iiiTriangle / partSize;
                if (iiTriangle >= meshletEnd)
                    continue;
                size_t iTriangle = MeshletTriangles[iiTriangle];
                MeshletTriangles.push_back(iTriangle);
            }
            MeshletTriangleOffsets.push_back(MeshletTriangles.size());
            MeshletParents1.push_back(0);
        }

        MeshletLayerOffsets.push_back(layerEnd + nParts);
    }

    void DecimateMeshlet(std::vector<IndexTriangle> triangles) {}

    void ConvertModel(ModelCPU &outModel)
    {
        size_t nMeshlets  = MeshletLayerOffsets[MeshletLayerOffsets.size() - 1];
        size_t nTriangles = Triangles.size();
        outModel.Meshlets.reserve(nMeshlets);
        outModel.Primitives.reserve(nTriangles);

        // Используем вектор вместо словаря, т.к. ключи --- это индексы вершин
        std::vector<uint> vertLocalIndex(Vertices.size());

        for (idx_t iMeshlet = 0; iMeshlet < nMeshlets; ++iMeshlet)
        {
            size_t meshletBeg = MeshletTriangleOffsets[iMeshlet];
            size_t meshletEnd = MeshletTriangleOffsets[iMeshlet + 1];

            // Помечаем каждую вершину мешлета как ещё не использованную в этом мешлете
            for (size_t iiTriangle = meshletBeg; iiTriangle < meshletEnd; ++iiTriangle)
            {
                size_t iTriangle = MeshletTriangles[iiTriangle];
                for (size_t iTriVert = 0; iTriVert < 3; ++iTriVert)
                {
                    size_t iVert          = Triangles[iTriangle].idx[iTriVert];
                    vertLocalIndex[iVert] = UINT32_MAX;
                }
            }

            MeshletDesc meshlet = {};
            meshlet.VertOffset  = outModel.GlobalIndices.size();
            meshlet.VertCount   = 0;
            meshlet.PrimOffset  = meshletBeg;
            meshlet.PrimCount   = meshletEnd - meshletBeg;
            meshlet.Parent1     = MeshletParents1[iMeshlet];

            for (size_t iiTriangle = meshletBeg; iiTriangle < meshletEnd; ++iiTriangle)
            {
                size_t iTriangle       = MeshletTriangles[iiTriangle];
                uint   encodedTriangle = 0;
                for (size_t iTriVert = 0; iTriVert < 3; ++iTriVert)
                {
                    size_t iVert    = Triangles[iTriangle].idx[iTriVert];
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

        MeshDesc outMesh               = {};
        outMesh.MeshletCount           = nMeshlets;
        outMesh.MeshletTriangleOffsets = 0;
        outModel.Meshes.push_back(outMesh);

        // Вершины без дополнительной информации
        outModel.Vertices.resize(Vertices.size());
        for (size_t iVert = 0; iVert < Vertices.size(); ++iVert)
            outModel.Vertices[iVert] = Vertices[iVert].m;
    }
};

int main()
{
    IntermediateMesh mesh;

    std::cout << "Loading model...\n";
    mesh.LoadGLB("input.glb");
    std::cout << "Loading model done\n";

    size_t nVertices  = mesh.Vertices.size();
    size_t nTriangles = mesh.Triangles.size();

    std::cout << "Building triangle edge index...\n";
    mesh.BuildTriangleEdgeIndex();
    std::cout << "Building triangle edge index done\n";

    if constexpr (DBGOUT)
    {
        std::cout << "\nBy edge:\n";
        for (auto &[edge, tris] : mesh.EdgeTriangles)
        {
            std::cout << "V[" << edge.first << ", " << edge.second << "]: T[";
            for (size_t i = 0; i < tris.Size; ++i)
            {
                if (i != 0)
                    std::cout << ", ";
                std::cout << tris.Data[i];
            }
            std::cout << "]\n";
        }
    }

    constexpr uint TARGET_PRIMITIVES = MESHLET_MAX_PRIMITIVES * 3 / 4;
    idx_t          nMeshlets         = idx_t((nTriangles + TARGET_PRIMITIVES - 1) / TARGET_PRIMITIVES);

    std::cout << "Partitioning meshlets...\n";
    mesh.DoFirstPartition(nMeshlets);
    std::cout << "Partitioning meshlets done\n";

    if constexpr (DBGOUT)
    {
        std::cout << "\nClusterization:\n";
        for (idx_t iTriangle = 0; iTriangle < nTriangles; ++iTriangle)
            std::cout << "T[" << iTriangle << "] => " << mesh.TriangleMeshlet[iTriangle] << '\n';
    }

    for (size_t iLayer = 0; mesh.LayerMeshletCount(iLayer) > 1; ++iLayer)
    {
        std::cout << "Building meshlet-edge index...\n";
        mesh.BuildMeshletEdgeIndex(iLayer);
        std::cout << "Building meshlet-edge index done\n";

        std::cout << "Partitioning meshlet graph...\n";
        mesh.PartitionMeshlets(iLayer);
        std::cout << "Partitioning meshlet graph done\n";
    }

    ModelCPU outModel;

    std::cout << "Converting out model...\n";
    mesh.ConvertModel(outModel);
    std::cout << "Converting out model done\n";

    // Предупреждаем о нарушениях контракта
    if constexpr (true)
    {
        for (size_t iMeshlet = 0; iMeshlet < nMeshlets; ++iMeshlet)
        {
            size_t meshletSize = mesh.MeshletSize(iMeshlet);
            if (meshletSize > MESHLET_MAX_PRIMITIVES)
                std::cout << "Meshlet[" << iMeshlet << "].Size = " << meshletSize << "\n";
        }
    }

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
                if (iTriVert != 0)
                    std::cout << ", ";
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
                    if (iTriVert != 0)
                        std::cout << ", ";
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
