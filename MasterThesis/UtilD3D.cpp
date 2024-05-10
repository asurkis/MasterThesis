#include "stdafx.h"

#include "UtilD3D.h"
#include <DirectXMath.h>

using namespace DirectX;

bool useWarpDevice = false;

struct TFence
{
    PCommandQueue       pCommandQueue;
    ComPtr<ID3D12Fence> pFence;
    UINT64              nextValue;
    TRaiiHandle         hFenceEvent;

    void Init(PCommandQueue pCommandQueue)
    {
        this->pCommandQueue = pCommandQueue;

        ThrowIfFailed(pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pFence)));
        nextValue = 1;

        hFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!hFenceEvent.Get())
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
    }

    void Wait()
    {
        UINT64 value  = nextValue++;
        HANDLE hEvent = hFenceEvent.Get();

        ThrowIfFailed(pCommandQueue->Signal(pFence.Get(), value));

        if (pFence->GetCompletedValue() < value)
        {
            ThrowIfFailed(pFence->SetEventOnCompletion(value, hEvent));
            WaitForSingleObject(hEvent, INFINITE);
        }
    }
};

TFence frameFences[FRAME_COUNT];
TFence computeFence;

static ComPtr<IDXGIAdapter1> GetHWAdapter(ComPtr<IDXGIFactory1> pFactory1)
{
    ComPtr<IDXGIFactory6> pFactory;
    ThrowIfFailed(pFactory1.As(&pFactory));

    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> pAdapter;
        if (FAILED(
                pFactory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&pAdapter))))
            break;

        DXGI_ADAPTER_DESC1 desc = {};
        pAdapter->GetDesc1(&desc);

        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;

        if (SUCCEEDED(D3D12CreateDevice(pAdapter.Get(), NEEDED_FEATURE_LEVEL, _uuidof(ID3D12Device), nullptr)))
            return pAdapter;
    }

    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> pAdapter = nullptr;
        if (FAILED(pFactory->EnumAdapters1(i, &pAdapter)))
            break;

        DXGI_ADAPTER_DESC1 desc = {};
        pAdapter->GetDesc1(&desc);

        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;

        if (SUCCEEDED(D3D12CreateDevice(pAdapter.Get(), NEEDED_FEATURE_LEVEL, _uuidof(ID3D12Device), nullptr)))
            return pAdapter;
    }

    return nullptr;
}

static void CreateDepthBuffer(UINT width, UINT height)
{
    CD3DX12_HEAP_PROPERTIES dsvProps(D3D12_HEAP_TYPE_DEFAULT);
    auto                    desc       = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT,
                                             width,
                                             height,
                                             1,
                                             0,
                                             1,
                                             0,
                                             D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12_CLEAR_VALUE       clearValue = {};
    clearValue.Format                  = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth      = 0.0f;
    ThrowIfFailed(pDevice->CreateCommittedResource(&dsvProps,
                                                   D3D12_HEAP_FLAG_NONE,
                                                   &desc,
                                                   D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                   &clearValue,
                                                   IID_PPV_ARGS(&pDepthBuffer)));
    D3D12_DEPTH_STENCIL_VIEW_DESC viewDesc = {};
    viewDesc.Format                        = DXGI_FORMAT_D32_FLOAT;
    viewDesc.ViewDimension                 = D3D12_DSV_DIMENSION_TEXTURE2D;
    viewDesc.Flags                         = D3D12_DSV_FLAG_NONE;
    viewDesc.Texture2D.MipSlice            = 0;
    pDevice->CreateDepthStencilView(pDepthBuffer.Get(), &viewDesc, pDsvHeap->GetCPUDescriptorHandleForHeapStart());
}

PResource CreateGenericBuffer(UINT64 width, D3D12_RESOURCE_STATES initState, D3D12_HEAP_TYPE heapType)
{
    PResource               pBuffer;
    CD3DX12_HEAP_PROPERTIES heapProps(heapType);
    auto                    desc = CD3DX12_RESOURCE_DESC::Buffer(width);
    ThrowIfFailed(pDevice->CreateCommittedResource(&heapProps,
                                                   D3D12_HEAP_FLAG_NONE,
                                                   &desc,
                                                   initState,
                                                   nullptr,
                                                   IID_PPV_ARGS(&pBuffer)));
    return std::move(pBuffer);
}

