#include "stdafx.h"

#include "UtilD3D.h"
#include "UtilWin32.h"
#include <DirectXMath.h>

#define USE_MONO_LODS

using namespace DirectX;

bool useVSync = true;

enum SrvDescriptors
{
    MY_SRV_DESC_IMGUI = 0,
    // MY_SRV_DESC_CAMERA,
    MY_SRV_DESC_TOTAL
};

PDescriptorHeap pSrvHeap;
UINT            srvDescSize;

ComPtr<ID3D12QueryHeap> pQueryHeap;
PResource               pQueryResults;

#ifdef USE_MONO_LODS
TMonoPipeline mainPipeline;

#else
TMeshletPipeline mainPipeline;
TMeshletPipeline aabbPipeline;
#endif

XMVECTOR camFocus  = XMVectorSet(-140.0f, 2010.0f, -150.0f, 0.0f);
float    camRotX   = XMConvertToRadians(-30.0f);
float    camRotY   = XMConvertToRadians(50.0f);
float    camSpeed  = 256.0f;
float    camOffset = 3.0f;

float errorThreshold = 0.25f;
int   displayType    = -1;

int    nInstances     = 64;
float3 instanceOffset = float3(400.0f, 600.0f, 400.0f);

#ifndef USE_MONO_LODS
bool drawModel       = true;
bool drawMeshletAABB = false;
#endif

PResource pMainCB;

constexpr size_t MAX_NUM_INSTANCES = 512;

#ifdef USE_MONO_LODS
TMonoModelGPU model;
#else
TMeshletModelGPU model;
#endif

static void LoadAssets()
{
    std::filesystem::path assetPath = GetAssetPath();

    {
        D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
        srvDesc.NumDescriptors             = MY_SRV_DESC_TOTAL;
        srvDesc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        srvDesc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        ThrowIfFailed(pDevice->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&pSrvHeap)));
        srvDescSize = pDevice->GetDescriptorHandleIncrementSize(srvDesc.Type);
    }

#ifdef USE_MONO_LODS
    mainPipeline.Load(assetPath / "MainVS.cso", assetPath / "MainPS.cso");
#else
    mainPipeline.Load(assetPath / "MainMS.cso", assetPath / "MainPS.cso", assetPath / "MainAS.cso");
    aabbPipeline.Load(assetPath / "AABB_MS.cso", assetPath / "AABB_PS.cso", assetPath / "MainAS.cso");
#endif

    {
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto                    desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(MainData));
        ThrowIfFailed(pDevice->CreateCommittedResource(&heapProps,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &desc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ,
                                                       nullptr,
                                                       IID_PPV_ARGS(&pMainCB)));
    }

    {
        D3D12_QUERY_HEAP_DESC desc = {};
        desc.Type                  = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
        desc.Count                 = 1;
        ThrowIfFailed(pDevice->CreateQueryHeap(&desc, IID_PPV_ARGS(&pQueryHeap)));
    }

    {
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_READBACK);
        auto                    desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
        ThrowIfFailed(pDevice->CreateCommittedResource(&heapProps,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &desc,
                                                       D3D12_RESOURCE_STATE_COMMON,
                                                       nullptr,
                                                       IID_PPV_ARGS(&pQueryResults)));
    }

#ifdef USE_MONO_LODS
    model.LoadGLBs("../Assets/Statue", 7, MAX_NUM_INSTANCES);
#else
    TMeshletModelCPU modelCPU;
    modelCPU.LoadFromFile("../Assets/model.bin");
    model.Upload(modelCPU);
#endif
}

class TRaiiImgui
{
  public:
    TRaiiImgui()
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

        ImGui_ImplWin32_Init(hWnd);
        CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(pSrvHeap->GetCPUDescriptorHandleForHeapStart(),
                                                MY_SRV_DESC_IMGUI,
                                                srvDescSize);
        CD3DX12_GPU_DESCRIPTOR_HANDLE gpuHandle(pSrvHeap->GetGPUDescriptorHandleForHeapStart(),
                                                MY_SRV_DESC_IMGUI,
                                                srvDescSize);
        ImGui_ImplDX12_Init(pDevice.Get(),
                            FRAME_COUNT,
                            DXGI_FORMAT_R8G8B8A8_UNORM,
                            pSrvHeap.Get(),
                            cpuHandle,
                            gpuHandle);
    }

    ~TRaiiImgui()
    {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
};

std::optional<TRaiiImgui> raiiImgui;

