#pragma once

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