void LoadPipeline(UINT width, UINT height)
{
    // #ifdef _DEBUG
#if true
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
            OutputDebugStringW(L"Enabled debug layer\n");
        }
    }
#endif

    ComPtr<IDXGIFactory4> pFactory;
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&pFactory)));
    if (useWarpDevice)
    {
        ComPtr<IDXGIAdapter> pAdapter;
        ThrowIfFailed(pFactory->EnumWarpAdapter(IID_PPV_ARGS(&pAdapter)));
        ThrowIfFailed(D3D12CreateDevice(pAdapter.Get(), NEEDED_FEATURE_LEVEL, IID_PPV_ARGS(&pDevice)));
    }
    else
    {
        ComPtr<IDXGIAdapter1> pAdapter = GetHWAdapter(pFactory);
        ThrowIfFailed(D3D12CreateDevice(pAdapter.Get(), NEEDED_FEATURE_LEVEL, IID_PPV_ARGS(&pDevice)));
    }

    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel[] = {D3D_SHADER_MODEL_6_5};
    if (FAILED(pDevice->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, shaderModel, sizeof(shaderModel))))
        throw std::runtime_error("Shader model 6.5 is not supported");

    D3D12_FEATURE_DATA_D3D12_OPTIONS7 features = {};
    if (FAILED(pDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &features, sizeof(features))))
        throw std::runtime_error("Feature level 7 is not supported");

    if (features.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED)
        throw std::runtime_error("Mesh shaders not supported");

    D3D12_COMMAND_QUEUE_DESC commandQueueDesc = {};
    commandQueueDesc.Flags                    = D3D12_COMMAND_QUEUE_FLAG_NONE;
    commandQueueDesc.Type                     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(pDevice->CreateCommandQueue(&commandQueueDesc, IID_PPV_ARGS(&pCommandQueueDirect)));

    commandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ThrowIfFailed(pDevice->CreateCommandQueue(&commandQueueDesc, IID_PPV_ARGS(&pCommandQueueCompute)));

    DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
    swapChainDesc.BufferCount          = FRAME_COUNT;
    swapChainDesc.BufferDesc.Width     = width;
    swapChainDesc.BufferDesc.Height    = height;
    swapChainDesc.BufferDesc.Format    = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage          = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect           = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.OutputWindow         = hWnd;
    swapChainDesc.SampleDesc.Count     = 1;
    swapChainDesc.SampleDesc.Count     = 1;
    swapChainDesc.Windowed             = TRUE;
    swapChainDesc.Flags                = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    ComPtr<IDXGISwapChain> pSwapChain0;
    ThrowIfFailed(pFactory->CreateSwapChain(pCommandQueueDirect.Get(), &swapChainDesc, &pSwapChain0));
    ThrowIfFailed(pSwapChain0.As(&pSwapChain));

    ThrowIfFailed(pFactory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER));

    curFrame = pSwapChain->GetCurrentBackBufferIndex();

    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
        rtvDesc.NumDescriptors             = FRAME_COUNT;
        rtvDesc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        rtvDesc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        ThrowIfFailed(pDevice->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&pRtvHeap)));
        rtvDescSize = pDevice->GetDescriptorHandleIncrementSize(rtvDesc.Type);
    }

    {
        D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
        dsvDesc.NumDescriptors             = 1;
        dsvDesc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        dsvDesc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        ThrowIfFailed(pDevice->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&pDsvHeap)));
        dsvDescSize = pDevice->GetDescriptorHandleIncrementSize(dsvDesc.Type);
    }

    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE handle(pRtvHeap->GetCPUDescriptorHandleForHeapStart());
        for (UINT i = 0; i < FRAME_COUNT; ++i)
        {
            ThrowIfFailed(pSwapChain->GetBuffer(i, IID_PPV_ARGS(&pRenderTargets[i])));
            pDevice->CreateRenderTargetView(pRenderTargets[i].Get(), nullptr, handle);
            handle.Offset(1, rtvDescSize);
        }
    }

    CreateDepthBuffer(width, height);

    ThrowIfFailed(
        pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&pCommandAllocatorDirect)));
    ThrowIfFailed(
        pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&pCommandAllocatorCompute)));

    for (UINT i = 0; i < FRAME_COUNT; ++i)
        frameFences[i].Init(pCommandQueueDirect);
    computeFence.Init(pCommandQueueCompute);

    ThrowIfFailed(pDevice->CreateCommandList(0,
                                             D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             pCommandAllocatorDirect.Get(),
                                             nullptr,
                                             IID_PPV_ARGS(&pCommandListDirect)));
    ThrowIfFailed(pCommandListDirect->Close());

    ThrowIfFailed(pDevice->CreateCommandList(0,
                                             D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                             pCommandAllocatorCompute.Get(),
                                             nullptr,
                                             IID_PPV_ARGS(&pCommandListCompute)));
    ThrowIfFailed(pCommandListCompute->Close());
}

