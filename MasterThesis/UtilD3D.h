#pragma once

#include "UtilWin32.h"
#include "stdafx.h"

constexpr D3D_FEATURE_LEVEL NEEDED_FEATURE_LEVEL = D3D_FEATURE_LEVEL_11_0;
constexpr UINT              FRAME_COUNT          = 3;

void LoadPipeline(UINT width, UINT height);
void UpdateRenderTargetSize(UINT width, UINT height);
void WaitForCurFrame();
void WaitForLastFrame();
void WaitForAllFrames();

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
