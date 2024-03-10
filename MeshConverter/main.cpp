#include <Common.h>

#include <algorithm>
#include <functional>
#include <iostream>
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

static void AssertFn(bool cond, std::string_view text, int line)
{
    if (cond)
        return;
    std::ostringstream oss;
    oss << text << " at line " << line;
    throw std::runtime_error(oss.str());
}

#define ASSERT_TEXT(cond, text) AssertFn(cond, text, __LINE__)
#define ASSERT(cond) AssertFn(cond, "Assertion failed: " #cond, __LINE__)

using MeshEdge = std::pair<size_t, size_t>;

struct MeshEdgeHasher
{
    uint64_t operator()(MeshEdge edge) const noexcept
    {
        return std::hash<uint64_t>{}((edge.first & UINT64_C(0xFFFFFFFF))
                                     | ((edge.second & UINT64_C(0xFFFFFFFF)) << 32));
    }
};

struct IntermediateVertex
{
    Vertex m;

    // Используем вектор вместо словаря, т.к. ключи --- это индексы вершин
    // UINT32_MAX --- непосещённая вершина
    size_t OtherIndex = 0;
};

struct IndexTriangle
{
    size_t idx[3] = {};

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
        ASSERT(!IsEmpty());
        return Data[Size - 1];
    }

    bool LastEquals(T x) const noexcept { return !IsEmpty() && Last() == x; }

    void Clear() noexcept { Size = 0; }

    void Push(T x)
    {
        ASSERT(Size < CAPACITY);
        Data[Size++] = x;
    }

    T Pop()
    {
        ASSERT(!IsEmpty());
        return Data[--Size];
    }

    // Имена в нижнем регистре связаны с синтаксисом for-each в C++
    T       *begin() noexcept { return Data; }
    T       *end() noexcept { return Data + Size; }
    const T *begin() const noexcept { return Data; }
    const T *end() const noexcept { return Data + Size; }
};

template <typename T> struct Slice
{
    Slice(T *data, size_t size) : mData(data), mSize(size) {}
    Slice(T *begin, T *end) : Slice(begin, std::distance(begin, end)) {}
    Slice(std::vector<T> &vec) : Slice(vec.data(), vec.size()) {}
    template <size_t N> Slice(T arr[N]) : Slice(arr, N) {}
    template <size_t N> Slice(TrivialVector<T, N> &vec) : Slice(vec.Data, vec.Size) {}
    Slice(const Slice &) = default;

    T       &operator[](size_t i) { return mData[i]; }
    const T &operator[](size_t i) const { return mData[i]; }

    constexpr bool   IsEmpty() const noexcept { return Size() == 0; }
    constexpr size_t Size() const noexcept { return mSize; }

    constexpr Slice Subslice(size_t beg, size_t end) const
    {
        ASSERT(beg <= end);
        ASSERT(end <= mSize);
        return Slice(mData + beg, mData + end);
    }
    constexpr Slice Skip(size_t n) const
    {
        ASSERT(n <= mSize);
        return Slice(mData + n, mSize - n);
    }
    constexpr Slice Limit(size_t n) const { return Slice(mData, std::min(mSize, n)); }

    constexpr T       *begin() noexcept { return mData; }
    constexpr T       *end() noexcept { return mData + mSize; }
    constexpr const T *begin() const noexcept { return mData; }
    constexpr const T *end() const noexcept { return mData + mSize; }

  private:
    T     *mData;
    size_t mSize;
};

template <typename T> Slice<T> Split(Slice<T> items, Slice<size_t> splits, size_t i)
{
    size_t beg = splits[i];
    size_t end = splits[i + 1];
    return items.Subslice(beg, end);
}

