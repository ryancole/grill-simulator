#include "camera.hpp"
#include "dx_common.hpp"
#include "input.hpp"
#include "renderer.hpp"
#include "scene.hpp"
#include "viewmodel.hpp"

#include <DirectXMath.h>

#include <algorithm>
#include <fstream>

using namespace DirectX;

namespace {

constexpr wchar_t kWindowClass[] = L"GrillSimulatorWindow";
constexpr UINT kDefaultWidth = 1280;
constexpr UINT kDefaultHeight = 720;

// A debugger breakpoint or a stalled swapchain can hand us an arbitrarily long
// frame. Clamping it means the player is never teleported through a wall.
constexpr float kMaxFrameSeconds = 0.1f;

// `scene` is built before `viewmodel` because members are initialised in
// declaration order, and the arms are drawn as instances of the scene's cube.
struct Game {
    Renderer renderer;
    Scene scene;
    Camera camera;
    Viewmodel viewmodel{scene.CubeModel()};
    Input input;
};

// WIC decodes the textures inside a glTF, and WIC is COM. Uninitialising is left
// to process teardown: the models are loaded once and never released.
void InitializeCom() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // S_FALSE means some other component got here first, which is not a failure.
    if (hr != S_FALSE) {
        ThrowIfFailed(hr, "CoInitializeEx");
    }
}

void RegisterRawMouse(HWND hwnd) {
    RAWINPUTDEVICE mouse{};
    mouse.usUsagePage = 0x01; // HID_USAGE_PAGE_GENERIC
    mouse.usUsage = 0x02;     // HID_USAGE_GENERIC_MOUSE
    mouse.dwFlags = 0;        // Deliver only while this window is in the foreground.
    mouse.hwndTarget = hwnd;
    if (!RegisterRawInputDevices(&mouse, 1, sizeof(mouse))) {
        throw HrError(HRESULT_FROM_WIN32(GetLastError()), "RegisterRawInputDevices");
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* game = reinterpret_cast<Game*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (game == nullptr && message != WM_CREATE) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    switch (message) {
    case WM_CREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return 0;
    }
    case WM_SIZE:
        // SIZE_MINIMIZED reports a 0x0 client area, which no swapchain accepts.
        if (wparam != SIZE_MINIMIZED) {
            game->renderer.Resize(LOWORD(lparam), HIWORD(lparam));
            game->input.UpdateClip(hwnd);
        }
        return 0;
    case WM_MOVE:
        // The cursor clip lives in screen coordinates, so it follows the window.
        game->input.UpdateClip(hwnd);
        return 0;
    case WM_INPUT: {
        RAWINPUT raw{};
        UINT size = sizeof(raw);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &raw, &size,
                            sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)) {
            game->input.OnRawInput(raw);
        }
        // WM_INPUT must reach DefWindowProc so the system can clean up the
        // buffered input for this message.
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    case WM_LBUTTONDOWN:
        game->input.SetMouseLook(hwnd, true);
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            // Escape backs out one level: first it frees the cursor, then it
            // closes the game.
            if (game->input.mouse_look()) {
                game->input.SetMouseLook(hwnd, false);
            } else {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            return 0;
        }
        game->input.OnKey(wparam, true);
        return 0;
    case WM_KEYUP:
        game->input.OnKey(wparam, false);
        return 0;
    case WM_KILLFOCUS:
        // Key-up messages are not delivered to a background window, so a key
        // held across an Alt+Tab would otherwise stay down forever.
        game->input.SetMouseLook(hwnd, false);
        game->input.ReleaseAllKeys();
        return 0;
    case WM_DESTROY:
        game->input.SetMouseLook(hwnd, false);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

int Run(HINSTANCE instance, int show_command) {
    // Before `Game`, whose Scene loads a glTF and decodes its textures.
    InitializeCom();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    Game game;

    RECT bounds = {0, 0, static_cast<LONG>(kDefaultWidth), static_cast<LONG>(kDefaultHeight)};
    AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExW(0, kWindowClass, L"Grill Simulator - click to look, WASD to walk",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr,
                                nullptr, instance, &game);
    if (hwnd == nullptr) {
        throw HrError(HRESULT_FROM_WIN32(GetLastError()), "CreateWindowEx");
    }

    RegisterRawMouse(hwnd);
    game.renderer.Initialize(hwnd, kDefaultWidth, kDefaultHeight, game.scene);
    ShowWindow(hwnd, show_command);

    LARGE_INTEGER frequency{};
    LARGE_INTEGER previous{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&previous);

    MSG message{};
    while (message.message != WM_QUIT) {
        // Drain the queue first: raw mouse motion arrives one message per
        // report, and a frame's worth of them has to be summed before it is used.
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            continue;
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const float dt =
            std::min(static_cast<float>(static_cast<double>(now.QuadPart - previous.QuadPart) /
                                        static_cast<double>(frequency.QuadPart)),
                     kMaxFrameSeconds);
        previous = now;

        float mouse_dx = 0.0f;
        float mouse_dy = 0.0f;
        game.input.ConsumeMouseDelta(mouse_dx, mouse_dy);
        game.camera.Look(mouse_dx, mouse_dy);
        game.camera.Update(game.input, game.scene.Colliders(), dt);

        const XMMATRIX view_projection =
            game.camera.ViewMatrix() * game.camera.ProjectionMatrix(game.renderer.AspectRatio());
        game.renderer.Render(game.scene, game.viewmodel.Pose(game.camera.CameraToWorldMatrix()),
                             view_projection);
    }

    game.renderer.Shutdown();
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
        // A fatal error can outrun the first presented frame, so a message box is
        // the only way this is ever seen when launched outside a debugger.
        const std::string message = error.what();
        MessageBoxA(nullptr, message.c_str(), "Grill Simulator - fatal error",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
}
