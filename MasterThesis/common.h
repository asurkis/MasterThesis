#pragma once

#include <Windows.h>
#include <stdexcept>

inline HINSTANCE hInstance;
inline HWND hWnd;

extern LRESULT WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
