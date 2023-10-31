#pragma once

#include "common.h"

class RaiiMainWindow
{
    inline static LPCWSTR WINDOW_CLASS_NAME = L"WindowClass1";
    inline static bool isInit = false;

  public:
    RaiiMainWindow(const RaiiMainWindow &) = delete;
    RaiiMainWindow &operator=(const RaiiMainWindow &) = delete;

    explicit RaiiMainWindow()
    {
        if (isInit)
            throw std::runtime_error("Initializing main window twice");
        isInit = true;

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

class RaiiHandle
{
    HANDLE hSelf = nullptr;

  public:
    RaiiHandle(HANDLE handle) : hSelf(handle)
    {
    }

    ~RaiiHandle()
    {
        Clear();
    }

    RaiiHandle(const RaiiHandle &) = delete;
    RaiiHandle &operator=(const RaiiHandle &) = delete;

    RaiiHandle(RaiiHandle &&rhs) noexcept : hSelf(rhs.Release())
    {
    }

    RaiiHandle &operator=(RaiiHandle &&rhs) noexcept
    {
        Clear();
        hSelf = rhs.Release();
        return *this;
    }

    HANDLE Get() const noexcept
    {
        return hSelf;
    }

    HANDLE Release() noexcept
    {
        HANDLE h = hSelf;
        hSelf = nullptr;
        return h;
    }

    void Clear()
    {
        if (hSelf)
        {
            CloseHandle(hSelf);
            hSelf = nullptr;
        }
    }
};
