#include "common.h"

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

ComPtr<ID3D12RootSignature> pRootSignature;
ComPtr<ID3D12PipelineState> pInitPipelineState;

struct CameraData
{
    XMMATRIX MatView;
    XMMATRIX MatProj;
    XMMATRIX MatViewProj;
    XMMATRIX MatNormalViewProj;
};

XMVECTOR camPos        = XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f);
float    camRotX       = 0.0f;
float    camRotY       = 0.0f;
float    movementSpeed = 1.0f;

CameraData CameraCB;
PResource  pCameraGPU;

Model model;

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

    {
        std::vector<BYTE> dataMS = ReadFile(assetPath / L"MainMS.cso");
        std::vector<BYTE> dataPS = ReadFile(assetPath / L"MainPS.cso");

        ThrowIfFailed(pDevice->CreateRootSignature(0, dataMS.data(), dataMS.size(), IID_PPV_ARGS(&pRootSignature)));

        D3DX12_MESH_SHADER_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature                         = pRootSignature.Get();
        psoDesc.MS.pShaderBytecode                     = dataMS.data();
        psoDesc.MS.BytecodeLength                      = dataMS.size();
        psoDesc.PS.pShaderBytecode                     = dataPS.data();
        psoDesc.PS.BytecodeLength                      = dataPS.size();
        psoDesc.BlendState                             = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.SampleMask                             = UINT_MAX;
        psoDesc.RasterizerState                        = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.RasterizerState.CullMode               = D3D12_CULL_MODE_NONE;
        psoDesc.DepthStencilState                      = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthFunc            = D3D12_COMPARISON_FUNC_GREATER;
        psoDesc.PrimitiveTopologyType                  = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets                       = 1;
        psoDesc.RTVFormats[0]                          = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.DSVFormat                              = DXGI_FORMAT_R32_FLOAT;
        psoDesc.SampleDesc.Count                       = 1;
        psoDesc.SampleDesc.Quality                     = 0;
        psoDesc.CachedPSO                              = {};
        psoDesc.Flags                                  = D3D12_PIPELINE_STATE_FLAG_NONE;

        CD3DX12_PIPELINE_MESH_STATE_STREAM psoStream(psoDesc);

        D3D12_PIPELINE_STATE_STREAM_DESC streamDesc = {};
        streamDesc.pPipelineStateSubobjectStream    = &psoStream;
        streamDesc.SizeInBytes                      = sizeof(psoStream);

        ThrowIfFailed(pDevice->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&pInitPipelineState)));
    }

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

    std::filesystem::path dragonPath = assetPath / "Dragon_LOD0.bin";
    ThrowIfFailed(model.LoadFromFile(dragonPath.c_str()));
    ThrowIfFailed(model.UploadGpuResources(pDevice.Get(),
                                           pCommandQueueDirect.Get(),
                                           pCommandAllocator.Get(),
                                           pCommandList.Get()));
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
    ThrowIfFailed(pCommandList->Reset(pCommandAllocator.Get(), pInitPipelineState.Get()));

    ID3D12DescriptorHeap *heapsToSet[] = {pSrvHeap.Get()};
    pCommandList->SetDescriptorHeaps(1, heapsToSet);

    D3D12_VIEWPORT viewport = {};
    viewport.TopLeftX       = 0.0f;
    viewport.TopLeftY       = 0.0f;
    viewport.Width          = static_cast<float>(WindowWidth);
    viewport.Height         = static_cast<float>(WindowHeight);
    viewport.MinDepth       = D3D12_MIN_DEPTH;
    viewport.MaxDepth       = D3D12_MAX_DEPTH;

    D3D12_RECT scissorRect = {};
    scissorRect.left       = 0;
    scissorRect.top        = 0;
    scissorRect.right      = WindowWidth;
    scissorRect.bottom     = WindowHeight;

    pCommandList->SetGraphicsRootSignature(pRootSignature.Get());
    pCommandList->RSSetViewports(1, &viewport);
    pCommandList->RSSetScissorRects(1, &scissorRect);

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(pRenderTargets[curFrame].Get(),
                                                        D3D12_RESOURCE_STATE_PRESENT,
                                                        D3D12_RESOURCE_STATE_RENDER_TARGET);
    pCommandList->ResourceBarrier(1, &barrier);

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(pRtvHeap->GetCPUDescriptorHandleForHeapStart(), curFrame, rtvDescSize);
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(pDsvHeap->GetCPUDescriptorHandleForHeapStart());
    pCommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    constexpr float CLEAR_COLOR[] = {0.0f, 0.2f, 0.4f, 1.0f};
    pCommandList->ClearRenderTargetView(rtvHandle, CLEAR_COLOR, 0, nullptr);
    pCommandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);

    pCommandList->SetGraphicsRootConstantBufferView(0, pCameraGPU->GetGPUVirtualAddress());

    for (uint32_t meshId = 0; meshId < model.GetMeshCount(); ++meshId)
    {
        auto &mesh = model.GetMesh(meshId);

        pCommandList->SetGraphicsRoot32BitConstant(1, mesh.IndexSize, 0);
        pCommandList->SetGraphicsRootShaderResourceView(2, mesh.VertexResources[0]->GetGPUVirtualAddress());
        pCommandList->SetGraphicsRootShaderResourceView(3, mesh.MeshletResource->GetGPUVirtualAddress());
        pCommandList->SetGraphicsRootShaderResourceView(4, mesh.UniqueVertexIndexResource->GetGPUVirtualAddress());
        pCommandList->SetGraphicsRootShaderResourceView(5, mesh.PrimitiveIndexResource->GetGPUVirtualAddress());

        for (size_t subsetId = 0; subsetId < mesh.MeshletSubsets.size(); ++subsetId)
        {
            auto &subset = mesh.MeshletSubsets[subsetId];
            pCommandList->SetGraphicsRoot32BitConstant(1, subset.Offset, 1);
            pCommandList->DispatchMesh(subset.Count, 1, 1);
            // pCommandList->DispatchMesh(1, 1, 1);
            // break;
        }
        // break;
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
    ImGui::DragFloat("Movement speed", &movementSpeed);
    ImGui::End();

    float aspect    = WindowWidth / static_cast<float>(WindowHeight);
    float deltaTime = ImGui::GetIO().DeltaTime;

    if (!ImGui::GetIO().WantCaptureMouse)
    {
        ImVec2 mouseDelta = ImGui::GetMouseDragDelta(0, 0.0f);
        ImGui::ResetMouseDragDelta();
        camRotX += mouseDelta.y / static_cast<float>(WindowHeight);
        camRotY += mouseDelta.x / static_cast<float>(WindowHeight);
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
        if (ImGui::IsKeyDown(ImGuiKey_W)) camPos += movementSpeed * deltaTime * vecForward;
        if (ImGui::IsKeyDown(ImGuiKey_S)) camPos -= movementSpeed * deltaTime * vecForward;
        if (ImGui::IsKeyDown(ImGuiKey_D)) camPos += movementSpeed * deltaTime * vecRight;
        if (ImGui::IsKeyDown(ImGuiKey_A)) camPos -= movementSpeed * deltaTime * vecRight;
        if (ImGui::IsKeyDown(ImGuiKey_E)) camPos += movementSpeed * deltaTime * vecUpGlobal;
        if (ImGui::IsKeyDown(ImGuiKey_Q)) camPos -= movementSpeed * deltaTime * vecUpGlobal;
    }

    XMVECTOR vecUp = XMVector3Cross(vecRight, vecForward);

    static float angle = 0.0f;
    angle += ImGui::GetIO().DeltaTime;

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

    XMMATRIX matTrans = XMMatrixTranslationFromVector(-camPos);

    CameraCB.MatView     = XMMatrixTranspose(matTrans * matRot);
    CameraCB.MatProj     = XMMatrixTranspose(XMMatrixPerspectiveFovRH(45.0f, aspect, 1000.0f, 0.001f));
    CameraCB.MatViewProj = CameraCB.MatProj * CameraCB.MatView;

    void         *pCameraDataBegin;
    CD3DX12_RANGE readRange(0, 0);
    ThrowIfFailed(pCameraGPU->Map(0, &readRange, &pCameraDataBegin));
    memcpy(pCameraDataBegin, &CameraCB, sizeof(CameraCB));
    pCameraGPU->Unmap(0, nullptr);

    FillCommandList();

    ID3D12CommandList *ppCommandLists[] = {pCommandList.Get()};
    pCommandQueueDirect->ExecuteCommandLists(1, ppCommandLists);
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
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    hInstance = hCurInstance;

    try
    {
        RaiiMainWindow raiiMainWindow;

        LoadPipeline(WindowWidth, WindowHeight);
        LoadAssets();
        raiiImgui.emplace();
        WaitForAllFrames();

        ShowWindow(hWnd, nShowCmd);
        // PostQuitMessage(0);

        MSG msg = {};
        while (GetMessageW(&msg, HWND_DESKTOP, 0, 0))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        WaitForAllFrames();
        return static_cast<int>(msg.wParam);
    }
    catch (const std::runtime_error &err)
    {
        MessageBoxA(HWND_DESKTOP, err.what(), "Error", MB_ICONERROR | MB_OK);
        return -1;
    }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

LRESULT WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam)) return 1;

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

    case WM_PAINT: OnRender(); break;
    }

    default: return DefWindowProc(hWnd, uMsg, wParam, lParam);
    }
    return 0;
}