static uint GetZCodeComponent3(uint x)
{
    x = x & 0x49249249;
    x = (x ^ (x >> 2)) & 0xC30C30C3;
    x = (x ^ (x >> 4)) & 0x0F00F00F;
    x = (x ^ (x >> 8)) & 0xFF0000FF;
    x = (x ^ (x >> 16)) & 0x00000FFF;
    return x;
}

static void FillCommandList()
{
    ThrowIfFailed(pCommandAllocator->Reset());
    ThrowIfFailed(pCommandList->Reset(pCommandAllocator.Get(), mainPipeline.GetStateRaw()));

    ID3D12DescriptorHeap *heapsToSet[] = {pSrvHeap.Get()};
    pCommandList->SetDescriptorHeaps(1, heapsToSet);

    D3D12_VIEWPORT viewport = {};
    viewport.TopLeftX       = 0.0f;
    viewport.TopLeftY       = 0.0f;
    viewport.Width          = float(WindowWidth);
    viewport.Height         = float(WindowHeight);
    viewport.MinDepth       = D3D12_MIN_DEPTH;
    viewport.MaxDepth       = D3D12_MAX_DEPTH;

    D3D12_RECT scissorRect = {};
    scissorRect.left       = 0;
    scissorRect.top        = 0;
    scissorRect.right      = WindowWidth;
    scissorRect.bottom     = WindowHeight;

    pCommandList->RSSetViewports(1, &viewport);
    pCommandList->RSSetScissorRects(1, &scissorRect);

    CD3DX12_RESOURCE_BARRIER barriers[2];

    barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(pRenderTargets[curFrame].Get(),
                                                       D3D12_RESOURCE_STATE_PRESENT,
                                                       D3D12_RESOURCE_STATE_RENDER_TARGET);
    barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(pQueryResults.Get(),
                                                       D3D12_RESOURCE_STATE_COMMON,
                                                       D3D12_RESOURCE_STATE_COPY_DEST);
    pCommandList->ResourceBarrier(2, barriers);

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(pRtvHeap->GetCPUDescriptorHandleForHeapStart(), curFrame, rtvDescSize);
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(pDsvHeap->GetCPUDescriptorHandleForHeapStart());
    pCommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    constexpr float CLEAR_COLOR[] = {1.0f, 0.75f, 0.5f, 1.0f};
    pCommandList->ClearRenderTargetView(rtvHandle, CLEAR_COLOR, 0, nullptr);
    pCommandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);

    pCommandList->SetGraphicsRootSignature(mainPipeline.GetRootSignatureRaw());
    pCommandList->SetGraphicsRootConstantBufferView(0, pMainCB->GetGPUVirtualAddress());

    pCommandList->BeginQuery(pQueryHeap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);

#ifdef USE_MONO_LODS
    model.Reset(displayType);
    for (int i = 0; i < nInstances; ++i)
    {
        uint ix = GetZCodeComponent3(i >> 0);
        uint iy = GetZCodeComponent3(i >> 1);
        uint iz = GetZCodeComponent3(i >> 2);
        model.Instance(float3(ix * instanceOffset.x, iy * instanceOffset.y, iz * instanceOffset.z));
    }
    model.Commit();
#else
    if (drawModel)
    {
        model.Render(nInstances);
    }

    if (drawMeshletAABB)
    {
        pCommandList->SetPipelineState(aabbPipeline.GetStateRaw());
        model.Render(nInstances);
    }
#endif

    pCommandList->EndQuery(pQueryHeap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    pCommandList
        ->ResolveQueryData(pQueryHeap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, pQueryResults.Get(), 0);

    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), pCommandList.Get());

    barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(pRenderTargets[curFrame].Get(),
                                                       D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                       D3D12_RESOURCE_STATE_PRESENT);
    barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(pQueryResults.Get(),
                                                       D3D12_RESOURCE_STATE_COPY_DEST,
                                                       D3D12_RESOURCE_STATE_COMMON);
    pCommandList->ResourceBarrier(2, barriers);

    ThrowIfFailed(pCommandList->Close());
}

