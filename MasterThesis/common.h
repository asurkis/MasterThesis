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
#include <string_view>
#include <optional>

#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"

using Microsoft::WRL::ComPtr;

using PBlob = ComPtr<ID3DBlob>;
using PDescriptorHeap = ComPtr<ID3D12DescriptorHeap>;
using PFence = ComPtr<ID3D12Fence>;
using PResource = ComPtr<ID3D12Resource>;

inline void ThrowIfFailedFn_(HRESULT hr, std::string_view file, int line)
{
    if (SUCCEEDED(hr))
        return;
    std::ostringstream oss;
    oss << "Failure on " << file << ":" << line << ", hr = 0x" << std::hex << std::setw(8) << hr;
    throw std::runtime_error(oss.str());
}

#define ThrowIfFailed(hr) ThrowIfFailedFn_(hr, __FILE__, __LINE__)