void WaitForCurFrame()
{
    frameFences[curFrame].Wait();
    curFrame = pSwapChain->GetCurrentBackBufferIndex();
}

void WaitForLastFrame()
{
    curFrame = pSwapChain->GetCurrentBackBufferIndex();
    frameFences[curFrame].Wait();
}

void WaitForAllFrames()
{
    for (UINT i = 0; i < FRAME_COUNT; ++i)
        frameFences[i].Wait();
}

void ExecuteCommandList()
{
    ID3D12CommandList *ppCommandLists[] = {pCommandListDirect.Get()};
    pCommandQueueDirect->ExecuteCommandLists(1, ppCommandLists);
}

void UpdateRenderTargetSize(UINT width, UINT height)
{
    width  = max(1, width);
    height = max(1, height);
    ThrowIfFailed(pSwapChain->ResizeBuffers(FRAME_COUNT,
                                            width,
                                            height,
                                            DXGI_FORMAT_R8G8B8A8_UNORM,
                                            DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING));
    CreateDepthBuffer(width, height);
}

static PResource GetCurRenderTarget() { return pRenderTargets[curFrame]; }

void TMonoPipeline::Load(const std::filesystem::path &pathVS, const std::filesystem::path &pathPS)
{
    PPipelineState pPipelineState;
    PRootSignature pRootSignature;

    std::vector<BYTE> bytecodeVS = ReadFile(pathVS);
    std::vector<BYTE> bytecodePS = ReadFile(pathPS);

    ThrowIfFailed(pDevice->CreateRootSignature(0, bytecodeVS.data(), bytecodeVS.size(), IID_PPV_ARGS(&pRootSignature)));

    enum
    {
        IE_POSITION = 0,
        IE_NORMAL,
        IE_INSTANCE_OFFSET,
        IE_TOTAL
    };

    D3D12_INPUT_ELEMENT_DESC inputElementDesc[IE_TOTAL]       = {};
    inputElementDesc[IE_POSITION].SemanticName                = "POSITION";
    inputElementDesc[IE_POSITION].Format                      = DXGI_FORMAT_R32G32B32_FLOAT;
    inputElementDesc[IE_POSITION].InputSlot                   = 0;
    inputElementDesc[IE_POSITION].AlignedByteOffset           = 0;
    inputElementDesc[IE_POSITION].InputSlotClass              = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    inputElementDesc[IE_NORMAL].SemanticName                  = "NORMAL";
    inputElementDesc[IE_NORMAL].Format                        = DXGI_FORMAT_R32G32B32_FLOAT;
    inputElementDesc[IE_NORMAL].InputSlot                     = 0;
    inputElementDesc[IE_NORMAL].AlignedByteOffset             = sizeof(float3);
    inputElementDesc[IE_NORMAL].InputSlotClass                = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    inputElementDesc[IE_INSTANCE_OFFSET].SemanticName         = "INSTANCE_OFFSET";
    inputElementDesc[IE_INSTANCE_OFFSET].Format               = DXGI_FORMAT_R32G32B32_FLOAT;
    inputElementDesc[IE_INSTANCE_OFFSET].InputSlot            = 1;
    inputElementDesc[IE_INSTANCE_OFFSET].AlignedByteOffset    = 0;
    inputElementDesc[IE_INSTANCE_OFFSET].InputSlotClass       = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
    inputElementDesc[IE_INSTANCE_OFFSET].InstanceDataStepRate = 1;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature                     = pRootSignature.Get();
    psoDesc.VS.pShaderBytecode                 = bytecodeVS.data();
    psoDesc.VS.BytecodeLength                  = bytecodeVS.size();
    psoDesc.PS.pShaderBytecode                 = bytecodePS.data();
    psoDesc.PS.BytecodeLength                  = bytecodePS.size();
    psoDesc.BlendState                         = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask                         = UINT_MAX;
    psoDesc.RasterizerState                    = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode           = D3D12_CULL_MODE_FRONT;
    psoDesc.DepthStencilState                  = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthFunc        = D3D12_COMPARISON_FUNC_GREATER;
    psoDesc.InputLayout.pInputElementDescs     = inputElementDesc;
    psoDesc.InputLayout.NumElements            = IE_TOTAL;
    psoDesc.PrimitiveTopologyType              = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets                   = 1;
    psoDesc.RTVFormats[0]                      = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat                          = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count                   = 1;
    psoDesc.SampleDesc.Quality                 = 0;
    psoDesc.CachedPSO                          = {};
    psoDesc.Flags                              = D3D12_PIPELINE_STATE_FLAG_NONE;

    ThrowIfFailed(pDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pPipelineState)));

    this->pPipelineState = std::move(pPipelineState);
    this->pRootSignature = std::move(pRootSignature);
}

