#include "common.h"

#include "RaiiMainWindow.h"

using PBlob = ComPtr<ID3DBlob>;
using PResource = ComPtr<ID3D12Resource>;

bool useWarpDevice = false;
ComPtr<ID3D12Device> pDevice;
ComPtr<ID3D12CommandQueue> pCommandQueueDirect;
ComPtr<IDXGISwapChain3> pSwapChain;
ComPtr<ID3D12CommandAllocator> pCommandAllocator;

PResource pRenderTargets[FRAME_COUNT];
// PResource pVertexBuffer;
// D3D12_VERTEX_BUFFER_VIEW vertexBufferView = {};

ComPtr<ID3D12DescriptorHeap> pRtvHeap;
UINT rtvDescSize;

UINT curFrame = 0;

D3D12_RECT scissorRect = {};
CD3DX12_VIEWPORT viewport;

ComPtr<ID3D12RootSignature> pRootSignature;
ComPtr<ID3D12PipelineState> pPipelineState;

ComPtr<ID3D12GraphicsCommandList> pCommandList;
ComPtr<ID3D12Fence> pFence;
UINT64 nextFenceValue = 1;
HANDLE hFenceEvent = nullptr;

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

void LoadPipeline()
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

    D3D12_COMMAND_QUEUE_DESC commandQueueDesc = {};
    commandQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    commandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(pDevice->CreateCommandQueue(&commandQueueDesc, IID_PPV_ARGS(&pCommandQueueDirect)));

    RECT wndRect = {};
    GetClientRect(hWnd, &wndRect);

    scissorRect.left = 0;
    scissorRect.top = 0;
    scissorRect.right = wndRect.right - wndRect.left;
    scissorRect.bottom = wndRect.bottom - wndRect.top;

    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = scissorRect.right;
    viewport.Height = scissorRect.bottom;
    viewport.MinDepth = D3D12_MIN_DEPTH;
    viewport.MaxDepth = D3D12_MAX_DEPTH;

    DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
    swapChainDesc.BufferCount = FRAME_COUNT;
    swapChainDesc.BufferDesc.Width = wndRect.right - wndRect.left;
    swapChainDesc.BufferDesc.Height = wndRect.bottom - wndRect.top;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.OutputWindow = hWnd;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.Windowed = TRUE;
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
        CD3DX12_CPU_DESCRIPTOR_HANDLE handle(pRtvHeap->GetCPUDescriptorHandleForHeapStart());
        for (UINT i = 0; i < FRAME_COUNT; ++i)
        {
            ThrowIfFailed(pSwapChain->GetBuffer(i, IID_PPV_ARGS(&pRenderTargets[i])));
            pDevice->CreateRenderTargetView(pRenderTargets[i].Get(), nullptr, handle);
            handle.Offset(1, rtvDescSize);
        }
    }

    ThrowIfFailed(pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&pCommandAllocator)));
}

void LoadAssets()
{
    {
        PBlob vertexShader;
        PBlob pixelShader;

#ifdef _DEBUG
        UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        UINT compileFlags = 0;
#endif

        ThrowIfFailed(D3DCompileFromFile(L"ShaderVertex.hlsl", nullptr, nullptr, "main", "vs_5_0", compileFlags, 0,
                                         &vertexShader, nullptr));
        ThrowIfFailed(D3DCompileFromFile(L"ShaderPixel.hlsl", nullptr, nullptr, "main", "ps_5_0", compileFlags, 0,
                                         &pixelShader, nullptr));
        ThrowIfFailed(pDevice->CreateRootSignature(0, vertexShader->GetBufferPointer(), vertexShader->GetBufferSize(),
                                                   IID_PPV_ARGS(&pRootSignature)));

        D3D12_INPUT_ELEMENT_DESC inputElementDesc[2];

        inputElementDesc[0].SemanticName = "POSITION";
        inputElementDesc[0].SemanticIndex = 0;
        inputElementDesc[0].Format = DXGI_FORMAT_R32G32_FLOAT;
        inputElementDesc[0].InputSlot = 0;
        inputElementDesc[0].AlignedByteOffset = 0;
        inputElementDesc[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        inputElementDesc[0].InstanceDataStepRate = 0;

        inputElementDesc[1].SemanticName = "COLOR";
        inputElementDesc[1].SemanticIndex = 0;
        inputElementDesc[1].Format = DXGI_FORMAT_R32G32B32_FLOAT;
        inputElementDesc[1].InputSlot = 0;
        inputElementDesc[1].AlignedByteOffset = 8;
        inputElementDesc[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        inputElementDesc[1].InstanceDataStepRate = 0;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = pRootSignature.Get();
        psoDesc.VS.pShaderBytecode = vertexShader->GetBufferPointer();
        psoDesc.VS.BytecodeLength = vertexShader->GetBufferSize();
        psoDesc.PS.pShaderBytecode = pixelShader->GetBufferPointer();
        psoDesc.PS.BytecodeLength = pixelShader->GetBufferSize();
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.DSVFormat = DXGI_FORMAT_R32_FLOAT;
        psoDesc.SampleDesc.Count = 1;
        psoDesc.SampleDesc.Quality = 0;
        psoDesc.InputLayout.pInputElementDescs = nullptr;
        psoDesc.InputLayout.NumElements = 0;

        ThrowIfFailed(pDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pPipelineState)));
    }

    ThrowIfFailed(pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, pCommandAllocator.Get(),
                                             pPipelineState.Get(), IID_PPV_ARGS(&pCommandList)));
    ThrowIfFailed(pCommandList->Close());

    /*
    {
        float vertices[] = {
            0.0f, 0.0f,       // position
            1.0f, 0.0f, 0.0f, // color
            0.0f, 0.5f,       // position
            0.0f, 1.0f, 0.0f, // color
            0.5f, 0.0f,       // position
            0.0f, 0.0f, 1.0f, // color
        };
        UINT vertexBufferSize = sizeof(vertices);

        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto desc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);
        ThrowIfFailed(pDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(&pVertexBuffer)));

        void *pVertexDataBegin = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        ThrowIfFailed(pVertexBuffer->Map(0, &readRange, &pVertexDataBegin));
        memcpy(pVertexDataBegin, vertices, vertexBufferSize);
        pVertexBuffer->Unmap(0, nullptr);

        vertexBufferView.BufferLocation = pVertexBuffer->GetGPUVirtualAddress();
        vertexBufferView.SizeInBytes = vertexBufferSize;
        vertexBufferView.StrideInBytes = 20;
    }
    */

    {
        ThrowIfFailed(pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pFence)));
        nextFenceValue = 1;

        hFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!hFenceEvent)
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
    }
}

