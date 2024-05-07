#pragma once

#include "stdafx.h"

extern LRESULT WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

inline HINSTANCE hInstance    = nullptr;
inline HWND      hWnd         = nullptr;
inline UINT      WindowWidth  = 0;
inline UINT      WindowHeight = 0;

class TRaiiMainWindow
{
  public:
    TRaiiMainWindow(const TRaiiMainWindow &)            = delete;
    TRaiiMainWindow &operator=(const TRaiiMainWindow &) = delete;

    explicit TRaiiMainWindow();
    ~TRaiiMainWindow();
};

class TRaiiHandle
{
    HANDLE hSelf = nullptr;

  public:
    TRaiiHandle(const TRaiiHandle &)            = delete;
    TRaiiHandle &operator=(const TRaiiHandle &) = delete;

    TRaiiHandle() noexcept;
    TRaiiHandle(HANDLE handle);
    ~TRaiiHandle();

    TRaiiHandle(TRaiiHandle &&rhs) noexcept;
    TRaiiHandle &operator=(TRaiiHandle &&rhs) noexcept;

    HANDLE Get() const noexcept;
    HANDLE Release() noexcept;

    void Clear();
};

std::vector<unsigned char> ReadFile(const std::filesystem::path &path);
std::filesystem::path      GetAssetPath();
