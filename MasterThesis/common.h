#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <d3dx12.h>
#include <dxgi1_6.h>
#include <wrl.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <optional>

#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"

using Microsoft::WRL::ComPtr;

inline void ThrowIfFailedFn(HRESULT hr, const char *file, int line)
{
    if (SUCCEEDED(hr))
        return;
    std::ostringstream oss;
    oss << "Failure on " << file << ":" << line << ", hr = 0x" << std::hex << std::setw(8) << hr;
    throw std::runtime_error(oss.str());
}

#define ThrowIfFailed(hr) ThrowIfFailedFn(hr, __FILE__, __LINE__)

inline HINSTANCE hInstance = nullptr;
inline HWND hWnd = nullptr;

constexpr D3D_FEATURE_LEVEL NEEDED_FEATURE_LEVEL = D3D_FEATURE_LEVEL_12_2;
constexpr UINT FRAME_COUNT = 3;

extern LRESULT WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