template <typename T> struct SplitVector
{
    SplitVector()                               = default;
    SplitVector(const SplitVector &)            = default;
    SplitVector(SplitVector &&)                 = default;
    SplitVector &operator=(const SplitVector &) = default;
    SplitVector &operator=(SplitVector &&)      = default;

    // Преобразовать из вектора, в котором записаны номера кластеров
    template <typename IntT>
    SplitVector(size_t nClusters, Slice<IntT> clusterization) : mVec(clusterization.Size()), mSplits(nClusters + 1, 0)
    {
        for (IntT cluster : clusterization)
            mSplits[cluster + 1]++;
        for (size_t i = 2; i <= nClusters; ++i)
            mSplits[i] += mSplits[i - 1];
        for (size_t i = 0; i < clusterization.Size(); ++i)
        {
            size_t iCluster = clusterization[i];
            size_t pos      = mSplits[iCluster]++;
            mVec[pos]       = i;
        }
        for (size_t i = nClusters; i > 0; --i)
            mSplits[i] = mSplits[i - 1];
        mSplits[0] = 0;

        for (size_t i = 0; i < nClusters; ++i)
        {
            ASSERT(mSplits[i] <= mSplits[i + 1]);
        }
    }

    size_t   PartSize(size_t iPart) const { return mSplits[iPart + 1] - mSplits[iPart]; }
    size_t   PartCount() const noexcept { return mSplits.size() - 1; }
    Slice<T> operator[](size_t iPart) { return ::Split(Slice(mVec), Slice(mSplits), iPart); }

    T       &Flat(size_t i) { return mVec[i]; }
    const T &Flat(size_t i) const { return mVec[i]; }
    size_t   Split(size_t i) const { return mSplits[i]; }

    void Push(const T &x) { mVec.push_back(x); }
    void Push(T &&x) { mVec.push_back(std::move(x)); }
    void PushSplit() { mSplits.push_back(mVec.size()); }

    void Clear()
    {
        mVec.clear();
        mSplits.clear();
        mSplits.push_back(0);
    }

  private:
    std::vector<T>      mVec;
    std::vector<size_t> mSplits{0};
};

struct DisjointSetUnion
{
    struct Node
    {
        size_t Parent;
        size_t Height;
    };

    std::vector<Node> Nodes;

    DisjointSetUnion(size_t n) : Nodes(n)
    {
        for (size_t i = 0; i < n; ++i)
        {
            Nodes[i].Parent = i;
            Nodes[i].Height = 0;
        }
    }

    size_t Get(size_t i)
    {
        size_t j = Nodes[i].Parent;
        if (i != j)
        {
            j               = Get(j);
            Nodes[i].Parent = j;
        }
        return j;
    }

    size_t Unite(size_t i, size_t j)
    {
        i = Get(i);
        j = Get(j);
        if (i == j)
            return i;
        if (Nodes[i].Height < Nodes[j].Height)
        {
            Nodes[i].Parent = j;
            return j;
        }
        else if (Nodes[i].Height > Nodes[j].Height)
        {
            Nodes[j].Parent = i;
            return i;
        }
        else
        {
            Nodes[j].Parent = i;
            Nodes[i].Height++;
            return i;
        }
    }

    void UniteLeft(size_t i, size_t j)
    {
        i = Get(i);
        j = Get(j);
        if (i == j)
            return;
        Nodes[i].Height = std::max(Nodes[i].Height, Nodes[j].Height + 1);
        Nodes[j].Parent = i;
    }
};

struct IntermediateMesh
{
    template <size_t N> using EdgeIndicesMap = std::unordered_map<MeshEdge, TrivialVector<size_t, N>, MeshEdgeHasher>;

    std::vector<IntermediateVertex> Vertices;
    std::vector<IndexTriangle>      Triangles;
    XMVECTOR                        BoxMax = XMVectorZero();
    XMVECTOR                        BoxMin = XMVectorZero();

    std::vector<size_t> MeshletLayerOffsets;

    SplitVector<IndexTriangle> MeshletTriangles;
    std::vector<size_t>        MeshletParents1;

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

        ASSERT(inModel.meshes.size() == 1);
        tinygltf::Mesh &mesh = inModel.meshes[0];

        ASSERT(mesh.primitives.size() == 1);
        tinygltf::Primitive &primitive = mesh.primitives[0];

        ASSERT(primitive.mode == TINYGLTF_MODE_TRIANGLES);

