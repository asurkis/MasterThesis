#include <Common.h>

#include "Util.h"

#include <algorithm>
#include <array>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <metis.h>
#include <tiny_gltf.h>

using namespace DirectX;

#if IDXTYPEWIDTH == 32
#define IDX_C(x) INT32_C(x)
#elif IDXTYPEWIDTH == 64
#define IDX_C(x) INT64_C(x)
#else
#error "IDTYPEWIDTH not set"
#endif

using MeshEdge = std::pair<size_t, size_t>;

template <> struct std::hash<MeshEdge>
{
    size_t operator()(const MeshEdge &edge) const noexcept
    {
        return std::hash<uint64_t>{}((edge.first & UINT64_C(0xFFFFFFFF))
                                     | ((edge.second & UINT64_C(0xFFFFFFFF)) << 32));
    }
};

struct IntermediateVertex
{
    Vertex   m             = {};
    XMMATRIX Quadric       = XMMatrixIdentity();
    size_t   OtherIndex    = 0;
    size_t   TriangleCount = 0;
    bool     Visited       = false;
    bool     IsBorder      = false;
};

struct IntermediateTriangle
{
    std::array<size_t, 3> idx       = {};
    std::array<float, 4>  Error     = {};
    XMVECTOR              Normal    = XMVectorZero();
    bool                  IsDeleted = false;

    constexpr MeshEdge EdgeKey(size_t iEdge) const
    {
        size_t v = idx[iEdge];
        size_t u = idx[(iEdge + 1) % 3];
        if (v > u)
            std::swap(v, u);
        return {v, u};
    }

    std::array<size_t, 3> SortedIndices() const noexcept
    {
        std::array<size_t, 3> res = idx;
        std::sort(res.begin(), res.end());
        return res;
    }

    bool operator==(const IntermediateTriangle &rhs) const noexcept { return SortedIndices() == rhs.SortedIndices(); }
    bool operator<(const IntermediateTriangle &rhs) const noexcept { return SortedIndices() < rhs.SortedIndices(); }
};

constexpr size_t TARGET_PRIMITIVES = MESHLET_MAX_PRIMITIVES * 3 / 4;

struct IntermediateMeshlet
{
    std::vector<IntermediateVertex>        Vertices;
    std::vector<IntermediateTriangle>      Triangles;
    std::vector<size_t>                    VertexCluster;
    SplitVector<std::pair<size_t, size_t>> ClusterTriangles;
    std::unordered_set<MeshEdge>           dbgUsedEdges;

    Slice<std::pair<size_t, size_t>> VertexTriangles(size_t iVert)
    {
        ASSERT(iVert < VertexCluster.size());
        size_t iCluster = VertexCluster[iVert];
        ASSERT(iCluster < ClusterTriangles.PartCount());
        return ClusterTriangles[iCluster];
    }

    void Init(Slice<IntermediateVertex>          globalVertices,
              SplitVector<IntermediateTriangle> &globalTriangles,
              Slice<size_t>                      baseMeshlets,
              size_t                             layerBeg)
    {
        Vertices.clear();
        Triangles.clear();

        // Помечаем вершины для сбора, собираем треугольники
        for (size_t iiMeshlet : baseMeshlets)
        {
            size_t iMeshlet = layerBeg + iiMeshlet;
            for (const IntermediateTriangle &tri : globalTriangles[iMeshlet])
            {
                for (size_t iVert : tri.idx)
                    globalVertices[iVert].Visited = false;
                Triangles.push_back(tri);
            }
        }

        // Собираем вершины, а также преобразовываем индексы к локальным
        for (IntermediateTriangle &tri : Triangles)
        {
            tri.Error = {};

            for (size_t &iVert : tri.idx)
            {
                size_t              iLocVert = globalVertices[iVert].OtherIndex;
                IntermediateVertex &globVert = globalVertices[iVert];
                if (!globVert.Visited)
                {
                    globVert.Visited        = true;
                    iLocVert                = Vertices.size();
                    globVert.OtherIndex     = iLocVert;
                    IntermediateVertex vert = globVert;
                    vert.TriangleCount      = 0;
                    vert.IsBorder           = false;
                    vert.OtherIndex         = iVert;
                    vert.Visited            = true;
                    Vertices.push_back(vert);
                }
                iVert = iLocVert;
                Vertices[iLocVert].TriangleCount++;
            }
        }
        MarkBorderVertices();
        BuildVertexTriangleIndex();
    }

