#pragma once

#include "common.h"

class RaiiMainWindow
{
    inline static LPCWSTR WINDOW_CLASS_NAME = L"WindowClass1";

  public:
    RaiiMainWindow()
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEX);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpszClassName = WINDOW_CLASS_NAME;
        wc.lpfnWndProc = WndProc;
        if (!RegisterClassExW(&wc))
            throw std::runtime_error("Could not register window class");

        hWnd = CreateWindowExW(0, WINDOW_CLASS_NAME, L"Window1", WS_OVERLAPPEDWINDOW & ~(WS_SIZEBOX | WS_MAXIMIZEBOX),
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, HWND_DESKTOP, nullptr,
                               hInstance, nullptr);
        if (!hWnd)
            throw std::runtime_error("Could not create window");
    }

    ~RaiiMainWindow()
    {
        DestroyWindow(hWnd);
        UnregisterClassW(WINDOW_CLASS_NAME, hInstance);
    }
};