void TMeshletPipeline::LoadBytecode(const std::vector<BYTE> &bytecodeAS,
                                    const std::vector<BYTE> &bytecodeMS,
                                    const std::vector<BYTE> &bytecodePS)
{
    PPipelineState pPipelineState;
    PRootSignature pRootSignature;

    ThrowIfFailed(pDevice->CreateRootSignature(0, bytecodeMS.data(), bytecodeMS.size(), IID_PPV_ARGS(&pRootSignature)));

    D3DX12_MESH_SHADER_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature                         = pRootSignature.Get();
    psoDesc.AS.pShaderBytecode                     = bytecodeAS.data();
    psoDesc.AS.BytecodeLength                      = bytecodeAS.size();
    psoDesc.MS.pShaderBytecode                     = bytecodeMS.data();
    psoDesc.MS.BytecodeLength                      = bytecodeMS.size();
    psoDesc.PS.pShaderBytecode                     = bytecodePS.data();
    psoDesc.PS.BytecodeLength                      = bytecodePS.size();
    psoDesc.BlendState                             = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask                             = UINT_MAX;
    psoDesc.RasterizerState                        = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.FillMode               = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode               = D3D12_CULL_MODE_FRONT;
    psoDesc.DepthStencilState                      = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthFunc            = D3D12_COMPARISON_FUNC_GREATER;
    psoDesc.PrimitiveTopologyType                  = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
    psoDesc.NumRenderTargets                       = 1;
    psoDesc.RTVFormats[0]                          = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat                              = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count                       = 1;
    psoDesc.SampleDesc.Quality                     = 0;
    psoDesc.CachedPSO                              = {};
    psoDesc.Flags                                  = D3D12_PIPELINE_STATE_FLAG_NONE;

    CD3DX12_PIPELINE_MESH_STATE_STREAM psoStream(psoDesc);

    D3D12_PIPELINE_STATE_STREAM_DESC streamDesc = {};
    streamDesc.pPipelineStateSubobjectStream    = &psoStream;
    streamDesc.SizeInBytes                      = sizeof(psoStream);

    ThrowIfFailed(pDevice->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&pPipelineState)));

    this->pPipelineState = std::move(pPipelineState);
    this->pRootSignature = std::move(pRootSignature);
}

void TMeshletPipeline::Load(const std::filesystem::path &pathMS, const std::filesystem::path &pathPS)
{
    std::vector<BYTE> dataMS = ReadFile(pathMS);
    std::vector<BYTE> dataPS = ReadFile(pathPS);
    LoadBytecode({}, dataMS, dataPS);
}

void TMeshletPipeline::Load(const std::filesystem::path &pathMS,
                            const std::filesystem::path &pathPS,
                            const std::filesystem::path &pathAS)
{
    std::vector<BYTE> dataMS = ReadFile(pathMS);
    std::vector<BYTE> dataPS = ReadFile(pathPS);
    std::vector<BYTE> dataAS = ReadFile(pathAS);
    LoadBytecode(dataAS, dataMS, dataPS);
}

void TComputePipeline::Load(const std::filesystem::path &pathCS)
{
    std::vector<BYTE> bytecodeCS = ReadFile(pathCS);

    PPipelineState pPipelineState;
    PRootSignature pRootSignature;

    ThrowIfFailed(pDevice->CreateRootSignature(0, bytecodeCS.data(), bytecodeCS.size(), IID_PPV_ARGS(&pRootSignature)));

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature                    = pRootSignature.Get();
    psoDesc.CS.pShaderBytecode                = bytecodeCS.data();
    psoDesc.CS.BytecodeLength                 = bytecodeCS.size();
    psoDesc.Flags                             = D3D12_PIPELINE_STATE_FLAG_NONE;

    ThrowIfFailed(pDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pPipelineState)));

    this->pPipelineState = std::move(pPipelineState);
    this->pRootSignature = std::move(pRootSignature);
}

