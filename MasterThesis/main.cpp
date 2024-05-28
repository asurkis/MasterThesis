#include "stdafx.h"

#include "UtilD3D.h"
#include "UtilWin32.h"
#include <DirectXMath.h>

#define USE_MONO_LODS

using namespace DirectX;

bool ShowImgui = true;

RECT BeforeFullScreen = {};
bool IsFullScreen     = false;

bool UseVSync = true;

enum SrvDescriptors
{
    MY_SRV_DESC_IMGUI = 0,
    // MY_SRV_DESC_CAMERA,
    MY_SRV_DESC_TOTAL
};

PDescriptorHeap pSrvHeap;
UINT            SrvDescSize;

ComPtr<ID3D12QueryHeap> pQueryHeap;
PResource               pQueryResults;

#ifdef USE_MONO_LODS
TMonoPipeline MainPipeline;
#else
TMeshletPipeline MainPipeline;
TMeshletPipeline AabbPipeline;
#endif

XMVECTOR CamFocus  = XMVectorSet(-140.0f, 220.0f, -150.0f, 0.0f);
float    CamRotX   = XMConvertToRadians(-30.0f);
float    CamRotY   = XMConvertToRadians(45.0f);
float    CamSpeed  = 256.0f;
float    CamOffset = 3.0f;

float ErrorThreshold = 0.25f;
int   DisplayType    = -1;

int    nInstances     = 64;
float3 InstanceOffset = float3(300.0f, 600.0f, 300.0f);

#ifndef USE_MONO_LODS
bool DrawModel       = true;
bool DrawMeshletAABB = false;
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
        SrvDescSize = pDevice->GetDescriptorHandleIncrementSize(srvDesc.Type);
    }

#ifdef USE_MONO_LODS
    MainPipeline.Load(assetPath / "MainVS.cso", assetPath / "MainPS.cso");
    // MainPipeline.Load(assetPath / "MainVS.cso", assetPath / "MainPS.cso", assetPath / "HeatmapGS.cso");
#else
    MainPipeline.Load(assetPath / "MainMS.cso", assetPath / "MainPS.cso", assetPath / "MainAS.cso");
    // MainPipeline.Load(assetPath / "HeatmapMS.cso", assetPath / "MainPS.cso", assetPath / "MainAS.cso");
    AabbPipeline.Load(assetPath / "AABB_MS.cso", assetPath / "AABB_PS.cso", assetPath / "MainAS.cso");
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
                                                SrvDescSize);
        CD3DX12_GPU_DESCRIPTOR_HANDLE gpuHandle(pSrvHeap->GetGPUDescriptorHandleForHeapStart(),
                                                MY_SRV_DESC_IMGUI,
                                                SrvDescSize);
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

static uint GetZCodeComponent2(uint x)
{
    x = x & 0x55555555;
    x = (x ^ (x >> 1)) & 0x33333333;
    x = (x ^ (x >> 2)) & 0x0F0F0F0F;
    x = (x ^ (x >> 4)) & 0x00FF00FF;
    x = (x ^ (x >> 8)) & 0x0000FFFF;
    return x;
}

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
    ThrowIfFailed(pCommandList->Reset(pCommandAllocator.Get(), MainPipeline.GetStateRaw()));

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

    CD3DX12_RESOURCE_BARRIER barriers[2] = {};

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

    pCommandList->SetGraphicsRootSignature(MainPipeline.GetRootSignatureRaw());
    pCommandList->SetGraphicsRootConstantBufferView(0, pMainCB->GetGPUVirtualAddress());

    pCommandList->BeginQuery(pQueryHeap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);

#ifdef USE_MONO_LODS
    model.Reset(DisplayType);
    for (int i = 0; i < nInstances; ++i)
    {
        uint ix = GetZCodeComponent2(i >> 0);
        uint iz = GetZCodeComponent2(i >> 1);
        model.Instance(float3(ix * InstanceOffset.x, 0.0f, iz * InstanceOffset.z));
    }
    model.Commit();
