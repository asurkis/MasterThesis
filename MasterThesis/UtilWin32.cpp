#include "UtilWin32.h"

static LPCWSTR WINDOW_CLASS_NAME = L"WindowClass1";
static bool isInit = false;

RaiiMainWindow::RaiiMainWindow()
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
                           CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, HWND_DESKTOP, nullptr, hInstance,
                           nullptr);
    if (!hWnd)
        throw std::runtime_error("Could not create window");
}

RaiiMainWindow::~RaiiMainWindow()
{
    DestroyWindow(hWnd);
    UnregisterClassW(WINDOW_CLASS_NAME, hInstance);
}

RaiiHandle::RaiiHandle(HANDLE handle) : hSelf(handle)
{
}

RaiiHandle::~RaiiHandle()
{
    Clear();
}

RaiiHandle::RaiiHandle(RaiiHandle &&rhs) noexcept : hSelf(rhs.Release())
{
}

RaiiHandle &RaiiHandle::operator=(RaiiHandle &&rhs) noexcept
{
    Clear();
    hSelf = rhs.Release();
    return *this;
}

HANDLE RaiiHandle::Get() const noexcept
{
    return hSelf;
}

HANDLE RaiiHandle::Release() noexcept
{
    HANDLE h = hSelf;
    hSelf = nullptr;
    return h;
}

void RaiiHandle::Clear()
{
    if (hSelf)
    {
        CloseHandle(hSelf);
        hSelf = nullptr;
    }
}

std::vector<unsigned char> ReadFile(const std::filesystem::path &path)
{
    std::basic_ifstream<unsigned char> fin(path, std::ios::binary);
    if (!fin)
        throw std::runtime_error("Could not open file");
    fin.seekg(0, std::ios::end);
    size_t len = fin.tellg();
    fin.seekg(0, std::ios::beg);
    std::vector<unsigned char> bytes(len);
    fin.read(bytes.data(), len);
    return bytes;
}

std::filesystem::path GetAssetPath()
{
    constexpr size_t BUFLEN = 1024;
    WCHAR buf[BUFLEN];
    GetModuleFileNameW(nullptr, buf, BUFLEN);
    std::filesystem::path path(buf);
    path.remove_filename();
    return path;
}