template <typename T>
static void QueryUploadVector(const std::vector<T> &data, PResource *ppBuffer, PResource *ppUpload)
{
    size_t dataSize = sizeof(T) * data.size();
    UINT64 bufWidth = (dataSize + 3) / 4 * 4;

    *ppBuffer = CreateGenericBuffer(bufWidth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_HEAP_TYPE_DEFAULT);
    *ppUpload = CreateGenericBuffer(bufWidth);

    void *memory = nullptr;
    ThrowIfFailed((*ppUpload)->Map(0, nullptr, &memory));
    std::memcpy(memory, data.data(), dataSize);
    (*ppUpload)->Unmap(0, nullptr);

    pCommandListDirect->CopyResource(ppBuffer->Get(), ppUpload->Get());
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(ppBuffer->Get(),
                                                        D3D12_RESOURCE_STATE_COPY_DEST,
                                                        D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    pCommandListDirect->ResourceBarrier(1, &barrier);
}

void TMonoLodGPU::Upload(const TMonoLodCPU &model)
{
    PResource pUploadVertices;
    PResource pUploadIndices;

    mNIndices = model.Indices.size();

    ThrowIfFailed(pCommandListDirect->Reset(pCommandAllocatorDirect.Get(), nullptr));
    QueryUploadVector(model.Vertices, &pVertices, &pUploadVertices);
    QueryUploadVector(model.Indices, &pIndices, &pUploadIndices);
    ThrowIfFailed(pCommandListDirect->Close());
    ExecuteCommandList();

    TFence fence;
    fence.Init(pCommandQueueDirect);
    fence.Wait();

    mVertexBufferView.BufferLocation = pVertices->GetGPUVirtualAddress();
    mVertexBufferView.SizeInBytes    = sizeof(TVertex) * model.Vertices.size();
    mVertexBufferView.StrideInBytes  = sizeof(TVertex);

    mIndexBufferView.BufferLocation = pIndices->GetGPUVirtualAddress();
    mIndexBufferView.SizeInBytes    = sizeof(uint) * model.Indices.size();
    mIndexBufferView.Format         = DXGI_FORMAT_R32_UINT;
}

void TMonoLodGPU::Render(UINT InstanceCount, UINT StartInstance) const
{
    if (InstanceCount == 0)
        return;
    pCommandListDirect->IASetIndexBuffer(&mIndexBufferView);
    pCommandListDirect->IASetVertexBuffers(0, 1, &mVertexBufferView);
    pCommandListDirect->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    pCommandListDirect->DrawIndexedInstanced(mNIndices, InstanceCount, 0, 0, StartInstance);
}

void TMonoModelGPU::InitBBox(const TMonoLodCPU &lod)
{
    mBBoxMin = XMVectorSet(INFINITY, INFINITY, INFINITY, 0.0f);
    mBBoxMax = XMVectorSet(-INFINITY, -INFINITY, -INFINITY, 0.0f);
    for (const TVertex &vert : lod.Vertices)
    {
        mBBoxMin = XMVectorMin(mBBoxMin, XMLoadFloat3(&vert.Position));
        mBBoxMax = XMVectorMax(mBBoxMax, XMLoadFloat3(&vert.Position));
    }
}

size_t TMonoModelGPU::PickLod(float3 pos) const
{
    float4 center   = XMLoadFloat3(&pos) + 0.5 * (mBBoxMin + mBBoxMax);
    float  diameter = XMVectorGetX(XMVector3Length(mBBoxMax - mBBoxMin));
    float  distance = XMVectorGetX(XMVector3Length(center - MainData.CameraPos));
    float  radius   = 0.5f * diameter;
    if (distance < radius)
        return 0;
    float angularRadius     = radius / distance; // approximate
    float pixelRadius       = angularRadius * XMMin(WindowWidth, WindowHeight);
    float threshold         = XMVectorGetW(MainData.FloatInfo);
    float preferredVertices = pixelRadius * pixelRadius / threshold;
    for (size_t iLod = 1; iLod < LodCount(); ++iLod)
    {
        if (mLods[iLod].IndexCount() < preferredVertices)
            return iLod - 1;
    }
    return LodCount() - 1;
}

void TMonoModelGPU::LoadGLBs(std::string_view basePath, size_t nLods, size_t nMaxInstances)
{
    mLods.resize(nLods);
    for (size_t iLod = 0; iLod < nLods; ++iLod)
    {
        std::ostringstream lodPath;
        lodPath << basePath << "_LOD" << iLod << ".glb";
        TMonoLodCPU lodCPU;
        lodCPU.LoadGLB(lodPath.str());
        if (iLod == 0)
            InitBBox(lodCPU);
        mLods[iLod].Upload(lodCPU);
    }

    mInstances.resize(nMaxInstances);
    mInstancesOrdered.resize(nMaxInstances);
    mPickedLods.resize(nMaxInstances);
    mLodOffset.resize(nLods + 1);

    pInstanceBuffer = CreateGenericBuffer(nMaxInstances * sizeof(float3));

    mInstanceBufferView                = {};
    mInstanceBufferView.BufferLocation = pInstanceBuffer->GetGPUVirtualAddress();
    mInstanceBufferView.SizeInBytes    = nMaxInstances * sizeof(float3);
    mInstanceBufferView.StrideInBytes  = sizeof(float3);
}

void TMonoModelGPU::Reset(int displayType)
{
    mNInstances  = 0;
    mDisplayType = displayType;
}

void TMonoModelGPU::Instance(float3 pos)
{
    size_t pickedLod = 0;
    if (mDisplayType >= 0)
        pickedLod = mDisplayType;
    else
        pickedLod = PickLod(pos);
    mInstances[mNInstances]  = pos;
    mPickedLods[mNInstances] = pickedLod;
    ++mNInstances;
}

void TMonoModelGPU::Commit()
{
    if (mNInstances == 0)
        return;

    std::fill(mLodOffset.begin(), mLodOffset.end(), 0);
    for (size_t i = 0; i < mNInstances; ++i)
        ++mLodOffset[mPickedLods[i] + 1];
    for (size_t i = 2; i <= LodCount(); ++i)
        mLodOffset[i] += mLodOffset[i - 1];
    for (size_t i = 0; i < mNInstances; ++i)
        mInstancesOrdered[mLodOffset[mPickedLods[i]]++] = mInstances[i];
    for (size_t i = LodCount(); i > 0; --i)
        mLodOffset[i] = mLodOffset[i - 1];
    mLodOffset[0] = 0;

    void         *pInstanceDataBegin = nullptr;
    CD3DX12_RANGE readRange(0, 0);
    ThrowIfFailed(pInstanceBuffer->Map(0, &readRange, &pInstanceDataBegin));
    memcpy(pInstanceDataBegin, mInstancesOrdered.data(), sizeof(float3) * mNInstances);
    CD3DX12_RANGE writtenRange(0, sizeof(float3) * mNInstances);
    pInstanceBuffer->Unmap(0, &writtenRange);

    pCommandListDirect->IASetVertexBuffers(1, 1, &mInstanceBufferView);
    for (size_t iLod = 0; iLod < LodCount(); ++iLod)
    {
        size_t lodBeg = mLodOffset[iLod];
        size_t lodEnd = mLodOffset[iLod + 1];
        size_t lodN   = lodEnd - lodBeg;
        mLods[iLod].Render(lodN, lodBeg);
    }

    /*
    void         *pInstanceDataBegin = nullptr;
    CD3DX12_RANGE emptyRange(0, 0);
    ThrowIfFailed(pInstanceBuffer->Map(0, &emptyRange, &pInstanceDataBegin));
    memcpy(pInstanceDataBegin, mInstances.data(), sizeof(float3) * mNInstances);
    pInstanceBuffer->Unmap(0, nullptr);

    pCommandListDirect->IASetVertexBuffers(1, 1, &mInstanceBufferView);
    for (size_t iInstance = 0; iInstance < mNInstances; ++iInstance)
    {
        size_t iLod = mPickedLods[iInstance];
        mLods[iLod].Render(1, iInstance);
    }
    */
}

void TMeshletModelGPU::Upload(const TMeshletModelCPU &model, uint nMaxInstances)
{
    PResource pUploadVertices;
    PResource pUploadPrimitives;
    PResource pUploadMeshlets;
    PResource pUploadMeshletBoxes;
    PResource pUploadParents;
    PResource pUploadChildren;

    mMeshes = model.Meshes;

    ThrowIfFailed(pCommandListDirect->Reset(pCommandAllocatorDirect.Get(), nullptr));

    std::vector<TVertex> appliedVertices;
    appliedVertices.reserve(model.GlobalIndices.size());
    for (uint iVert : model.GlobalIndices)
        appliedVertices.push_back(model.Vertices[iVert & UINT32_C(0x7FFFFFFF)]);

    mRoots.clear();
    for (size_t iMeshlet = 0; iMeshlet < model.Meshlets.size(); ++iMeshlet)
    {
        const TMeshletDesc &meshlet = model.Meshlets[iMeshlet];
        if (meshlet.ParentCount == 0)
            mRoots.push_back(iMeshlet);
    }

    // x = count, y = offset
    std::vector<uint2> parentDescs(model.Meshlets.size(), uint2(0, 0));
    std::vector<uint>  children;
    for (const TMeshletDesc &meshlet : model.Meshlets)
    {
        for (size_t iiParent = 0; iiParent < meshlet.ParentCount; ++iiParent)
        {
            size_t iParent = meshlet.ParentOffset + iiParent;
            parentDescs[iParent].x++;
            children.push_back(0);
        }
    }
    for (size_t i = 1; i < model.Meshlets.size(); ++i)
    {
        parentDescs[i].y     = parentDescs[i - 1].x + parentDescs[i - 1].y;
        parentDescs[i - 1].x = 0;
    }
    parentDescs[model.Meshlets.size() - 1].x = 0;
    for (size_t iMeshlet = 0; iMeshlet < model.Meshlets.size(); ++iMeshlet)
    {
        const TMeshletDesc &meshlet = model.Meshlets[iMeshlet];
        for (uint iiParent = 0; iiParent < meshlet.ParentCount; ++iiParent)
        {
            uint iParent  = meshlet.ParentOffset + iiParent;
            uint pos      = parentDescs[iParent].x++ + parentDescs[iParent].y;
            children[pos] = iMeshlet;
        }
    }

    uint MaxVertCount = 0;
    uint MaxPrimCount = 0;
    for (const TMeshletDesc &meshlet : model.Meshlets)
    {
        MaxVertCount = XMMax(MaxVertCount, meshlet.VertCount);
        MaxPrimCount = XMMax(MaxPrimCount, meshlet.PrimCount);
    }
    {
        std::wstringstream oss;
        oss << L"MaxVertCount = " << MaxVertCount << L"\nMaxPrimCount = " << MaxPrimCount << L"\n";
        std::wstring str = oss.str();
        OutputDebugStringW(str.c_str());
    }

    QueryUploadVector(appliedVertices, &pVertices, &pUploadVertices);
    QueryUploadVector(model.Primitives, &pPrimitives, &pUploadPrimitives);
    QueryUploadVector(model.Meshlets, &pMeshlets, &pUploadMeshlets);
    QueryUploadVector(model.MeshletBoxes, &pMeshletBoxes, &pUploadMeshletBoxes);
    QueryUploadVector(parentDescs, &pParents, &pUploadParents);
    QueryUploadVector(children, &pChildren, &pUploadChildren);
    ThrowIfFailed(pCommandListDirect->Close());
    ExecuteCommandList();

    uint nMaxMeshlets = nMaxInstances * model.Meshlets.size();
    pTasks            = CreateGenericBuffer(20);
    pQueue            = CreateGenericBuffer(sizeof(uint) * nMaxMeshlets);

    pInstancedMeshlets
        = CreateGenericBuffer(sizeof(uint4[2]) * nMaxMeshlets, D3D12_RESOURCE_STATE_COMMON, D3D12_HEAP_TYPE_DEFAULT);

    TFence fence;
    fence.Init(pCommandQueueDirect);
    fence.Wait();

    mMaxLayer = 0;
    for (size_t iMesh = 0; iMesh < mMeshes.size(); ++iMesh)
    {
        size_t iLastMeshlet = mMeshes[iMesh].MeshletCount - 1;
        uint   iLayer       = model.Meshlets[iLastMeshlet].Height;
        mMaxLayer           = (std::max)(mMaxLayer, iLayer);
    }
}

void TMeshletModelGPU::Reset(uint nInstances)
{
    uint          nQueued = 0;
    void         *pMapped = nullptr;
    CD3DX12_RANGE emptyRange(0, 0);
    ThrowIfFailed(pQueue->Map(0, &emptyRange, &pMapped));
    {
        auto pQueue = reinterpret_cast<uint2 *>(pMapped);
        for (uint iInstance = 0; iInstance < nInstances; ++iInstance)
        {
            for (uint iMeshlet : mRoots)
                *pQueue++ = uint2(iMeshlet, iInstance);
        }
    }
    pQueue->Unmap(0, nullptr);
    ThrowIfFailed(pTasks->Map(0, &emptyRange, &pMapped));
    {
        auto pTasks = reinterpret_cast<uint *>(pMapped);
        pTasks[0]   = 0;
        pTasks[1]   = nInstances * mRoots.size();
        pTasks[2]   = pTasks[1];
        pTasks[3]   = 0;
        pTasks[4]   = 0;
    }
    pTasks->Unmap(0, nullptr);

    pCommandListCompute->SetComputeRootShaderResourceView(1, pMeshlets->GetGPUVirtualAddress());
    pCommandListCompute->SetComputeRootShaderResourceView(2, pMeshletBoxes->GetGPUVirtualAddress());
    pCommandListCompute->SetComputeRootShaderResourceView(3, pParents->GetGPUVirtualAddress());
    pCommandListCompute->SetComputeRootShaderResourceView(4, pChildren->GetGPUVirtualAddress());
    pCommandListCompute->SetComputeRootUnorderedAccessView(5, pTasks->GetGPUVirtualAddress());
    pCommandListCompute->SetComputeRootUnorderedAccessView(6, pQueue->GetGPUVirtualAddress());
    pCommandListCompute->SetComputeRootUnorderedAccessView(7, pInstancedMeshlets->GetGPUVirtualAddress());
    // ��������� ����� ���������, ����� ������� �� �������� ����������
    pCommandListCompute->Dispatch(256, 1, 1);

    ThrowIfFailed(pCommandListCompute->Close());
    ID3D12CommandList *ppCommandLists[] = {pCommandListCompute.Get()};
    pCommandQueueCompute->ExecuteCommandLists(1, ppCommandLists);
    computeFence.Wait();

    ThrowIfFailed(pTasks->Map(0, nullptr, &pMapped));
    mNInstancedMeshlets = reinterpret_cast<uint *>(pMapped)[4];
    pTasks->Unmap(0, nullptr);
}

void TMeshletModelGPU::Render(int nInstances)
{
    /*
    for (uint iMesh = 0; iMesh < mMeshes.size(); ++iMesh)
    {
        TMeshDesc &mesh = mMeshes[iMesh];
        pCommandListDirect->SetGraphicsRoot32BitConstant(1, mesh.MeshletCount, 0);
        pCommandListDirect->SetGraphicsRoot32BitConstant(1, mesh.MeshletTriangleOffsets, 1);
        pCommandListDirect->SetGraphicsRootShaderResourceView(2, pVertices->GetGPUVirtualAddress());
        // pCommandListDirect->SetGraphicsRootShaderResourceView(3, pGlobalIndices->GetGPUVirtualAddress());
        pCommandListDirect->SetGraphicsRootShaderResourceView(4, pPrimitives->GetGPUVirtualAddress());
        pCommandListDirect->SetGraphicsRootShaderResourceView(5, pMeshlets->GetGPUVirtualAddress());
        pCommandListDirect->SetGraphicsRootShaderResourceView(6, pMeshletBoxes->GetGPUVirtualAddress());

        constexpr uint GROUP_SIZE_AS = 32;

        uint nDispatch = (mesh.MeshletCount + GROUP_SIZE_AS - 1) / GROUP_SIZE_AS;
        pCommandListDirect->DispatchMesh(nDispatch, nInstances, 1);
    }
    */
    pCommandListDirect->SetGraphicsRootShaderResourceView(1, pVertices->GetGPUVirtualAddress());
    pCommandListDirect->SetGraphicsRootShaderResourceView(2, pPrimitives->GetGPUVirtualAddress());
    pCommandListDirect->SetGraphicsRootShaderResourceView(3, pInstancedMeshlets->GetGPUVirtualAddress());
    pCommandListDirect->DispatchMesh(mNInstancedMeshlets, 1, 1);
}