        int positionIdx = -1;
        int normalIdx   = -1;
        for (auto &&pair : primitive.attributes)
        {
            if (pair.first == "POSITION")
            {
                ASSERT(positionIdx == -1);
                positionIdx = pair.second;
            }
            else if (pair.first == "NORMAL")
            {
                ASSERT(normalIdx == -1);
                normalIdx = pair.second;
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
        ASSERT(positionAccessor.type == TINYGLTF_TYPE_VEC3);
        ASSERT(positionAccessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT);
        ASSERT(normalAccessor.type == TINYGLTF_TYPE_VEC3);
        ASSERT(normalAccessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT);

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

        ASSERT(indexComponentSize == indexAccessor.ByteStride(indexBufferView));

        size_t nTriangles = indexAccessor.count / 3;
        Triangles.reserve(nTriangles);
        for (size_t iTriangle = 0; iTriangle < nTriangles; ++iTriangle)
        {
            IndexTriangle tri;
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

    EdgeIndicesMap<2> BuildTriangleEdgeIndex(Slice<IndexTriangle> triangles)
    {
        EdgeIndicesMap<2> edgeTriangles;
        for (size_t iTriangle = 0; iTriangle < triangles.Size(); ++iTriangle)
        {
            for (size_t iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
            {
                MeshEdge edge        = triangles[iTriangle].EdgeKey(iTriEdge);
                auto [iter, isFirst] = edgeTriangles.try_emplace(edge);
                auto &vec            = iter->second;
                ASSERT(!vec.LastEquals(iTriangle));
                vec.Push(iTriangle);
            }
        }
        return edgeTriangles;
    }

    void DoFirstPartition()
    {
        constexpr size_t TARGET_PRIMITIVES = MESHLET_MAX_PRIMITIVES * 3 / 4;
        size_t           nMeshlets         = (Triangles.size() + TARGET_PRIMITIVES - 1) / TARGET_PRIMITIVES;

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
        ASSERT(metisResult == METIS_OK);

        SplitVector<size_t> meshletTriangleIndices(nMeshlets, Slice(triangleMeshlet));
        MeshletTriangles.Clear();
        for (size_t iMeshlet = 0; iMeshlet < nMeshlets; ++iMeshlet)
        {
            for (size_t iTriangle : meshletTriangleIndices[iMeshlet])
                MeshletTriangles.Push(Triangles[iTriangle]);
            MeshletTriangles.PushSplit();
        }
        MeshletParents1 = std::vector<size_t>(nMeshlets, 0);
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
            size_t                iiMeshlet = iMeshlet - layerBeg;
            std::vector<MeshEdge> seenEdges;
            for (IndexTriangle &tri : MeshletTriangles[iMeshlet])
            {
                for (size_t iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
                {
                    MeshEdge edge        = tri.EdgeKey(iTriEdge);
                    auto [iter, isFirst] = EdgeMeshlets.try_emplace(edge);
                    auto &vec            = iter->second;
                    if (!vec.LastEquals(iiMeshlet))
                        vec.Push(iiMeshlet);
                    seenEdges.push_back(edge);
                }
            }

            std::sort(seenEdges.begin(), seenEdges.end());
            seenEdges.erase(std::unique(seenEdges.begin(), seenEdges.end()), seenEdges.end());
            for (MeshEdge &edge : seenEdges)
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

        if (nParts < 2)
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
        ASSERT(metisResult == METIS_OK);

        SplitVector<size_t> partMeshlets(nParts, Slice(meshletPart));

        for (size_t iMeshlet = layerBeg; iMeshlet < layerEnd; ++iMeshlet)
        {
            size_t iiMeshlet = iMeshlet - layerBeg;
            size_t iPart     = meshletPart[iiMeshlet];
            // Обоих родителей мешлета будем добавлять подряд,
            // поэтому индекс второго на 1 больше индекса первого
            MeshletParents1[iMeshlet] = layerEnd + 2 * iPart;
        }

        // Отладочный второй способ подсчёта граничных вершин
        dbgVertexMeshletCount.resize(Vertices.size());
        std::fill(dbgVertexMeshletCount.begin(), dbgVertexMeshletCount.end(), 0);
        for (size_t iPart = 0; iPart < nParts; ++iPart)
        {
            for (size_t iiMeshlet : partMeshlets[iPart])
            {
                size_t iMeshlet = layerBeg + iiMeshlet;
                for (IndexTriangle &tri : MeshletTriangles[iMeshlet])
                {
                    for (size_t iVert : tri.idx)
                        Vertices[iVert].OtherIndex = UINT32_MAX;
                }
            }
            for (size_t iiMeshlet : partMeshlets[iPart])
            {
                size_t iMeshlet = layerBeg + iiMeshlet;
                for (IndexTriangle &tri : MeshletTriangles[iMeshlet])
                {
                    for (size_t iVert : tri.idx)
                    {
                        if (Vertices[iVert].OtherIndex == UINT32_MAX)
                        {
                            dbgVertexMeshletCount[iVert]++;
                            Vertices[iVert].OtherIndex = 0;
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
        return true;
    }

    void DecimateSuperMeshlet(size_t iLayer, Slice<size_t> baseMeshlets)
    {
        size_t layerBeg = MeshletLayerOffsets[iLayer];
        size_t layerEnd = MeshletLayerOffsets[iLayer + 1];

#if false
        for (size_t iMeshlet : baseMeshlets)
        {
            // Нельзя использовать Slice
            size_t meshletBeg = MeshletTriangles.Split(iMeshlet);
            size_t meshletEnd = MeshletTriangles.Split(iMeshlet + 1);
            for (size_t iiTriangle = meshletBeg; iiTriangle < meshletEnd; ++iiTriangle)
            {
                size_t iTriangle = MeshletTriangles.Flat(iiTriangle);
                MeshletTriangles.Push(iTriangle);
            }
            MeshletParents1[iMeshlet] = MeshletParents1.size();
            MeshletTriangles.PushSplit();
            MeshletTriangles.PushSplit();
            MeshletParents1.push_back(0);
            MeshletParents1.push_back(0);
        }
        return;
#endif

        // TODO: Квадрики
        // TODO: Оптимизировать поиск граничных рёбер
        std::vector<IntermediateVertex>                      locVertices;
        std::vector<IndexTriangle>                           locTriangles;
        std::unordered_map<MeshEdge, size_t, MeshEdgeHasher> edgeTriangleCount;

        std::unordered_set<MeshEdge, MeshEdgeHasher> dbgUsedEdges;

        // Помечаем вершины для сбора, собираем треугольники
        for (size_t iiMeshlet : baseMeshlets)
        {
            size_t iMeshlet = layerBeg + iiMeshlet;
            for (IndexTriangle &tri : MeshletTriangles[iMeshlet])
            {
                for (size_t iVert : tri.idx)
                    Vertices[iVert].OtherIndex = UINT32_MAX;
                locTriangles.push_back(tri);
            }
        }

        // Собираем вершины, а также преобразовываем индексы к локальным
        for (IndexTriangle &tri : locTriangles)
        {
            for (size_t iTriVert = 0; iTriVert < 3; ++iTriVert)
            {
                size_t iVert    = tri.idx[iTriVert];
                size_t iLocVert = Vertices[iVert].OtherIndex;
                if (iLocVert >= locVertices.size())
                {
                    iLocVert                   = locVertices.size();
                    Vertices[iVert].OtherIndex = iLocVert;
                    IntermediateVertex vert    = Vertices[iVert];
                    // Обратный индекс, по которому будем определять, нужно ли добавлять вершину
                    vert.OtherIndex = iVert;
                    locVertices.push_back(vert);
                }
                tri.idx[iTriVert] = iLocVert;
            }

            for (size_t iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
            {
                MeshEdge edge        = tri.EdgeKey(iTriEdge);
                auto [iter, isFirst] = edgeTriangleCount.try_emplace(edge, 0);
                iter->second++;
            }
        }

        std::vector<bool> isTriangleDeleted(locTriangles.size(), false);
        std::vector<bool> isVertexBorder(locVertices.size(), false);

        for (auto &[edge, count] : edgeTriangleCount)
        {
            if (count == 1)
            {
                isVertexBorder[edge.first]  = true;
                isVertexBorder[edge.second] = true;
                dbgUsedEdges.insert(edge);
            }
        }

        // Проверим, что правильно определили граничные вершины
        for (size_t iLocVert = 0; iLocVert < locVertices.size(); ++iLocVert)
        {
            size_t iVert = locVertices[iLocVert].OtherIndex;
            // С плоской панелью некоторые вершины на границе мешлета
            // не принадлежат другим мешлетам
            if (!isVertexBorder[iLocVert])
                ASSERT(dbgVertexMeshletCount[iVert] == 1);
        }

        // Неправильная децимация, просто схлопываем ребро к одной вершине
        // Хотим проверить корректность топологии
        DisjointSetUnion dsu(locVertices.size());
        size_t           nCollapsed = 0;

        for (size_t iTriangle = 0; iTriangle < locTriangles.size(); ++iTriangle)
        {
            for (size_t iTriEdge = 0; !isTriangleDeleted[iTriangle] && iTriEdge < 3; ++iTriEdge)
            {
                size_t iVert = dsu.Get(locTriangles[iTriangle].idx[iTriEdge]);
                size_t jVert = dsu.Get(locTriangles[iTriangle].idx[(iTriEdge + 1) % 3]);
                if (iVert == jVert)
                {
                    isTriangleDeleted[iTriangle] = true;
                    break;
                }
                if (isVertexBorder[iVert])
                {
                    if (isVertexBorder[jVert])
                    {
                        // Ребро на границе, схлопывать нельзя
                        continue;
                    }
                    else
                    {
                        dsu.UniteLeft(iVert, jVert);
                    }
                }
                else
                {
                    if (isVertexBorder[jVert])
                    {
                        dsu.UniteLeft(jVert, iVert);
                    }
                    else
                    {
                        size_t kVert = dsu.Unite(iVert, jVert);
                        ASSERT(kVert == iVert || kVert == jVert);
                    }
                }
                ++nCollapsed;
                isTriangleDeleted[iTriangle] = true;
            }
        }

        std::vector<IndexTriangle> resultTriangles;

        for (size_t iTriangle = 0; iTriangle < locTriangles.size(); ++iTriangle)
        {
            IndexTriangle tri = locTriangles[iTriangle];
            for (size_t iTriVert = 0; iTriVert < 3; ++iTriVert)
                tri.idx[iTriVert] = dsu.Get(tri.idx[iTriVert]);
            for (size_t iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
            {
                size_t iVert = tri.idx[iTriEdge];
                size_t jVert = tri.idx[(iTriEdge + 1) % 3];
                if (iVert == jVert)
                    isTriangleDeleted[iTriangle] = true;
            }

            if (isTriangleDeleted[iTriangle])
                continue;

            for (size_t iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
            {
                MeshEdge edge = tri.EdgeKey(iTriEdge);
                dbgUsedEdges.erase(edge);
            }

            for (size_t iTriVert = 0; iTriVert < 3; ++iTriVert)
            {
                size_t iLocVert   = tri.idx[iTriVert];
                size_t iVert      = locVertices[iLocVert].OtherIndex;
                tri.idx[iTriVert] = iVert;
            }
            resultTriangles.push_back(tri);
        }

        ASSERT(dbgUsedEdges.empty());

        // size_t nTrisLeft = MeshletTriangles.size() - MeshletTriangleOffsets[MeshletTriangleOffsets.size() - 1];
        // std::cout << "Collapsed " << nCollapsed << " out of " << edgeTriangleCount.size() << " edges, " << nTrisLeft
        //           << " triangles out of " << locTriangles.size() << " left\n";

        // Разбиваем децимированный мешлет на два

        EdgeIndicesMap<2> edgeTriangles = BuildTriangleEdgeIndex(resultTriangles);

        idx_t nvtxs = resultTriangles.size();
        idx_t ncon  = 1;

        std::vector<idx_t> xadj;
        xadj.reserve(nvtxs + 1);
        xadj.push_back(0);

        std::vector<idx_t> adjncy;
        for (size_t iTriangle = 0; iTriangle < resultTriangles.size(); ++iTriangle)
        {
            for (size_t iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
            {
                MeshEdge edge = resultTriangles[iTriangle].EdgeKey(iTriEdge);
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

        idx_t nparts = 2;

        idx_t options[METIS_NOPTIONS] = {};
        METIS_SetDefaultOptions(options);
        options[METIS_OPTION_NUMBERING] = 0;

        idx_t edgecut = 0;

        std::vector<idx_t> part(nvtxs, 0);

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
        ASSERT(metisResult == METIS_OK);

        // Достаточно будет два раза пойти по вектору
        for (idx_t iPart = 0; iPart < 2; ++iPart)
        {
            for (size_t iTriangle = 0; iTriangle < resultTriangles.size(); ++iTriangle)
            {
                if (part[iTriangle] != iPart)
                    continue;
                MeshletTriangles.Push(resultTriangles[iTriangle]);
            }
            MeshletTriangles.PushSplit();
            MeshletParents1.push_back(0);
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
            for (IndexTriangle &tri : MeshletTriangles[iMeshlet])
            {
                for (size_t iVert : tri.idx)
                    Vertices[iVert].OtherIndex = UINT32_MAX;
            }

            MeshletDesc meshlet = {};
            meshlet.VertOffset  = outModel.GlobalIndices.size();
            meshlet.VertCount   = 0;
            meshlet.PrimOffset  = MeshletTriangles.Split(iMeshlet);
            meshlet.PrimCount   = MeshletTriangles.PartSize(iMeshlet);
            meshlet.Parent1     = MeshletParents1[iMeshlet];
            meshlet.Parent2     = meshlet.Parent1 == 0 ? 0 : meshlet.Parent1 + 1;

            // Для отладки закодируем, какие вершины у мешлета --- граничные
            std::unordered_map<MeshEdge, size_t, MeshEdgeHasher> edgeTriangleCount;
            for (IndexTriangle &tri : MeshletTriangles[iMeshlet])
            {
                for (size_t iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
                {
                    MeshEdge edge        = tri.EdgeKey(iTriEdge);
                    auto [iter, isFirst] = edgeTriangleCount.try_emplace(edge, 0);
                    iter->second++;
                }
            }
            std::unordered_set<size_t> borderVertices;
            for (auto &[edge, nTris] : edgeTriangleCount)
            {
                if (nTris >= 2)
                    continue;
                for (size_t iVert : {edge.first, edge.second})
                    borderVertices.insert(nTris);
            }

            for (IndexTriangle &tri : MeshletTriangles[iMeshlet])
            {
                uint encodedTriangle = 0;
                for (size_t iTriVert = 0; iTriVert < 3; ++iTriVert)
                {
                    size_t iVert    = tri.idx[iTriVert];
                    uint   iLocVert = Vertices[iVert].OtherIndex;
                    // Если вершина ещё не использована в этом мешлете,
                    // назначим ей новый индекс
                    if (iLocVert >= meshlet.VertCount)
                    {
                        iLocVert                   = meshlet.VertCount++;
                        Vertices[iVert].OtherIndex = iLocVert;
                        if (borderVertices.find(iVert) != borderVertices.end())
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
};

int main()
{
    IntermediateMesh mesh;

    std::cout << "Loading model...\n";
    mesh.LoadGLB("plane.glb");
    std::cout << "Loading model done\n";

    size_t nVertices  = mesh.Vertices.size();
    size_t nTriangles = mesh.Triangles.size();

    if constexpr (false)
    {
        auto edgeTriangles = mesh.BuildTriangleEdgeIndex(mesh.Triangles);
        std::cout << "\nBy edge:\n";
        for (auto &[edge, tris] : edgeTriangles)
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

    // std::cout << "Partitioning meshlets...\n";
    mesh.DoFirstPartition();
    // std::cout << "Partitioning meshlets done\n";

    for (int i = 0; i < 2; ++i)
    {
        // std::cout << "Partitioning meshlet graph...\n";
        if (!mesh.PartitionMeshlets())
            break;
        // std::cout << "Partitioning meshlet graph done\n";
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