#else
    if (DrawModel)
    {
        model.Render(nInstances);
    }

    if (DrawMeshletAABB)
    {
        pCommandList->SetPipelineState(AabbPipeline.GetStateRaw());
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

    if (ShowImgui)
    {
        ImGui::Begin("Info");
        ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
        if (ImGui::CollapsingHeader("Stats"))
        {
            void         *pResultsBegin = nullptr;
            CD3DX12_RANGE readRange(0, sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
            ThrowIfFailed(pQueryResults->Map(0, &readRange, &pResultsBegin));
            auto pStats = reinterpret_cast<D3D12_QUERY_DATA_PIPELINE_STATISTICS *>(pResultsBegin);
            ImGui::Text("Primitives invoked: %d", pStats->CInvocations);
            ImGui::Text("Of them rendered: %d", pStats->CPrimitives);
            ImGui::Text("Pixel shader invocations: %d", pStats->PSInvocations);
            pQueryResults->Unmap(0, nullptr);

            ImGui::Text("Width: %d", WindowWidth);
            ImGui::Text("Height: %d", WindowHeight);
        }
        ImGui::Checkbox("VSync", &UseVSync);
        if (ImGui::CollapsingHeader("Camera"))
        {
            ImGui::SliderFloat("Movement speed", &CamSpeed, 1.0f, 256.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Offset", &CamOffset, 1.0f, 256.0f, "%.3f", ImGuiSliderFlags_Logarithmic);

            float camFocusTmp[3] = {XMVectorGetX(CamFocus), XMVectorGetY(CamFocus), XMVectorGetZ(CamFocus)};
            if (ImGui::SliderFloat3("Focus", camFocusTmp, -1000.0f, 1000.0f))
                CamFocus = XMVectorSet(camFocusTmp[0], camFocusTmp[1], camFocusTmp[2], 0.0f);

            float camRotDeg[] = {XMConvertToDegrees(CamRotX), XMConvertToDegrees(CamRotY)};
            if (ImGui::SliderFloat2("Rotation (deg)", camRotDeg, -180.0f, 180.0f))
            {
                CamRotX = XMConvertToRadians(camRotDeg[0]);
                CamRotY = XMConvertToRadians(camRotDeg[1]);
            }
        }
#ifdef USE_MONO_LODS
        ImGui::SliderInt("LOD", &DisplayType, -1, model.LodCount() - 1);
#else
        ImGui::Checkbox("Draw model", &DrawModel);
        ImGui::Checkbox("Draw meshlet AABB", &DrawMeshletAABB);
        ImGui::SliderInt("Meshlet layer", &DisplayType, -1, 3 * model.MaxLayer() + 1);
#endif
        ImGui::SliderFloat("Error threshold", &ErrorThreshold, 0.1f, 1000.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderInt("Object count", &nInstances, 1, MAX_NUM_INSTANCES);
        ImGui::SliderFloat("Object Offset X", &InstanceOffset.x, 0.1f, 1000.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Object Offset Y", &InstanceOffset.y, 0.1f, 1000.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Object Offset Z", &InstanceOffset.z, 0.1f, 1000.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
        ImGui::End();
    }

    float aspect    = float(WindowWidth) / float(WindowHeight);
    float deltaTime = ImGui::GetIO().DeltaTime;

    if (!ImGui::GetIO().WantCaptureMouse)
    {
        ImVec2 mouseDelta = ImGui::GetMouseDragDelta(0, 0.0f);
        ImGui::ResetMouseDragDelta();
        CamRotX -= 4.0f * mouseDelta.y / float(WindowHeight);
        CamRotY -= 4.0f * mouseDelta.x / float(WindowHeight);
        CamRotX = std::clamp(CamRotX, 0.001f - XM_PIDIV2, XM_PIDIV2 - 0.001f);

        while (CamRotY < -XM_PI)
            CamRotY += XM_2PI;
        while (CamRotY > XM_PI)
            CamRotY -= XM_2PI;
    }

    float camRotXSin = 0.0f;
    float camRotXCos = 0.0f;
    float camRotYSin = 0.0f;
    float camRotYCos = 0.0f;
    XMScalarSinCos(&camRotXSin, &camRotXCos, CamRotX);
    XMScalarSinCos(&camRotYSin, &camRotYCos, CamRotY);

    XMVECTOR vecForward  = XMVectorSet(camRotXCos * camRotYSin, camRotXSin, camRotXCos * camRotYCos, 0.0f);
    XMVECTOR vecUpGlobal = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMVECTOR vecRight    = XMVector3Normalize(XMVector3Cross(vecForward, vecUpGlobal));

    if (!ImGui::GetIO().WantCaptureKeyboard)
    {
        if (ImGui::IsKeyDown(ImGuiKey_W))
            CamFocus += CamSpeed * deltaTime * vecForward;
        if (ImGui::IsKeyDown(ImGuiKey_S))
            CamFocus -= CamSpeed * deltaTime * vecForward;
        if (ImGui::IsKeyDown(ImGuiKey_D))
            CamFocus += CamSpeed * deltaTime * vecRight;
        if (ImGui::IsKeyDown(ImGuiKey_A))
            CamFocus -= CamSpeed * deltaTime * vecRight;
        if (ImGui::IsKeyDown(ImGuiKey_E))
            CamFocus += CamSpeed * deltaTime * vecUpGlobal;
        if (ImGui::IsKeyDown(ImGuiKey_Q))
            CamFocus -= CamSpeed * deltaTime * vecUpGlobal;
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

    MainData.CameraPos = CamFocus - CamOffset * vecForward;
    XMMATRIX matTrans  = XMMatrixTranslationFromVector(-MainData.CameraPos);

    MainData.MatView     = XMMatrixTranspose(matTrans * matRot);
    MainData.MatProj     = XMMatrixTranspose(XMMatrixPerspectiveFovRH(45.0f, aspect, 1000000.0f, 0.001f));
    MainData.MatViewProj = MainData.MatProj * MainData.MatView;
    MainData.MatNormal   = XMMatrixTranspose(XMMatrixInverse(nullptr, MainData.MatView));
    MainData.FloatInfo   = XMVectorSet(InstanceOffset.x, InstanceOffset.y, InstanceOffset.z, ErrorThreshold);
    MainData.IntInfo     = XMVectorSetInt(WindowWidth, WindowHeight, DisplayType < 0 ? UINT32_MAX : DisplayType, 0);

    void         *pCameraDataBegin = nullptr;
    CD3DX12_RANGE readRange(0, 0);
    ThrowIfFailed(pMainCB->Map(0, &readRange, &pCameraDataBegin));
    memcpy(pCameraDataBegin, &MainData, sizeof(MainData));
    pMainCB->Unmap(0, nullptr);

    FillCommandList();
    ExecuteCommandList();

    if (UseVSync)
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
        case 'V': UseVSync ^= true; break;
        case 'I': ShowImgui ^= true; break;
        case 'F':
        case VK_F11:
            IsFullScreen ^= true;
            if (IsFullScreen)
            {
                GetWindowRect(hWnd, &BeforeFullScreen);
                SetWindowLongW(hWnd, GWL_STYLE, WS_OVERLAPPED);
                HMONITOR      hMonitor    = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFOEX monitorInfo = {};
                monitorInfo.cbSize        = sizeof(MONITORINFOEX);
                GetMonitorInfoW(hMonitor, &monitorInfo);

                SetWindowPos(hWnd,
                             HWND_TOP,
                             monitorInfo.rcMonitor.left,
                             monitorInfo.rcMonitor.top,
                             monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
                             monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
                             SWP_FRAMECHANGED | SWP_NOACTIVATE);
                ShowWindow(hWnd, SW_MAXIMIZE);
            }
            else
            {
                SetWindowLongW(hWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);
                SetWindowPos(hWnd,
                             HWND_NOTOPMOST,
                             BeforeFullScreen.left,
                             BeforeFullScreen.top,
                             BeforeFullScreen.right - BeforeFullScreen.left,
                             BeforeFullScreen.bottom - BeforeFullScreen.top,
                             SWP_FRAMECHANGED | SWP_NOACTIVATE);
                ShowWindow(hWnd, SW_NORMAL);
            }
            break;
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
