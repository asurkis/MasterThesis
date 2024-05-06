#pragma once

#include "UtilWin32.h"
#include "stdafx.h"

constexpr D3D_FEATURE_LEVEL NEEDED_FEATURE_LEVEL = D3D_FEATURE_LEVEL_11_0;
constexpr UINT              FRAME_COUNT          = 3;

inline ComPtr<ID3D12Device2>              pDevice;
inline ComPtr<ID3D12CommandQueue>         pCommandQueueDirect;
inline ComPtr<IDXGISwapChain3>            pSwapChain;
inline ComPtr<ID3D12CommandAllocator>     pCommandAllocator;
inline ComPtr<ID3D12GraphicsCommandList6> pCommandList;

inline PDescriptorHeap pRtvHeap;
inline UINT            rtvDescSize;

inline PDescriptorHeap pDsvHeap;
inline UINT            dsvDescSize;

inline PResource pRenderTargets[FRAME_COUNT];
inline PResource pDepthBuffer;
inline UINT      curFrame = 0;

void LoadPipeline(UINT width, UINT height);
void UpdateRenderTargetSize(UINT width, UINT height);
void WaitForCurFrame();
void WaitForLastFrame();
void WaitForAllFrames();
void ExecuteCommandList();

struct MainConstantBuffer
{
    float4x4 MatView;
    float4x4 MatProj;
    float4x4 MatViewProj;
    float4x4 MatNormal;
    float4   FloatInfo; // xyz = InstanceOffset, w = ErrorThreshold
    uint4    IntInfo;   // x = DisplayType
};

class MonoPipeline
{
    PPipelineState pPipelineState;
    PRootSignature pRootSignature;

  public:
    void Load(const std::filesystem::path &pathVS, const std::filesystem::path &pathPS);

    ID3D12PipelineState *GetStateRaw() const noexcept { return pPipelineState.Get(); }
    ID3D12RootSignature *GetRootSignatureRaw() const noexcept { return pRootSignature.Get(); }
};

class MeshletPipeline
{
    PPipelineState pPipelineState;
    PRootSignature pRootSignature;

    void LoadBytecode(const std::vector<BYTE> &dataMS,
                      const std::vector<BYTE> &dataPS,
                      const std::vector<BYTE> &dataAS);

  public:
    void Load(const std::filesystem::path &pathMS, const std::filesystem::path &pathPS);
    void Load(const std::filesystem::path &pathMS,
              const std::filesystem::path &pathPS,
              const std::filesystem::path &pathAS);

    ID3D12PipelineState *GetStateRaw() const noexcept { return pPipelineState.Get(); }
    ID3D12RootSignature *GetRootSignatureRaw() const noexcept { return pRootSignature.Get(); }
};

class MonoLodGPU
{
    PResource                pVertices;
    PResource                pIndices;
    D3D12_VERTEX_BUFFER_VIEW mVertexBufferView;
    D3D12_INDEX_BUFFER_VIEW  mIndexBufferView;
    uint                     nIndices;

  public:
    void Upload(const MonoLodCPU &model);
    void Render(const MainConstantBuffer &MainCB, float3 InstanceOffset) const;
};

class MeshletModelGPU
{
    PResource pVertices;
    PResource pGlobalIndices;
    PResource pPrimitives;
    PResource pMeshlets;
    PResource pMeshletBoxes;
    uint      mMaxLayer;

    // Пока поддерживаем отрисовку только одного меша за раз
    std::vector<MeshDesc> meshes;

  public:
    constexpr uint MaxLayer() const noexcept { return mMaxLayer; }

    void Upload(const MeshletModelCPU &model);
    void Render(int nInstances);
};
