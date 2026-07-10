#include "dx_common.hpp"
#include "renderer.hpp"

#include <fstream>

namespace {

constexpr wchar_t kWindowClass[] = L"GrillSimulatorWindow";
constexpr UINT kDefaultWidth = 1280;
constexpr UINT kDefaultHeight = 720;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* renderer = reinterpret_cast<Renderer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
    case WM_CREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return 0;
    }
    case WM_SIZE:
        // SIZE_MINIMIZED reports a 0x0 client area, which no swapchain accepts.
        if (renderer && wparam != SIZE_MINIMIZED) {
            renderer->Resize(LOWORD(lparam), HIWORD(lparam));
        }
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

int Run(HINSTANCE instance, int show_command) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    Renderer renderer;

    RECT bounds = {0, 0, static_cast<LONG>(kDefaultWidth), static_cast<LONG>(kDefaultHeight)};
    AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExW(0, kWindowClass, L"Grill Simulator", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left,
                                bounds.bottom - bounds.top, nullptr, nullptr, instance, &renderer);
    if (hwnd == nullptr) {
        throw HrError(HRESULT_FROM_WIN32(GetLastError()), "CreateWindowEx");
    }

    renderer.Initialize(hwnd, kDefaultWidth, kDefaultHeight);
    ShowWindow(hwnd, show_command);

    MSG message{};
    while (message.message != WM_QUIT) {
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        } else {
            renderer.Render();
        }
    }

    renderer.Shutdown();
    return static_cast<int>(message.wParam);
}

} // namespace

std::filesystem::path ExecutableDirectory() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length == MAX_PATH) {
        throw HrError(HRESULT_FROM_WIN32(GetLastError()), "GetModuleFileName");
    }
    return std::filesystem::path(buffer).parent_path();
}

std::vector<std::byte> ReadBinaryFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Cannot open " + path.string());
    }

    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<std::byte> data(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
        throw std::runtime_error("Cannot read " + path.string());
    }
    return data;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    try {
        return Run(instance, show_command);
    } catch (const std::exception& error) {
        // Nothing is rendering yet, so a message box is the only way this is
        // ever seen when launched outside a debugger.
        const std::string message = error.what();
        MessageBoxA(nullptr, message.c_str(), "Grill Simulator - fatal error",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
}
