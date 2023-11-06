#pragma once

#include "common.h"

constexpr D3D_FEATURE_LEVEL NEEDED_FEATURE_LEVEL = D3D_FEATURE_LEVEL_11_0;
constexpr UINT FRAME_COUNT = 3;

ComPtr<IDXGIAdapter1> GetHWAdapter(ComPtr<IDXGIFactory1> pFactory1);