static void OnRender()
{
    WaitForLastFrame();

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::Begin("Info");
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    {
        void         *pResultsBegin = nullptr;
        CD3DX12_RANGE readRange(0, sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
        ThrowIfFailed(pQueryResults->Map(0, &readRange, &pResultsBegin));
        auto pStats = reinterpret_cast<D3D12_QUERY_DATA_PIPELINE_STATISTICS *>(pResultsBegin);
        ImGui::Text("Primitives invoked: %d", pStats->CInvocations);
        ImGui::Text("Of them rendered: %d", pStats->CPrimitives);
        pQueryResults->Unmap(0, nullptr);
    }
    ImGui::Checkbox("VSync", &useVSync);
    if (ImGui::CollapsingHeader("Camera"))
    {
        ImGui::SliderFloat("Movement speed", &camSpeed, 1.0f, 256.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Offset", &camOffset, 1.0f, 256.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
        ImGui::Text("Focus X: %.3f", XMVectorGetX(camFocus));
        ImGui::Text("Focus Y: %.3f", XMVectorGetY(camFocus));
        ImGui::Text("Focus Z: %.3f", XMVectorGetZ(camFocus));
        ImGui::Text("Rotation X: %.1f deg", XMConvertToDegrees(camRotX));
        ImGui::Text("Rotation Y: %.1f deg", XMConvertToDegrees(camRotY));
    }
#ifdef USE_MONO_LODS
    ImGui::SliderInt("LOD", &displayType, -1, model.LodCount() - 1);
#else
    ImGui::Checkbox("Draw model", &drawModel);
    ImGui::Checkbox("Draw meshlet AABB", &drawMeshletAABB);
    ImGui::SliderInt("Meshlet layer", &displayType, -1, 3 * model.MaxLayer() + 1);
#endif
    ImGui::SliderFloat("Error threshold", &errorThreshold, 0.1f, 1000.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderInt("Object count", &nInstances, 1, MAX_NUM_INSTANCES);
    ImGui::SliderFloat("Object Offset X", &instanceOffset.x, 0.1f, 1000.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Object Offset Y", &instanceOffset.y, 0.1f, 1000.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Object Offset Z", &instanceOffset.z, 0.1f, 1000.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
    ImGui::End();

    float aspect    = float(WindowWidth) / float(WindowHeight);
    float deltaTime = ImGui::GetIO().DeltaTime;

    if (!ImGui::GetIO().WantCaptureMouse)
    {
        ImVec2 mouseDelta = ImGui::GetMouseDragDelta(0, 0.0f);
        ImGui::ResetMouseDragDelta();
        camRotX -= 4.0f * mouseDelta.y / float(WindowHeight);
        camRotY -= 4.0f * mouseDelta.x / float(WindowHeight);
        camRotX = std::clamp(camRotX, 0.001f - XM_PIDIV2, XM_PIDIV2 - 0.001f);

        while (camRotY < -XM_PI)
            camRotY += XM_2PI;
        while (camRotY > XM_PI)
            camRotY -= XM_2PI;
    }

    float camRotXSin = 0.0f;
    float camRotXCos = 0.0f;
    float camRotYSin = 0.0f;
    float camRotYCos = 0.0f;
    XMScalarSinCos(&camRotXSin, &camRotXCos, camRotX);
    XMScalarSinCos(&camRotYSin, &camRotYCos, camRotY);

    XMVECTOR vecForward  = XMVectorSet(camRotXCos * camRotYSin, camRotXSin, camRotXCos * camRotYCos, 0.0f);
    XMVECTOR vecUpGlobal = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMVECTOR vecRight    = XMVector3Normalize(XMVector3Cross(vecForward, vecUpGlobal));

    if (!ImGui::GetIO().WantCaptureKeyboard)
    {
        if (ImGui::IsKeyDown(ImGuiKey_W))
            camFocus += camSpeed * deltaTime * vecForward;
        if (ImGui::IsKeyDown(ImGuiKey_S))
            camFocus -= camSpeed * deltaTime * vecForward;
        if (ImGui::IsKeyDown(ImGuiKey_D))
            camFocus += camSpeed * deltaTime * vecRight;
        if (ImGui::IsKeyDown(ImGuiKey_A))
            camFocus -= camSpeed * deltaTime * vecRight;
        if (ImGui::IsKeyDown(ImGuiKey_E))
            camFocus += camSpeed * deltaTime * vecUpGlobal;
        if (ImGui::IsKeyDown(ImGuiKey_Q))
            camFocus -= camSpeed * deltaTime * vecUpGlobal;
    }

    XMVECTOR vecUp = XMVector3Cross(vecRight, vecForward);

    XMMATRIX matRot = XMMatrixSet(XMVectorGetX(vecRight),
                                  XMVectorGetX(vecUp),
                                  -XMVectorGetX(vecForward),
                                  0.0f,
                                  XMVectorGetY(vecRight),
                                  XMVectorGetY(vecUp),
                                  -XMVectorGetY(vecForward),
                                  0.0f,
                                  XMVectorGetZ(vecRight),
                                  XMVectorGetZ(vecUp),
                                  -XMVectorGetZ(vecForward),
                                  0.0f,
                                  0.0f,
                                  0.0f,
                                  0.0f,
                                  1.0f);

    MainData.CameraPos = camFocus - camOffset * vecForward;
    XMMATRIX matTrans  = XMMatrixTranslationFromVector(-MainData.CameraPos);

    MainData.MatView     = XMMatrixTranspose(matTrans * matRot);
    MainData.MatProj     = XMMatrixTranspose(XMMatrixPerspectiveFovRH(45.0f, aspect, 1000000.0f, 0.001f));
    MainData.MatViewProj = MainData.MatProj * MainData.MatView;
    MainData.MatNormal   = XMMatrixTranspose(XMMatrixInverse(nullptr, MainData.MatView));
    MainData.FloatInfo   = XMVectorSet(instanceOffset.x, instanceOffset.y, instanceOffset.z, errorThreshold);
    MainData.IntInfo     = XMVectorSetInt(WindowWidth, WindowHeight, displayType < 0 ? UINT32_MAX : displayType, 0);

    void         *pCameraDataBegin = nullptr;
    CD3DX12_RANGE readRange(0, 0);
    ThrowIfFailed(pMainCB->Map(0, &readRange, &pCameraDataBegin));
    memcpy(pCameraDataBegin, &MainData, sizeof(MainData));
    pMainCB->Unmap(0, nullptr);

    FillCommandList();
    ExecuteCommandList();

    if (useVSync)
        ThrowIfFailed(pSwapChain->Present(1, 0));
    else
        ThrowIfFailed(pSwapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING));
}

int WINAPI wWinMain(_In_ HINSTANCE     hCurInstance,
                    _In_opt_ HINSTANCE hPrevInstance,
                    _In_ LPWSTR        lpCmdLine,
                    _In_ int           nShowCmd)
{
    (void)hPrevInstance;
    (void)lpCmdLine;
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    hInstance = hCurInstance;

    // try
    //{
    TRaiiMainWindow raiiMainWindow;

    LoadPipeline(WindowWidth, WindowHeight);
    LoadAssets();
    raiiImgui.emplace();
    WaitForAllFrames();

    ShowWindow(hWnd, nShowCmd);

    MSG msg = {};
    while (GetMessageW(&msg, HWND_DESKTOP, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    WaitForAllFrames();
    return int(msg.wParam);
    //}
    // catch (const std::runtime_error &err)
    //{
    //    MessageBoxA(HWND_DESKTOP, err.what(), "Error", MB_ICONERROR | MB_OK);
    //    return -1;
    //}
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

LRESULT WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam))
        return 1;

    switch (uMsg)
    {
    case WM_CLOSE: PostQuitMessage(0); break;

    case WM_KEYDOWN:
        switch (wParam)
        {
        case VK_ESCAPE: PostQuitMessage(0); break;
        case 'V': useVSync ^= true; break;
        default: return DefWindowProc(hWnd, uMsg, wParam, lParam);
        }
        break;

    case WM_SIZE: {
        WindowWidth  = lParam & 0xFFFF;
        WindowHeight = (lParam >> 16) & 0xFFFF;

        for (UINT i = 0; i < FRAME_COUNT; ++i)
            pRenderTargets[i] = nullptr;

        WaitForAllFrames();
        UpdateRenderTargetSize(WindowWidth, WindowHeight);

        {
            CD3DX12_CPU_DESCRIPTOR_HANDLE handle(pRtvHeap->GetCPUDescriptorHandleForHeapStart());
            for (UINT i = 0; i < FRAME_COUNT; ++i)
            {
                ThrowIfFailed(pSwapChain->GetBuffer(i, IID_PPV_ARGS(&pRenderTargets[i])));
                pDevice->CreateRenderTargetView(pRenderTargets[i].Get(), nullptr, handle);
                handle.Offset(1, rtvDescSize);
            }
        }

        curFrame = pSwapChain->GetCurrentBackBufferIndex();
        break;
    }

    case WM_PAINT: OnRender(); break;

    default: return DefWindowProc(hWnd, uMsg, wParam, lParam);
    }
    return 0;
}
