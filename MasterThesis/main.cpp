#include "stdafx.h"

#include "Model.h"
#include "UtilD3D.h"
#include "UtilWin32.h"
#include <DirectXMath.h>

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

MeshPipeline mainPipeline;
MeshPipeline aabbPipeline;

// XMVECTOR camPos   = XMVectorSet(-100.0f, 80.0f, 150.0f, 0.0f);
// float    camRotX  = XMConvertToRadians(0.0f);
// float    camRotY  = XMConvertToRadians(135.0f);
// float    camSpeed = 50.0f;

XMVECTOR camFocus  = XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f);
float    camRotX   = XMConvertToRadians(0.0f);
float    camRotY   = XMConvertToRadians(0.0f);
float    camSpeed  = 10.0f;
float    camOffset = 3.0f;

float meshletCutoff = 0.25f;

bool drawModel       = true;
bool drawMeshletAABB = false;

struct Camera
{
    float4x4 MatView;
    float4x4 MatProj;
    float4x4 MatViewProj;
    float4x4 MatNormal;
};

Camera    CameraCB;
PResource pCameraGPU;

ModelGPU model;

void LoadAssets()
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

    mainPipeline.Load(assetPath / "MainMS.cso", assetPath / "MainPS.cso", assetPath / "MainAS.cso");
    aabbPipeline.Load(assetPath / "AABB_MS.cso", assetPath / "AABB_PS.cso", assetPath / "MainAS.cso");

    {
        UINT                    camBufSize = sizeof(CameraCB);
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto                    desc = CD3DX12_RESOURCE_DESC::Buffer(camBufSize);
        ThrowIfFailed(pDevice->CreateCommittedResource(&heapProps,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &desc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ,
                                                       nullptr,
                                                       IID_PPV_ARGS(&pCameraGPU)));
    }

    ModelCPU modelCpu;
    modelCpu.LoadFromFile("model.bin");
    model.Upload(modelCpu);
}

class RaiiImgui
{
  public:
    RaiiImgui()
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

    ~RaiiImgui()
    {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
};

std::optional<RaiiImgui> raiiImgui;

void FillCommandList()
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

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(pRenderTargets[curFrame].Get(),
                                                        D3D12_RESOURCE_STATE_PRESENT,
                                                        D3D12_RESOURCE_STATE_RENDER_TARGET);
    pCommandList->ResourceBarrier(1, &barrier);

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(pRtvHeap->GetCPUDescriptorHandleForHeapStart(), curFrame, rtvDescSize);
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(pDsvHeap->GetCPUDescriptorHandleForHeapStart());
    pCommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    constexpr float CLEAR_COLOR[] = {1.0f, 0.75f, 0.5f, 1.0f};
    pCommandList->ClearRenderTargetView(rtvHandle, CLEAR_COLOR, 0, nullptr);
    pCommandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);

    pCommandList->SetGraphicsRootSignature(mainPipeline.GetRootSignatureRaw());
    pCommandList->SetGraphicsRootConstantBufferView(0, pCameraGPU->GetGPUVirtualAddress());
    if (drawModel)
    {
        pCommandList->SetGraphicsRoot32BitConstant(1, 1e6f * meshletCutoff, 2);
        model.Render();
    }

    if (drawMeshletAABB)
    {
        pCommandList->SetPipelineState(aabbPipeline.GetStateRaw());
        pCommandList->SetGraphicsRoot32BitConstant(1, 1e6f * meshletCutoff, 2);
        model.Render();
    }

    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), pCommandList.Get());

    barrier = CD3DX12_RESOURCE_BARRIER::Transition(pRenderTargets[curFrame].Get(),
                                                   D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                   D3D12_RESOURCE_STATE_PRESENT);
    pCommandList->ResourceBarrier(1, &barrier);

    ThrowIfFailed(pCommandList->Close());
}

void OnRender()
{
    WaitForLastFrame();

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::Begin("Info");
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
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
    ImGui::Checkbox("Draw model", &drawModel);
    ImGui::Checkbox("Draw meshlet AABB", &drawMeshletAABB);
    ImGui::SliderFloat("Meshlet cutoff", &meshletCutoff, 0.1f, 2.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
    ImGui::End();

    float aspect    = float(WindowWidth) / float(WindowHeight);
    float deltaTime = ImGui::GetIO().DeltaTime;

    if (!ImGui::GetIO().WantCaptureMouse)
    {
        ImVec2 mouseDelta = ImGui::GetMouseDragDelta(0, 0.0f);
        ImGui::ResetMouseDragDelta();
        camRotX -= camOffset * mouseDelta.y / float(WindowHeight);
        camRotY -= camOffset * mouseDelta.x / float(WindowHeight);
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

    XMMATRIX matTrans = XMMatrixTranslationFromVector(camOffset * vecForward - camFocus);

    CameraCB.MatView     = XMMatrixTranspose(matTrans * matRot);
    CameraCB.MatProj     = XMMatrixTranspose(XMMatrixPerspectiveFovRH(45.0f, aspect, 1000.0f, 0.001f));
    CameraCB.MatViewProj = CameraCB.MatProj * CameraCB.MatView;
    CameraCB.MatNormal   = XMMatrixTranspose(XMMatrixInverse(nullptr, CameraCB.MatView));

    void         *pCameraDataBegin;
    CD3DX12_RANGE readRange(0, 0);
    ThrowIfFailed(pCameraGPU->Map(0, &readRange, &pCameraDataBegin));
    memcpy(pCameraDataBegin, &CameraCB, sizeof(CameraCB));
    pCameraGPU->Unmap(0, nullptr);

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
    RaiiMainWindow raiiMainWindow;

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
