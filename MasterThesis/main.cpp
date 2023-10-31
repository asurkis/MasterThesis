#include "RaiiMainWindow.h"
#include "common.h"

using PCommandAllocator = ComPtr<ID3D12CommandAllocator>;
using PCommandQueue = ComPtr<ID3D12CommandQueue>;
using PDescriptorHeap = ComPtr<ID3D12DescriptorHeap>;
using PDevice = ComPtr<ID3D12Device>;
using PResource = ComPtr<ID3D12Resource>;
using PSwapChain = ComPtr<IDXGISwapChain3>;

bool useWarpDevice = false;
PDevice pDevice;
PCommandQueue pCommandQueueDirect;
PSwapChain pSwapChain;
PCommandAllocator pCommandAllocator;

PResource renderTargets[FRAME_COUNT];

PDescriptorHeap pRtvHeap;
UINT rtvDescSize;

UINT curFrame = 0;

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
            ThrowIfFailed(pSwapChain->GetBuffer(i, IID_PPV_ARGS(&renderTargets[i])));
            pDevice->CreateRenderTargetView(renderTargets[i].Get(), nullptr, handle);
            handle.Offset(1, rtvDescSize);
        }
    }

    ThrowIfFailed(pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&pCommandAllocator)));
}

void LoadAssets()
{

}

int WINAPI wWinMain(HINSTANCE hCurInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nShowCmd)
{
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    hInstance = hCurInstance;

    try
    {
        RaiiMainWindow raiiMainWindow;

        LoadPipeline();
        LoadAssets();

        ShowWindow(hWnd, nShowCmd);
        PostQuitMessage(0);

        MSG msg = {};
        while (GetMessage(&msg, HWND_DESKTOP, 0, 0))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
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
