#include "UtilD3D.h"

bool useWarpDevice = false;

inline PFence pFrameFence[FRAME_COUNT];
inline UINT64 nextFenceValue[FRAME_COUNT];
inline RaiiHandle hFenceEvent[FRAME_COUNT];

ComPtr<IDXGIAdapter1> GetHWAdapter(ComPtr<IDXGIFactory1> pFactory1)
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

void CreateDepthBuffer(UINT width, UINT height)
{
    CD3DX12_HEAP_PROPERTIES dsvProps(D3D12_HEAP_TYPE_DEFAULT);
    auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, width, height, 1, 0, 1, 0,
                                             D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 0.0f;
    ThrowIfFailed(pDevice->CreateCommittedResource(&dsvProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
                                                   IID_PPV_ARGS(&pDepthBuffer)));
    D3D12_DEPTH_STENCIL_VIEW_DESC viewDesc = {};
    viewDesc.Format = DXGI_FORMAT_D32_FLOAT;
    viewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    viewDesc.Flags = D3D12_DSV_FLAG_NONE;
    viewDesc.Texture2D.MipSlice = 0;
    pDevice->CreateDepthStencilView(pDepthBuffer.Get(), &viewDesc, pDsvHeap->GetCPUDescriptorHandleForHeapStart());
}

void LoadPipeline(UINT width, UINT height)
{
#ifdef _DEBUG
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

    D3D12_COMMAND_QUEUE_DESC commandQueueDesc = {};
    commandQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    commandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(pDevice->CreateCommandQueue(&commandQueueDesc, IID_PPV_ARGS(&pCommandQueueDirect)));

    DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
    swapChainDesc.BufferCount = FRAME_COUNT;
    swapChainDesc.BufferDesc.Width = width;
    swapChainDesc.BufferDesc.Height = height;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.OutputWindow = hWnd;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.Windowed = TRUE;
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    ComPtr<IDXGISwapChain> pSwapChain0;
    ThrowIfFailed(pFactory->CreateSwapChain(pCommandQueueDirect.Get(), &swapChainDesc, &pSwapChain0));
    ThrowIfFailed(pSwapChain0.As(&pSwapChain));

    ThrowIfFailed(pFactory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER));

    curFrame = pSwapChain->GetCurrentBackBufferIndex();

    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
        rtvDesc.NumDescriptors = FRAME_COUNT;
        rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        ThrowIfFailed(pDevice->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&pRtvHeap)));
        rtvDescSize = pDevice->GetDescriptorHandleIncrementSize(rtvDesc.Type);
    }

    {
        D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
        dsvDesc.NumDescriptors = 1;
        dsvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
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

    ThrowIfFailed(pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&pCommandAllocator)));

    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        ThrowIfFailed(pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pFrameFence[i])));
        nextFenceValue[i] = 1;

        hFenceEvent[i] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!hFenceEvent[i].Get())
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
    }

    ThrowIfFailed(pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, pCommandAllocator.Get(),
                                             nullptr, IID_PPV_ARGS(&pCommandList)));
    ThrowIfFailed(pCommandList->Close());
}

void WaitForFrame(UINT frame)
{
    UINT64 value = nextFenceValue[frame]++;
    ID3D12Fence *pFence = pFrameFence[frame].Get();
    HANDLE hEvent = hFenceEvent->Get();

    ThrowIfFailed(pCommandQueueDirect->Signal(pFence, value));

    if (pFence->GetCompletedValue() < value)
    {
        ThrowIfFailed(pFence->SetEventOnCompletion(value, hEvent));
        WaitForSingleObject(hEvent, INFINITE);
    }
}

void WaitForCurFrame()
{
    WaitForFrame(curFrame);
    curFrame = pSwapChain->GetCurrentBackBufferIndex();
}

void WaitForLastFrame()
{
    curFrame = pSwapChain->GetCurrentBackBufferIndex();
    WaitForFrame(curFrame);
}

void WaitForAllFrames()
{
    for (UINT i = 0; i < FRAME_COUNT; ++i)
        WaitForFrame(i);
}

void UpdateRenderTargetSize(UINT width, UINT height)
{
    width = max(1, width);
    height = max(1, height);
    ThrowIfFailed(pSwapChain->ResizeBuffers(FRAME_COUNT, width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
                                            DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING));
    CreateDepthBuffer(width, height);
}

PResource GetCurRenderTarget()
{
    return pRenderTargets[curFrame];
}
