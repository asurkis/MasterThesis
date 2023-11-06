#pragma once

#include "common.h"

extern LRESULT WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

inline HINSTANCE hInstance = nullptr;
inline HWND hWnd = nullptr;

class RaiiMainWindow
{
  public:
    RaiiMainWindow(const RaiiMainWindow &) = delete;
    RaiiMainWindow &operator=(const RaiiMainWindow &) = delete;

    explicit RaiiMainWindow();
    ~RaiiMainWindow();
};

class RaiiHandle
{
    HANDLE hSelf = nullptr;

  public:
    RaiiHandle(const RaiiHandle &) = delete;
    RaiiHandle &operator=(const RaiiHandle &) = delete;

    RaiiHandle(HANDLE handle);
    ~RaiiHandle();

    RaiiHandle(RaiiHandle &&rhs) noexcept;
    RaiiHandle &operator=(RaiiHandle &&rhs) noexcept;

    HANDLE Get() const noexcept;
    HANDLE Release() noexcept;

    void Clear();
};

std::vector<unsigned char> ReadFile(const std::filesystem::path &path);
std::filesystem::path GetAssetPath();