void WaitForLastFrame()
{
    UINT64 value = nextFenceValue;
    ThrowIfFailed(pCommandQueueDirect->Signal(pFence.Get(), value));
    nextFenceValue++;

    if (pFence->GetCompletedValue() < value)
    {
        ThrowIfFailed(pFence->SetEventOnCompletion(value, hFenceEvent));
        WaitForSingleObject(hFenceEvent, INFINITE);
    }

    curFrame = pSwapChain->GetCurrentBackBufferIndex();
}

void FillCommandList()
{
    ThrowIfFailed(pCommandAllocator->Reset());
    ThrowIfFailed(pCommandList->Reset(pCommandAllocator.Get(), pPipelineState.Get()));
    pCommandList->SetGraphicsRootSignature(pRootSignature.Get());
    pCommandList->RSSetViewports(1, &viewport);
    pCommandList->RSSetScissorRects(1, &scissorRect);

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(pRenderTargets[curFrame].Get(), D3D12_RESOURCE_STATE_PRESENT,
                                                        D3D12_RESOURCE_STATE_RENDER_TARGET);
    pCommandList->ResourceBarrier(1, &barrier);

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(pRtvHeap->GetCPUDescriptorHandleForHeapStart(), curFrame, rtvDescSize);
    pCommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    constexpr float CLEAR_COLOR[] = {0.0f, 0.2f, 0.4f, 1.0f};
    pCommandList->ClearRenderTargetView(rtvHandle, CLEAR_COLOR, 0, nullptr);
    pCommandList->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // pCommandList->IASetVertexBuffers(0, 1, &vertexBufferView);
    pCommandList->DrawInstanced(3, 1, 0, 0);

    barrier = CD3DX12_RESOURCE_BARRIER::Transition(pRenderTargets[curFrame].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                   D3D12_RESOURCE_STATE_PRESENT);
    pCommandList->ResourceBarrier(1, &barrier);

    ThrowIfFailed(pCommandList->Close());
}

void OnRender()
{
    FillCommandList();
    ID3D12CommandList *ppCommandLists[] = {pCommandList.Get()};
    pCommandQueueDirect->ExecuteCommandLists(1, ppCommandLists);
    ThrowIfFailed(pSwapChain->Present(1, 0));
    WaitForLastFrame();
}

int WINAPI wWinMain(_In_ HINSTANCE hCurInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine,
                    _In_ int nShowCmd)
{
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    hInstance = hCurInstance;

    try
    {
        RaiiMainWindow raiiMainWindow;

        LoadPipeline();
        LoadAssets();
        WaitForLastFrame();

        ShowWindow(hWnd, nShowCmd);
        // PostQuitMessage(0);

        MSG msg = {};
        while (GetMessage(&msg, HWND_DESKTOP, 0, 0))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            OnRender();
        }

        WaitForLastFrame();
        CloseHandle(hFenceEvent);
        return msg.wParam;
    }
    catch (const std::runtime_error &err)
    {
        MessageBoxA(HWND_DESKTOP, err.what(), "Error", MB_ICONERROR | MB_OK);
        return -1;
    }
}

LRESULT WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_CLOSE:
        PostQuitMessage(0);
        break;

    case WM_KEYDOWN:
        switch (wParam)
        {
        case VK_ESCAPE:
            PostQuitMessage(0);
            break;
        }
        break;

    default:
        return DefWindowProc(hWnd, uMsg, wParam, lParam);
    }
    return 0;
}
