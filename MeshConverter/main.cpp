#include <Common.h>

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

static void AssertFn(bool cond, std::string_view text, int line)
{
    if (cond)
        return;
    std::ostringstream oss;
    oss << text << " at line " << line;
    throw std::runtime_error(oss.str());
}

template <typename L, typename R>
static void AssertEqFn(const L &left, const R &right, std::string_view leftText, std::string_view rightText, int line)
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
    bool                  IsDirty   = false;

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

template <typename T> bool LastEquals(const std::vector<T> &vec, const T &x)
{
    return !vec.empty() && vec[vec.size() - 1] == x;
}

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
        FillStart(nClusters, clusterization.Size());
        for (IntT cluster : clusterization)
            FillReserve(cluster);
        FillPreparePush();
        for (size_t i = 0; i < clusterization.Size(); ++i)
        {
            size_t iCluster = clusterization[i];
            FillPush(iCluster, i);
            ASSERT(mSplits[iCluster] <= mSplits[iCluster + 1]);
        }
        FillCommit();
    }

    void FillStart(size_t nClusters, size_t nObjects)
    {
        mSplits.resize(nClusters + 1);
        mVec.resize(nObjects);
        std::fill(mSplits.begin(), mSplits.end(), 0);
    }

    void FillReserve(size_t iCluster, size_t by = 1) { mSplits[iCluster + 1]++; }

    void FillPreparePush()
    {
        size_t nClusters = mSplits.size() - 1;
        for (size_t i = 2; i <= nClusters; ++i)
            mSplits[i] += mSplits[i - 1];
        for (size_t i = 0; i < nClusters; ++i)
            ASSERT(mSplits[i] <= mSplits[i + 1]);
    }

    void FillPush(size_t iCluster, const T &x)
    {
        size_t pos = mSplits[iCluster]++;
        mVec[pos]  = x;
    }

    void FillPush(size_t iCluster, T &&x)
    {
        size_t pos = mSplits[iCluster]++;
        mVec[pos]  = std::move(x);
    }

    void FillCommit()
    {
        size_t nClusters = mSplits.size() - 1;
        for (size_t i = nClusters; i > 0; --i)
            mSplits[i] = mSplits[i - 1];
        mSplits[0] = 0;

        for (size_t i = 0; i < nClusters; ++i)
            ASSERT(mSplits[i] <= mSplits[i + 1]);
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

    void Init(size_t n)
    {
        Nodes.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            Nodes[i].Parent = i;
            Nodes[i].Height = 0;
        }
    }

    size_t NewNode()
    {
        size_t i    = Nodes.size();
        Node   node = {};
        node.Height = 0;
        node.Parent = i;
        Nodes.push_back(node);
        return i;
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

        InitQuadrics();

        size_t nMerged = 0;
        dbgSaveAsObj(nMerged);
        for (size_t iteration = 0; iteration < 100; ++iteration)
        {
            if (iteration % 5 == 0)
            {
                RemoveDeletedTriangles();
                nDeletedTriangles = 0;
            }
            if (Triangles.size() - nDeletedTriangles <= 2 * TARGET_PRIMITIVES)
                break;
            double threshold = 1e-9 * pow(iteration + 3.0, 5.0);
            for (IntermediateTriangle &tri : Triangles)
                tri.IsDirty = false;
            for (size_t iTriangle = 0; iTriangle < Triangles.size(); ++iTriangle)
            {
                if (Triangles.size() - nDeletedTriangles <= 2 * TARGET_PRIMITIVES)
                    break;
                const IntermediateTriangle &tri = Triangles[iTriangle];
                if (tri.IsDeleted || tri.IsDirty || tri.Error[3] > threshold)
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
                    CalculateError(iVert, jVert, mid);

                    deleted1.resize(VertexTriangles(iVert).Size());
                    std::fill(deleted1.begin(), deleted1.end(), 0);
                    if (Flipped(mid, iVert, jVert, deleted1))
                        continue;
                    deleted2.resize(VertexTriangles(jVert).Size());
                    std::fill(deleted2.begin(), deleted2.end(), 0);
                    if (Flipped(mid, jVert, iVert, deleted2))
                        continue;

                    if (vert1.IsBorder)
                    {
                        vert1.Quadric += vert2.Quadric;
                        GatherTriangles(iVert, iVert, nDeletedTriangles, deleted1);
                        GatherTriangles(iVert, jVert, nDeletedTriangles, deleted2);
                        VertexCluster[iVert] = ClusterTriangles.PartCount();
                        ClusterTriangles.PushSplit();
                        dbgSaveAsObj(++nMerged);
                        break;
                    }
                    if (vert2.IsBorder)
                    {
                        vert2.Quadric += vert1.Quadric;
                        GatherTriangles(jVert, iVert, nDeletedTriangles, deleted1);
                        GatherTriangles(jVert, jVert, nDeletedTriangles, deleted2);
                        VertexCluster[jVert] = ClusterTriangles.PartCount();
                        ClusterTriangles.PushSplit();
                        dbgSaveAsObj(++nMerged);
                        break;
                    }
                    XMStoreFloat3(&vert1.m.Position, mid);
                    vert1.Quadric += vert2.Quadric;
                    vert1.OtherIndex = UINT32_MAX;
                    vert1.Visited    = false;
                    GatherTriangles(iVert, iVert, nDeletedTriangles, deleted1);
                    GatherTriangles(iVert, jVert, nDeletedTriangles, deleted2);
                    VertexCluster[iVert] = ClusterTriangles.PartCount();
                    ClusterTriangles.PushSplit();
                    dbgSaveAsObj(++nMerged);
                    break;
                }
            }
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
        // ASSERT(dbgUsedEdges.empty());
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

        XMVECTOR det  = XMVectorZero();
        XMMATRIX qInv = XMMatrixInverse(&det, q);
        if (fabs(XMVectorGetX(det)) >= 0.001)
        {
            out = XMVector4Transform(XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f), qInv);
            return VertexError(q, out);
        }

        XMVECTOR p3   = 0.5 * (p1 + p2);
        float    err1 = VertexError(q, p1);
        float    err2 = VertexError(q, p2);
        float    err3 = VertexError(q, p3);
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
            tri.IsDirty       = true;
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
            if (XMVectorGetX(XMVector3Dot(normNew, normOld)) < 0.0)
                return true;
            if (fabs(XMVectorGetX(XMVector3Dot(nabNew, nacNew))) > 0.999)
                return true;
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

    void dbgSaveAsObj(size_t iteration)
    {
        if constexpr (false)
        {
            std::ostringstream ossFilename;
            ossFilename << "dbg" << std::setfill('0') << std::setw(4) << iteration << ".obj";
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
    std::vector<size_t>               MeshletParents1;

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

        // Разбиваем децимированный мешлет на два
        EdgeIndicesMap<2> edgeTriangles = BuildTriangleEdgeIndex(loc.Triangles);

        idx_t nvtxs = loc.Triangles.size();
        idx_t ncon  = 1;

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
        ASSERT_EQ(metisResult, METIS_OK);

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

        // Достаточно будет два раза пойти по вектору
        for (idx_t iPart = 0; iPart < 2; ++iPart)
        {
            for (size_t iTriangle = 0; iTriangle < loc.Triangles.size(); ++iTriangle)
            {
                if (part[iTriangle] != iPart)
                    continue;
                MeshletTriangles.Push(loc.Triangles[iTriangle]);
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
            for (const IntermediateTriangle &tri : MeshletTriangles[iMeshlet])
            {
                for (size_t iVert : tri.idx)
                    Vertices[iVert].Visited = false;
            }

            MeshletDesc meshlet = {};
            meshlet.VertOffset  = outModel.GlobalIndices.size();
            meshlet.VertCount   = 0;
            meshlet.PrimOffset  = MeshletTriangles.Split(iMeshlet);
            meshlet.PrimCount   = MeshletTriangles.PartSize(iMeshlet);
            meshlet.Parent1     = MeshletParents1[iMeshlet];
            meshlet.Parent2     = meshlet.Parent1 == 0 ? 0 : meshlet.Parent1 + 1;

            // Для отладки закодируем, какие вершины у мешлета --- граничные
            std::unordered_map<MeshEdge, size_t> edgeTriangleCount;
            for (const IntermediateTriangle &tri : MeshletTriangles[iMeshlet])
            {
                for (size_t iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
                {
                    MeshEdge edge        = tri.EdgeKey(iTriEdge);
                    auto [iter, isFirst] = edgeTriangleCount.try_emplace(edge, 0);
                    iter->second++;
                }
            }
            std::unordered_set<size_t> borderVertices;
            for (const auto &[edge, nTris] : edgeTriangleCount)
            {
                if (nTris >= 2)
                    continue;
                for (size_t iVert : {edge.first, edge.second})
                    borderVertices.insert(nTris);
            }

            for (const IntermediateTriangle &tri : MeshletTriangles[iMeshlet])
            {
                uint encodedTriangle = 0;
                for (size_t iTriVert = 0; iTriVert < 3; ++iTriVert)
                {
                    size_t iVert    = tri.idx[iTriVert];
                    uint   iLocVert = Vertices[iVert].OtherIndex;
                    // Если вершина ещё не использована в этом мешлете,
                    // назначим ей новый индекс
                    if (!Vertices[iVert].Visited)
                    {
                        Vertices[iVert].Visited    = true;
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

#if true
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
#else
int main()
{
    IntermediateMeshlet meshlet;
    meshlet.Vertices.reserve(16);
    meshlet.Triangles.reserve(18);
    for (int y = 0; y < 4; ++y)
    {
        for (int x = 0; x < 4; ++x)
        {
            IntermediateVertex v = {};
            v.m.Position.x       = 2.0f / 3.0f * x - 1.0f;
            v.m.Position.y       = 2.0f / 3.0f * y - 1.0f;
            meshlet.Vertices.push_back(v);
        }
    }
    for (int y = 0; y < 3; ++y)
    {
        for (int x = 0; x < 3; ++x)
        {
            IntermediateTriangle tri = {};

            tri.idx[0] = 4 * y + x;
            tri.idx[1] = 4 * y + x + 1;
            tri.idx[2] = 4 * y + x + 4;
            meshlet.Triangles.push_back(tri);

            tri.idx[0] = 4 * y + x + 5;
            tri.idx[1] = 4 * y + x + 4;
            tri.idx[2] = 4 * y + x + 1;
            meshlet.Triangles.push_back(tri);
        }
    }
    meshlet.MarkBorderVertices();
    meshlet.BuildVertexTriangleIndex();
    meshlet.Decimate();
    return 0;
}
#endif