    void MarkBorderVertices()
    {
        std::unordered_map<MeshEdge, size_t> edgeTriangleCount;
        for (const IntermediateTriangle &tri : Triangles)
        {
            for (size_t iVert : tri.idx)
                Vertices[iVert].IsBorder = false;
            for (size_t iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
            {
                MeshEdge edge        = tri.EdgeKey(iTriEdge);
                auto [iter, isFirst] = edgeTriangleCount.try_emplace(edge, 0);
                iter->second++;
            }
        }

        // Определяем граничные вершины
        for (const auto &[edge, count] : edgeTriangleCount)
        {
            if (count != 1)
                continue;
            Vertices[edge.first].IsBorder  = true;
            Vertices[edge.second].IsBorder = true;
            dbgUsedEdges.insert(edge);
        }
    }

    void BuildVertexTriangleIndex()
    {
        ClusterTriangles.Clear();
        ClusterTriangles.FillStart(Vertices.size(), 3 * Triangles.size());
        for (IntermediateTriangle &tri : Triangles)
        {
            for (size_t iVert : tri.idx)
                ClusterTriangles.FillReserve(iVert);
        }
        ClusterTriangles.FillPreparePush();
        for (size_t iTriangle = 0; iTriangle < Triangles.size(); ++iTriangle)
        {
            for (size_t iTriVert = 0; iTriVert < 3; ++iTriVert)
            {
                size_t iVert = Triangles[iTriangle].idx[iTriVert];
                ClusterTriangles.FillPush(iVert, {iTriangle, iTriVert});
            }
        }
        ClusterTriangles.FillCommit();

        VertexCluster.resize(Vertices.size());
        for (size_t iVert = 0; iVert < Vertices.size(); ++iVert)
            VertexCluster[iVert] = iVert;
    }

    void Decimate()
    {
        size_t            nDeletedTriangles = 0;
        std::vector<bool> deleted1;
        std::vector<bool> deleted2;
        std::vector<bool> deleted1Best;
        std::vector<bool> deleted2Best;

        InitQuadrics();

        size_t nMerged = 0;
        dbgSaveAsObj(nMerged);
        while (Triangles.size() - nDeletedTriangles > 2 * TARGET_PRIMITIVES)
        {
            if (4 * nDeletedTriangles >= Triangles.size())
            {
                RemoveDeletedTriangles();
                InitQuadrics();
                nDeletedTriangles = 0;
            }

            size_t   iVertBest = 0;
            size_t   jVertBest = 0;
            XMVECTOR midBest   = {};
            float    errBest   = 0.0f;
            bool     foundBest = false;

            for (const IntermediateTriangle &tri : Triangles)
            {
                if (tri.IsDeleted)
                    continue;
                for (size_t iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
                {
                    size_t              iVert = tri.idx[iTriEdge];
                    size_t              jVert = tri.idx[(iTriEdge + 1) % 3];
                    IntermediateVertex &vert1 = Vertices[iVert];
                    IntermediateVertex &vert2 = Vertices[jVert];
                    if (vert1.IsBorder && vert2.IsBorder)
                        continue;

                    XMVECTOR mid = {};
                    float    err = CalculateError(iVert, jVert, mid);
                    deleted1.resize(VertexTriangles(iVert).Size());
                    std::fill(deleted1.begin(), deleted1.end(), false);
                    if (Flipped(mid, iVert, jVert, deleted1))
                        continue;
                    deleted2.resize(VertexTriangles(jVert).Size());
                    std::fill(deleted2.begin(), deleted2.end(), false);
                    if (Flipped(mid, jVert, iVert, deleted2))
                        continue;

                    if (foundBest && err >= errBest)
                        continue;

                    deleted1Best.swap(deleted1);
                    deleted2Best.swap(deleted2);
                    iVertBest = iVert;
                    jVertBest = jVert;
                    midBest   = mid;
                    errBest   = err;
                    foundBest = true;
                }
            }

            if (!foundBest)
                break;

            IntermediateVertex &vert1 = Vertices[iVertBest];
            IntermediateVertex &vert2 = Vertices[jVertBest];
            if (vert1.IsBorder)
            {
                vert1.Quadric += vert2.Quadric;
                GatherTriangles(iVertBest, iVertBest, nDeletedTriangles, deleted1Best);
                GatherTriangles(iVertBest, jVertBest, nDeletedTriangles, deleted2Best);
                VertexCluster[iVertBest] = ClusterTriangles.PartCount();
                ClusterTriangles.PushSplit();
                dbgSaveAsObj(++nMerged);
                continue;
            }
            if (vert2.IsBorder)
            {
                vert2.Quadric += vert1.Quadric;
                GatherTriangles(jVertBest, iVertBest, nDeletedTriangles, deleted1Best);
                GatherTriangles(jVertBest, jVertBest, nDeletedTriangles, deleted2Best);
                VertexCluster[jVertBest] = ClusterTriangles.PartCount();
                ClusterTriangles.PushSplit();
                dbgSaveAsObj(++nMerged);
                continue;
            }
            XMStoreFloat3(&vert1.m.Position, midBest);
            vert1.Quadric += vert2.Quadric;
            vert1.OtherIndex = UINT32_MAX;
            vert1.Visited    = false;
            GatherTriangles(iVertBest, iVertBest, nDeletedTriangles, deleted1Best);
            GatherTriangles(iVertBest, jVertBest, nDeletedTriangles, deleted2Best);
            VertexCluster[iVertBest] = ClusterTriangles.PartCount();
            ClusterTriangles.PushSplit();
            dbgSaveAsObj(++nMerged);
        }

        // TODO: найти другой метод фильтрации дублирующихся треугольников
        std::set<IntermediateTriangle> resultTrianglesSet;

        for (size_t iTriangle = 0; iTriangle < Triangles.size(); ++iTriangle)
        {
            const IntermediateTriangle &tri = Triangles[iTriangle];
            for (size_t iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
            {
                size_t iVert = tri.idx[iTriEdge];
                size_t jVert = tri.idx[(iTriEdge + 1) % 3];
                if (iVert == jVert)
                    Triangles[iTriangle].IsDeleted = true;
            }

            if (Triangles[iTriangle].IsDeleted)
                continue;

            for (size_t iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
            {
                MeshEdge edge = tri.EdgeKey(iTriEdge);
                dbgUsedEdges.erase(edge);
            }

            resultTrianglesSet.insert(tri);
        }

        Triangles = std::vector(resultTrianglesSet.begin(), resultTrianglesSet.end());
        ASSERT(dbgUsedEdges.empty());

        RemoveDeletedTriangles();
        RestoreNormals();
    }

    void RestoreNormals()
    {
        for (const IntermediateTriangle &tri : Triangles)
        {
            for (size_t iVert : tri.idx)
            {
                IntermediateVertex &vert = Vertices[iVert];
                if (vert.IsBorder)
                    continue;
                vert.m.Normal = float3(0.0f, 0.0f, 0.0f);
            }
        }
        for (IntermediateTriangle &tri : Triangles)
        {
            XMVECTOR p[3] = {};
            for (size_t iTriVert = 0; iTriVert < 3; ++iTriVert)
                p[iTriVert] = XMLoadFloat3(&Vertices[tri.idx[iTriVert]].m.Position);
            XMVECTOR norm = XMVector3Normalize(XMVector3Cross(p[1] - p[0], p[2] - p[0]));
            for (size_t iTriVert = 0; iTriVert < 3; ++iTriVert)
            {
                IntermediateVertex &vert = Vertices[tri.idx[iTriVert]];
                XMVECTOR            acc  = XMLoadFloat3(&vert.m.Normal);
                XMStoreFloat3(&vert.m.Normal, acc + norm);
            }
        }
        for (size_t iVert = 0; iVert < Vertices.size(); ++iVert)
        {
            IntermediateVertex &vert = Vertices[iVert];
            if (vert.IsBorder)
                continue;
            XMVECTOR norm = XMLoadFloat3(&vert.m.Normal);
            norm          = XMVector3Normalize(norm);
            XMStoreFloat3(&vert.m.Normal, norm);
        }
    }

    float CalculateError(size_t iVert, size_t jVert, XMVECTOR &out)
    {
        IntermediateVertex &vert1 = Vertices[iVert];
        IntermediateVertex &vert2 = Vertices[jVert];
        XMMATRIX            q     = vert1.Quadric + vert2.Quadric;
        XMVECTOR            p1    = XMLoadFloat3(&vert1.m.Position);
        XMVECTOR            p2    = XMLoadFloat3(&vert2.m.Position);

        // Если мы на границе, то не имеем права двигать вершину
        if (vert1.IsBorder)
        {
            out = XMLoadFloat3(&vert1.m.Position);
            return VertexError(q, out);
        }
        if (vert2.IsBorder)
        {
            out = XMLoadFloat3(&vert2.m.Position);
            return VertexError(q, out);
        }

        /*
        XMVECTOR det  = XMVectorZero();
        XMMATRIX qInv = XMMatrixInverse(&det, q);
        if (fabs(XMVectorGetX(det)) >= 0.001)
        {
            out = XMVector4Transform(XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f), qInv);
            return VertexError(q, out);
        }
        */

        XMVECTOR p3   = 0.5 * (p1 + p2);
        float    err1 = VertexError(q, p1);
        float    err2 = VertexError(q, p2);
        float    err3 = VertexError(q, p3);
        /*
        if (err1 <= err2 && err1 <= err3)
        {
            out = p1;
            return err1;
        }
        if (err2 <= err1 && err2 <= err3)
        {
            out = p2;
            return err2;
        }
        out = p3;
        return err3;
        */
        if (err1 <= err2)
        {
            out = p1;
            return err1;
        }
        else
        {
            out = p2;
            return err2;
        }
    }

    float VertexError(const XMMATRIX &q, XMVECTOR v)
    {
        v          = XMVectorSetW(v, 1.0f);
        XMVECTOR u = XMVector3Transform(v, q);
        return XMVectorGetX(XMVector4Dot(v, u));
    }

    void GatherTriangles(size_t iNewVert, size_t iVert, size_t &nDeletedTriangles, const std::vector<bool> &deleted)
    {
        XMVECTOR p           = {};
        size_t   iCluster    = VertexCluster[iVert];
        size_t   vertTrisBeg = ClusterTriangles.Split(iCluster);
        size_t   vertTrisEnd = ClusterTriangles.Split(iCluster + 1);
        // Нельзя использовать срез, т.к. меняем вектор
        for (size_t iiTriangle = vertTrisBeg; iiTriangle < vertTrisEnd; ++iiTriangle)
        {
            const auto &[iTriangle, iTriVert] = ClusterTriangles.Flat(iiTriangle);
            IntermediateTriangle &tri         = Triangles[iTriangle];
            if (tri.IsDeleted)
                continue;
            if (deleted[iiTriangle - vertTrisBeg])
            {
                if (!tri.IsDeleted)
                    nDeletedTriangles++;
                tri.IsDeleted = true;
                continue;
            }
            ASSERT_EQ(tri.idx[iTriVert], iVert);
            tri.idx[iTriVert] = iNewVert;
            tri.Error[0]      = CalculateError(tri.idx[0], tri.idx[1], p);
            tri.Error[1]      = CalculateError(tri.idx[1], tri.idx[2], p);
            tri.Error[2]      = CalculateError(tri.idx[2], tri.idx[0], p);
            tri.Error[0]      = XMMin(tri.Error[0], XMMin(tri.Error[1], tri.Error[2]));
            ClusterTriangles.Push({iTriangle, iTriVert});
        }
        for (const IntermediateTriangle &tri : Triangles)
        {
            if (tri.IsDeleted)
                continue;
            for (size_t jVert : tri.idx)
                ASSERT(jVert == iNewVert || jVert != iVert);
        }
    }

    bool Flipped(XMVECTOR p, size_t iVert, size_t jVert, std::vector<bool> &deleted)
    {
        size_t                           nBorder = 0;
        IntermediateVertex              &vert1   = Vertices[iVert];
        IntermediateVertex              &vert2   = Vertices[jVert];
        Slice<std::pair<size_t, size_t>> refs    = VertexTriangles(iVert);

        XMVECTOR pa = XMLoadFloat3(&Vertices[iVert].m.Position);

        for (size_t iiiTriangle = 0; iiiTriangle < refs.Size(); ++iiiTriangle)
        {
            const auto &[iTriangle, iTriVert] = refs[iiiTriangle];
            IntermediateTriangle &tri         = Triangles[iTriangle];
            if (tri.IsDeleted)
                continue;
            size_t iVert1 = tri.idx[(iTriVert + 1) % 3];
            size_t iVert2 = tri.idx[(iTriVert + 2) % 3];
            if (iVert1 == jVert || iVert2 == jVert)
            {
                deleted[iiiTriangle] = true;
                continue;
            }

            XMVECTOR pb      = XMLoadFloat3(&Vertices[iVert1].m.Position);
            XMVECTOR pc      = XMLoadFloat3(&Vertices[iVert2].m.Position);
            XMVECTOR abOld   = pb - pa;
            XMVECTOR acOld   = pc - pa;
            XMVECTOR abNew   = pb - p;
            XMVECTOR acNew   = pc - p;
            XMVECTOR nabOld  = XMVector3Normalize(abOld);
            XMVECTOR nacOld  = XMVector3Normalize(acOld);
            XMVECTOR nabNew  = XMVector3Normalize(abNew);
            XMVECTOR nacNew  = XMVector3Normalize(acNew);
            XMVECTOR normOld = XMVector3Cross(abOld, acOld);
            XMVECTOR normNew = XMVector3Cross(abNew, acNew);
            // if (XMVectorGetX(XMVector3Dot(normNew, normOld)) < 0.0)
            //     return true;
            // if (fabs(XMVectorGetX(XMVector3Dot(nabNew, nacNew))) > 0.999)
            //     return true;
            tri.Normal = normNew;
        }
        return false;
    }

    void RemoveDeletedTriangles()
    {
        Triangles.erase(std::remove_if(Triangles.begin(),
                                       Triangles.end(),
                                       [](const IntermediateTriangle &tri) { return tri.IsDeleted; }),
                        Triangles.end());
        BuildVertexTriangleIndex();
    }

    void InitQuadrics()
    {
        for (IntermediateVertex &vert : Vertices)
        {
            vert.Quadric = XMMatrixSet(0.0f,
                                       0.0f,
                                       0.0f,
                                       0.0f,
                                       0.0f,
                                       0.0f,
                                       0.0f,
                                       0.0f,
                                       0.0f,
                                       0.0f,
                                       0.0f,
                                       0.0f,
                                       0.0f,
                                       0.0f,
                                       0.0f,
                                       0.0f);
        }
        for (IntermediateTriangle &tri : Triangles)
        {
            XMVECTOR p[3] = {};
            for (size_t iTriVert = 0; iTriVert < 3; ++iTriVert)
            {
                size_t              iVert = tri.idx[iTriVert];
                IntermediateVertex &vert  = Vertices[iVert];
                p[iTriVert]               = XMLoadFloat3(&vert.m.Position);
            }
            tri.Normal    = XMVector3Normalize(XMVector3Cross(p[1] - p[0], p[2] - p[0]));
            float    off  = -XMVectorGetX(XMVector3Dot(tri.Normal, p[0]));
            XMVECTOR v    = XMVectorSetW(tri.Normal, off);
            XMMATRIX prod = XMMatrixVectorTensorProduct(v, v);
            for (size_t iVert : tri.idx)
            {
                IntermediateVertex &vert = Vertices[iVert];
                vert.Quadric += prod;
            }
        }
        for (IntermediateTriangle &tri : Triangles)
        {
            XMVECTOR p = {};
            for (size_t iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
            {
                size_t iVert        = tri.idx[iTriEdge];
                size_t jVert        = tri.idx[(iTriEdge + 1) % 3];
                tri.Error[iTriEdge] = CalculateError(iVert, jVert, p);
            }
            tri.Error[3] = XMMin(tri.Error[0], XMMin(tri.Error[1], tri.Error[2]));
        }
    }

    void dbgSaveAsObj(size_t iteration, bool overrideDebug = true)
    {
        if (!overrideDebug)
            return;
        std::ostringstream ossFilename;
        ossFilename << "dbg/dbg" << std::setfill('0') << std::setw(4) << iteration << ".obj";
        std::ofstream fout(ossFilename.str());
        for (IntermediateVertex &v : Vertices)
            fout << "v " << v.m.Position.x << " " << v.m.Position.y << " " << v.m.Position.z << "\n";
        for (IntermediateVertex &v : Vertices)
        {
            if (v.IsBorder)
                fout << "vt 1\n";
            else
                fout << "vt 0\n";
        }
        for (IntermediateTriangle &tri : Triangles)
        {
            if (tri.IsDeleted)
                continue;
            fout << "f";
            for (size_t iVert : tri.idx)
                fout << " " << iVert + 1 << "/" << iVert + 1;
            fout << "\n";
        }
    }
};

struct IntermediateMesh
{
    template <size_t N> using EdgeIndicesMap = std::unordered_map<MeshEdge, std::vector<size_t>>;

    std::vector<IntermediateVertex>   Vertices;
    std::vector<IntermediateTriangle> Triangles;
    XMVECTOR                          BoxMax = XMVectorZero();
    XMVECTOR                          BoxMin = XMVectorZero();

    std::vector<size_t> MeshletLayerOffsets;

    SplitVector<IntermediateTriangle> MeshletTriangles;
    std::vector<size_t>               MeshletParentOffset;
    std::vector<size_t>               MeshletParentCount;

    SplitVector<MeshEdge> MeshletEdges;
    EdgeIndicesMap<2>     EdgeMeshlets;

    std::vector<size_t> dbgVertexMeshletCount;

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

            IntermediateVertex vert;
            vert.m.Position = *reinterpret_cast<float3 *>(pPosition);
            vert.m.Normal   = *reinterpret_cast<float3 *>(pNormal);
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

        size_t nTriangles = indexAccessor.count / 3;
        Triangles.reserve(nTriangles);
        for (size_t iTriangle = 0; iTriangle < nTriangles; ++iTriangle)
        {
            IntermediateTriangle tri;
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

        InitBBox();
    }

    void MakePlane(size_t n)
    {
        Vertices.clear();
        Triangles.clear();
        Vertices.reserve(n * n);
        Triangles.reserve((n - 1) * (n - 1));
        for (size_t z = 0; z < n; ++z)
        {
            for (size_t x = 0; x < n; ++x)
            {
                IntermediateVertex vert = {};
                vert.m.Position.x       = 2.0f * x / (n - 1) - 1.0f;
                vert.m.Position.y       = 0.0f;
                vert.m.Position.z       = 2.0f * z / (n - 1) - 1.0f;
                vert.m.Normal.x         = 0.0f;
                vert.m.Normal.y         = 1.0f;
                vert.m.Normal.z         = 0.0f;
                Vertices.push_back(vert);
            }
        }
        for (size_t z = 0; z + 1 < n; ++z)
        {
            for (size_t x = 0; x + 1 < n; ++x)
            {
                IntermediateTriangle tri = {};

                tri.idx[0] = n * z + x;
                tri.idx[1] = n * z + x + 1;
                tri.idx[2] = n * z + x + n;
                Triangles.push_back(tri);

                tri.idx[0] = n * z + x + n + 1;
                tri.idx[1] = n * z + x + n;
                tri.idx[2] = n * z + x + 1;
                Triangles.push_back(tri);
            }
        }
        InitBBox();
    }

    void MakeSphere(size_t nParallels, size_t nMeridians)
    {
        Vertices.clear();
        Triangles.clear();
        Vertices.reserve(nParallels * nMeridians + 2);
        Triangles.reserve((nParallels + 2) * nMeridians);
        IntermediateVertex   vert = {};
        IntermediateTriangle tri  = {};
        for (size_t iMeridian = 0; iMeridian < nMeridians; ++iMeridian)
        {
            size_t jMeridian = (iMeridian + 1) % nMeridians;
            float  yaw       = XM_2PI * iMeridian / nMeridians;
            float  yawSin    = 0.0f;
            float  yawCos    = 0.0f;
            XMScalarSinCos(&yawSin, &yawCos, yaw);
            for (size_t iParallel = 0; iParallel < nParallels; ++iParallel)
            {
                float pitch    = XM_PI * (iParallel + 1) / (nParallels + 1);
                float pitchSin = 0.0f;
                float pitchCos = 0.0f;
                XMScalarSinCos(&pitchSin, &pitchCos, pitch);

                vert.m.Position.x = pitchSin * yawCos;
                vert.m.Position.y = pitchCos;
                vert.m.Position.z = pitchSin * -yawSin;
                vert.m.Normal     = vert.m.Position;
                Vertices.push_back(vert);
            }
            for (size_t iParallel = 0; iParallel + 1 < nParallels; ++iParallel)
            {
                tri.idx[0] = iMeridian * nParallels + iParallel;
                tri.idx[1] = iMeridian * nParallels + iParallel + 1;
                tri.idx[2] = jMeridian * nParallels + iParallel;
                Triangles.push_back(tri);

                tri.idx[0] = jMeridian * nParallels + iParallel + 1;
                std::swap(tri.idx[1], tri.idx[2]);
                ASSERT(tri.idx[0] < nParallels * nMeridians);
                ASSERT(tri.idx[1] < nParallels * nMeridians);
                ASSERT(tri.idx[2] < nParallels * nMeridians);
                Triangles.push_back(tri);
            }

            tri.idx[0] = iMeridian * nParallels;
            tri.idx[1] = jMeridian * nParallels;
            tri.idx[2] = nMeridians * nParallels; // Первый полюс, положительный
            Triangles.push_back(tri);

            tri.idx[0] = nMeridians * nParallels + 1; // Второй полюс, отрицательный
            tri.idx[1] = jMeridian * nParallels + nParallels - 1;
            tri.idx[2] = iMeridian * nParallels + nParallels - 1;
            Triangles.push_back(tri);
        }

        vert.m.Position.x = 0.0f;
        vert.m.Position.y = 1.0f;
        vert.m.Position.z = 0.0f;
        vert.m.Normal     = vert.m.Position;
        Vertices.push_back(vert);

        vert.m.Position.y = -1.0f;
        vert.m.Normal     = vert.m.Position;
        Vertices.push_back(vert);

        InitBBox();
    }

    void InitBBox()
    {
        bool isFirst = true;
        for (const IntermediateVertex &vert : Vertices)
        {
            XMVECTOR pos = XMLoadFloat3(&vert.m.Position);
            if (isFirst)
            {
                BoxMax  = pos;
                BoxMin  = pos;
                isFirst = false;
            }
            else
            {
                BoxMax = XMVectorMax(BoxMax, pos);
                BoxMin = XMVectorMin(BoxMin, pos);
            }
        }
    }

    EdgeIndicesMap<2> BuildTriangleEdgeIndex(Slice<IntermediateTriangle> triangles)
    {
        EdgeIndicesMap<2> edgeTriangles;
        for (size_t iTriangle = 0; iTriangle < triangles.Size(); ++iTriangle)
        {
            for (size_t iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
            {
                MeshEdge edge        = triangles[iTriangle].EdgeKey(iTriEdge);
                auto [iter, isFirst] = edgeTriangles.try_emplace(edge);
                auto &vec            = iter->second;
                ASSERT(!LastEquals(vec, iTriangle));
                vec.push_back(iTriangle);
            }
        }
        return edgeTriangles;
    }

    void DoFirstPartition()
    {
        size_t nMeshlets = (Triangles.size() + TARGET_PRIMITIVES - 1) / TARGET_PRIMITIVES;

        MeshletLayerOffsets = {0, nMeshlets};

        size_t             nTriangles = Triangles.size();
        std::vector<idx_t> triangleMeshlet(nTriangles, 0);
        if (nMeshlets == 1)
        {
            std::fill(triangleMeshlet.begin(), triangleMeshlet.end(), IDX_C(0));
            return;
        }

        auto edgeTriangles = BuildTriangleEdgeIndex(Triangles);

        // std::cout << "Preparing METIS structure...\n";
        idx_t nparts = nMeshlets;
        idx_t nvtxs  = nTriangles;
        idx_t ncon   = 1;

        std::vector<idx_t> xadj;
        xadj.reserve(size_t(nvtxs) + 1);
        xadj.push_back(0);

        std::vector<idx_t> adjncy;
        std::vector<idx_t> adjwgt;

        XMVECTOR maxDiff = BoxMax - BoxMin;
        float    maxLen  = XMVectorGetX(XMVector3Length(maxDiff)) * (1.0f + FLT_EPSILON);

        for (idx_t iTriangle = 0; iTriangle < nTriangles; ++iTriangle)
        {
            for (idx_t iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
            {
                MeshEdge edge  = Triangles[iTriangle].EdgeKey(iTriEdge);
                size_t   iVert = edge.first;
                size_t   jVert = edge.second;
                auto    &vec   = edgeTriangles[edge];

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

        idx_t edgecut = 0;

        if constexpr (false)
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
                                              &nparts,
                                              nullptr /* tpwgts */,
                                              nullptr /* ubvec */,
                                              options /* options */,
                                              &edgecut /* edgecut */,
                                              triangleMeshlet.data() /* part */);
        ASSERT_EQ(metisResult, METIS_OK);

        SplitVector<size_t> meshletTriangleIndices(nMeshlets, Slice(triangleMeshlet));
        MeshletTriangles.Clear();
        for (size_t iMeshlet = 0; iMeshlet < nMeshlets; ++iMeshlet)
        {
            for (size_t iTriangle : meshletTriangleIndices[iMeshlet])
                MeshletTriangles.Push(Triangles[iTriangle]);
            MeshletTriangles.PushSplit();
        }
        MeshletParentOffset = std::vector<size_t>(nMeshlets, 0);
        MeshletParentCount  = std::vector<size_t>(nMeshlets, 0);
    }

    void BuildMeshletEdgeIndex(size_t iLayer)
    {
        size_t layerBeg  = MeshletLayerOffsets[iLayer];
        size_t layerEnd  = MeshletLayerOffsets[iLayer + 1];
        size_t nMeshlets = layerEnd - layerBeg;

        MeshletEdges.Clear();
        EdgeMeshlets.clear();

        for (size_t iMeshlet = layerBeg; iMeshlet < layerEnd; ++iMeshlet)
        {
            size_t                       iiMeshlet = iMeshlet - layerBeg;
            std::unordered_set<MeshEdge> seenEdges;
            for (const IntermediateTriangle &tri : MeshletTriangles[iMeshlet])
            {
                for (size_t iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
                {
                    MeshEdge edge        = tri.EdgeKey(iTriEdge);
                    auto [iter, isFirst] = EdgeMeshlets.try_emplace(edge);
                    auto &vec            = iter->second;
                    if (!LastEquals(vec, iiMeshlet))
                        vec.push_back(iiMeshlet);
                    seenEdges.insert(edge);
                }
            }

            for (const MeshEdge &edge : seenEdges)
                MeshletEdges.Push(edge);
            MeshletEdges.PushSplit();
        }
    }

    bool PartitionMeshlets()
    {
        size_t iLayer   = MeshletLayerOffsets.size() - 2;
        size_t layerBeg = MeshletLayerOffsets[iLayer];
        size_t layerEnd = MeshletLayerOffsets[iLayer + 1];

        // std::cout << "Building meshlet-edge index...\n";
        BuildMeshletEdgeIndex(iLayer);
        // std::cout << "Building meshlet-edge index done\n";

        idx_t              nMeshlets = layerEnd - layerBeg;
        idx_t              nParts    = (nMeshlets + 3) / 4;
        std::vector<idx_t> meshletPart(nMeshlets, 0);

        if (nParts < 3)
            return false;

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

            // Пометим все смежные мешлеты как непосещённые
            for (MeshEdge edge : MeshletEdges[iiMeshlet])
            {
                for (size_t jjMeshlet : EdgeMeshlets[edge])
                {
                    if (jjMeshlet != iiMeshlet)
                        commonEdgesWith[jjMeshlet] = 0;
                }
            }

            // Посчитаем веса рёбер как количество смежных рёбер сетки
            for (MeshEdge edge : MeshletEdges[iiMeshlet])
            {
                for (size_t jjMeshlet : EdgeMeshlets[edge])
                {
                    if (jjMeshlet != iiMeshlet)
                        commonEdgesWith[jjMeshlet]++;
                }
            }

            // Посчитаем веса рёбер как количество смежных рёбер сетки
            for (MeshEdge edge : MeshletEdges[iiMeshlet])
            {
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
        ASSERT_EQ(metisResult, METIS_OK);

        SplitVector<size_t> partMeshlets(nParts, Slice(meshletPart));

        for (size_t iMeshlet = layerBeg; iMeshlet < layerEnd; ++iMeshlet)
        {
            size_t iiMeshlet = iMeshlet - layerBeg;
            size_t iPart     = meshletPart[iiMeshlet];
        }

        // Отладочный второй способ подсчёта граничных вершин
        dbgVertexMeshletCount.resize(Vertices.size());
        std::fill(dbgVertexMeshletCount.begin(), dbgVertexMeshletCount.end(), 0);
        for (size_t iPart = 0; iPart < nParts; ++iPart)
        {
            for (size_t iiMeshlet : partMeshlets[iPart])
            {
                size_t iMeshlet = layerBeg + iiMeshlet;
                for (const IntermediateTriangle &tri : MeshletTriangles[iMeshlet])
                {
                    for (size_t iVert : tri.idx)
                        Vertices[iVert].Visited = false;
                }
            }
            for (size_t iiMeshlet : partMeshlets[iPart])
            {
                size_t iMeshlet = layerBeg + iiMeshlet;
                for (const IntermediateTriangle &tri : MeshletTriangles[iMeshlet])
                {
                    for (size_t iVert : tri.idx)
                    {
                        if (!Vertices[iVert].Visited)
                        {
                            Vertices[iVert].Visited = true;
                            dbgVertexMeshletCount[iVert]++;
                        }
                        ASSERT(dbgVertexMeshletCount[iVert] > 0);
                    }
                }
            }
        }

        // Децимация
        for (size_t iPart = 0; iPart < nParts; ++iPart)
            DecimateSuperMeshlet(iLayer, partMeshlets[iPart]);

        // Каждая часть становится двумя новыми мешлетами
        MeshletLayerOffsets.push_back(MeshletTriangles.PartCount());

        if (LayerMeshletCount(iLayer + 1) >= LayerMeshletCount(iLayer))
            return false;

        return true;
    }

    void DecimateSuperMeshlet(size_t iLayer, Slice<size_t> baseMeshlets)
    {
        size_t layerBeg = MeshletLayerOffsets[iLayer];
        size_t layerEnd = MeshletLayerOffsets[iLayer + 1];

        // TODO: Квадрики
        // TODO: Оптимизировать поиск граничных рёбер
        IntermediateMeshlet loc;
        loc.Init(Vertices, MeshletTriangles, baseMeshlets, layerBeg);

        // Проверим, что правильно определили граничные вершины
        for (size_t iLocVert = 0; iLocVert < loc.Vertices.size(); ++iLocVert)
        {
            size_t iVert = loc.Vertices[iLocVert].OtherIndex;
            // С плоской панелью некоторые вершины на границе мешлета
            // не принадлежат другим мешлетам
            if (!loc.Vertices[iLocVert].IsBorder)
                ASSERT_EQ(dbgVertexMeshletCount[iVert], 1);
        }

        loc.Decimate();

        idx_t nvtxs  = loc.Triangles.size();
        idx_t ncon   = 1;
        idx_t nparts = (nvtxs + TARGET_PRIMITIVES - 1) / TARGET_PRIMITIVES;
        if (nvtxs <= MESHLET_MAX_PRIMITIVES)
            nparts = 1;

        std::vector<idx_t> part(nvtxs, 0);

        if (nparts > 1)
        {
            // Разбиваем децимированный мешлет
            EdgeIndicesMap<2> edgeTriangles = BuildTriangleEdgeIndex(loc.Triangles);

            std::vector<idx_t> xadj;
            xadj.reserve(nvtxs + 1);
            xadj.push_back(0);

            std::vector<idx_t> adjncy;
            for (size_t iTriangle = 0; iTriangle < loc.Triangles.size(); ++iTriangle)
            {
                for (size_t iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
                {
                    MeshEdge edge = loc.Triangles[iTriangle].EdgeKey(iTriEdge);
                    auto    &vec  = edgeTriangles[edge];
                    for (size_t jTriangle : vec)
                    {
                        if (jTriangle == iTriangle)
                            continue;
                        adjncy.push_back(jTriangle);
                    }
                }
                xadj.push_back(adjncy.size());
            }

            idx_t options[METIS_NOPTIONS] = {};
            METIS_SetDefaultOptions(options);
            options[METIS_OPTION_NUMBERING] = 0;

            idx_t edgecut = 0;

            int metisResult = METIS_PartGraphKway(&nvtxs,
                                                  &ncon,
                                                  xadj.data(),
                                                  adjncy.data(),
                                                  /* vwgt */ nullptr,
                                                  /* vsize */ nullptr,
                                                  /* adjwgt */ nullptr,
                                                  &nparts,
                                                  /* tpwgts */ nullptr,
                                                  /* ubvec */ nullptr,
                                                  options,
                                                  &edgecut,
                                                  part.data());
            ASSERT_EQ(metisResult, METIS_OK);
        }

        for (IntermediateTriangle &tri : loc.Triangles)
        {
            for (size_t iTriVert = 0; iTriVert < 3; ++iTriVert)
            {
                size_t              iVert = tri.idx[iTriVert];
                IntermediateVertex &vert  = loc.Vertices[iVert];
                if (!vert.Visited)
                {
                    vert.Visited    = true;
                    vert.OtherIndex = Vertices.size();
                    Vertices.push_back(vert);
                }
                tri.idx[iTriVert] = vert.OtherIndex;
            }
        }

        for (size_t iiMeshlet : baseMeshlets)
        {
            size_t iMeshlet               = layerBeg + iiMeshlet;
            MeshletParentOffset[iMeshlet] = MeshletTriangles.PartCount();
            MeshletParentCount[iMeshlet]  = nparts;
        }

        SplitVector<size_t> triangleIdx(nparts, Slice(part));
        for (size_t iPart = 0; iPart < triangleIdx.PartCount(); ++iPart)
        {
            if (triangleIdx[iPart].Size() > MESHLET_MAX_PRIMITIVES)
                std::cerr << "Decimation fail: size = " << triangleIdx[iPart].Size() << " out of " << nvtxs << " and "
                          << nparts << " parts\n";
            for (size_t iTriangle : triangleIdx[iPart])
                MeshletTriangles.Push(loc.Triangles[iTriangle]);
            MeshletTriangles.PushSplit();
            MeshletParentOffset.push_back(0);
            MeshletParentCount.push_back(0);
        }
    }

    void ConvertModel(ModelCPU &outModel)
    {
        size_t nMeshlets  = MeshletLayerOffsets[MeshletLayerOffsets.size() - 1];
        size_t nTriangles = Triangles.size();
        outModel.Meshlets.reserve(nMeshlets);
        outModel.Primitives.reserve(nTriangles);

        for (idx_t iMeshlet = 0; iMeshlet < nMeshlets; ++iMeshlet)
        {
            // Помечаем каждую вершину мешлета как ещё не использованную в этом мешлете
            for (const IntermediateTriangle &tri : MeshletTriangles[iMeshlet])
            {
                for (size_t iVert : tri.idx)
                    Vertices[iVert].Visited = false;
            }

            MeshletDesc meshlet  = {};
            meshlet.VertOffset   = outModel.GlobalIndices.size();
            meshlet.VertCount    = 0;
            meshlet.PrimOffset   = MeshletTriangles.Split(iMeshlet);
            meshlet.PrimCount    = MeshletTriangles.PartSize(iMeshlet);
            meshlet.ParentOffset = MeshletParentOffset[iMeshlet];
            meshlet.ParentCount  = MeshletParentCount[iMeshlet];

            // Для отладки закодируем, какие вершины у мешлета --- граничные
            std::unordered_map<MeshEdge, size_t> edgeTriangleCount;
            for (const IntermediateTriangle &tri : MeshletTriangles[iMeshlet])
            {
                for (size_t iVert : tri.idx)
                    Vertices[iVert].IsBorder = false;
                for (size_t iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
                {
                    MeshEdge edge        = tri.EdgeKey(iTriEdge);
                    auto [iter, isFirst] = edgeTriangleCount.try_emplace(edge, 0);
                    iter->second++;
                }
            }
            for (const auto &[edge, nTris] : edgeTriangleCount)
            {
                if (nTris >= 2)
                    continue;
                for (size_t iVert : {edge.first, edge.second})
                    Vertices[iVert].IsBorder = true;
            }

            for (const IntermediateTriangle &tri : MeshletTriangles[iMeshlet])
            {
                uint encodedTriangle = 0;
                for (size_t iTriVert = 0; iTriVert < 3; ++iTriVert)
                {
                    size_t              iVert    = tri.idx[iTriVert];
                    IntermediateVertex &vert     = Vertices[iVert];
                    uint                iLocVert = vert.OtherIndex;
                    // Если вершина ещё не использована в этом мешлете,
                    // назначим ей новый индекс
                    if (!vert.Visited)
                    {
                        vert.Visited    = true;
                        iLocVert        = meshlet.VertCount++;
                        vert.OtherIndex = iLocVert;
                        if (vert.IsBorder)
                            outModel.GlobalIndices.push_back(iVert | UINT32_C(0x80000000));
                        else
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

    void dbgSaveAsObj(const std::filesystem::path &path)
    {
        std::ofstream fout(path);
        for (IntermediateVertex &v : Vertices)
            fout << "v " << v.m.Position.x << " " << v.m.Position.y << " " << v.m.Position.z << "\n";
        for (IntermediateVertex &v : Vertices)
        {
            if (v.IsBorder)
                fout << "vt 1\n";
            else
                fout << "vt 0\n";
        }
        for (IntermediateTriangle &tri : Triangles)
        {
            if (tri.IsDeleted)
                continue;
            fout << "f";
            for (size_t iVert : tri.idx)
                fout << " " << iVert + 1 << "/" << iVert + 1;
            fout << "\n";
        }
    }
};

int main()
{
    IntermediateMesh mesh;

#if false
    // Для отладки самой децимации пока будем выводить результат децимации сферы
    mesh.MakeSphere(32, 32);
    mesh.DoFirstPartition();
    std::vector<size_t> meshletIdx(mesh.MeshletTriangles.PartCount());
    for (size_t i = 0; i < meshletIdx.size(); ++i)
        meshletIdx[i] = i;
    IntermediateMeshlet meshlet;
    meshlet.Init(mesh.Vertices, mesh.MeshletTriangles, meshletIdx, 0);
    std::cout << "Init triangles: " << meshlet.Triangles.size() << std::endl;
    meshlet.Decimate();
    std::cout << "Triangles left: " << meshlet.Triangles.size() << std::endl;
    meshlet.dbgSaveAsObj(9999, true);
    return 0;
#endif

    std::cout << "Loading model...\n";
    // mesh.LoadGLB("plane1.glb");
    // mesh.LoadGLB("input.glb");
    // mesh.MakePlane(64);
    mesh.MakeSphere(64, 64);
    std::cout << "Loading model done\n";

    size_t nVertices  = mesh.Vertices.size();
    size_t nTriangles = mesh.Triangles.size();

    if constexpr (false)
    {
        auto edgeTriangles = mesh.BuildTriangleEdgeIndex(mesh.Triangles);
        std::cout << "\nBy edge:\n";
        for (const auto &[edge, tris] : edgeTriangles)
        {
            std::cout << "V[" << edge.first << ", " << edge.second << "]: T[";
            for (size_t i = 0; i < tris.size(); ++i)
            {
                if (i != 0)
                    std::cout << ", ";
                std::cout << tris[i];
            }
            std::cout << "]\n";
        }
    }

    // std::cout << "Partitioning meshlets...\n";
    mesh.DoFirstPartition();
    // std::cout << "Partitioning meshlets done\n";

    for (int i = 0;; ++i)
    {
        std::cout << "Partitioning layer " << i << "...\n";
        std::cout << "\tCurrent meshlets: " << mesh.LayerMeshletCount(i) << "\n";
        if (!mesh.PartitionMeshlets())
            break;
        std::cout << "Partitioning layer " << i << " done\n";
    }

    ModelCPU outModel;

    // std::cout << "Converting out model...\n";
    mesh.ConvertModel(outModel);
    // std::cout << "Converting out model done\n";

    // Предупреждаем о нарушениях контракта
    if constexpr (true)
    {
        for (size_t iMeshlet = 0; iMeshlet < mesh.MeshletTriangles.PartCount(); ++iMeshlet)
        {
            size_t meshletSize = mesh.MeshletTriangles.PartSize(iMeshlet);
            if (meshletSize > MESHLET_MAX_PRIMITIVES)
                std::cout << "Meshlet[" << iMeshlet << "].Size = " << meshletSize << "\n";
        }
    }

    std::cout << "Saving model...\n";
    outModel.SaveToFile("../MasterThesis/model.bin");
    std::cout << "Saving model done\n";

    if constexpr (false)
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
