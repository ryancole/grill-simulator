#include "audio.hpp"
#include "camera.hpp"
#include "dx_common.hpp"
#include "furniture.hpp"
#include "input.hpp"
#include "level.hpp"
#include "physics.hpp"
#include "props.hpp"
#include "renderer.hpp"
#include "scene.hpp"
#include "viewmodel.hpp"
#include "world.hpp"

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <optional>

using namespace DirectX;

namespace {

constexpr wchar_t kWindowClass[] = L"GrillSimulatorWindow";
constexpr UINT kDefaultWidth = 1280;
constexpr UINT kDefaultHeight = 720;

// A debugger breakpoint or a stalled swapchain can hand us an arbitrarily long
// frame. Clamping it means the player is never teleported through a wall.
constexpr float kMaxFrameSeconds = 0.1f;

// The persistent systems: everything that lives for the whole session, no matter
// which level is loaded. The level itself -- the Scene, Props and Furniture -- lives
// in `world`, which is built after Initialize and swapped out to change levels.
//
// Physics comes up before Camera because the camera's controller registers with the
// physics scene; the viewmodel names the shared unit cube, whose model index is
// fixed (Scene::kCubeModel) so it needs no live Scene to build its arms.
struct Game {
    Renderer renderer;
    Physics physics;
    Camera camera{physics};
    Viewmodel viewmodel{Scene::kCubeModel};
    Input input;
    Audio audio;

    // The current level. Empty until the first is loaded, and reset() then re-emplaced
    // to switch. Destroying it hands the level's renderer geometry and physics actors
    // back before the next is built. Declared last so it tears down first, while the
    // renderer and physics it borrows are still alive.
    std::optional<World> world;
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

    HWND hwnd = CreateWindowExW(0, kWindowClass,
                                L"Grill Simulator - WASD walk, E grab, 1/2 levels, R reload",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr,
                                nullptr, instance, &game);
    if (hwnd == nullptr) {
        throw HrError(HRESULT_FROM_WIN32(GetLastError()), "CreateWindowEx");
    }

    RegisterRawMouse(hwnd);
    // The device and pipelines first -- the session's, independent of any level.
    game.renderer.Initialize(hwnd, kDefaultWidth, kDefaultHeight);

    // The levels the player switches between, in the order the number keys select
    // them. Each is a factory, so every (re)load builds a fresh copy -- a level that
    // was knocked about is restored, not resumed.
    const std::array<LevelDef (*)(), 2> levels_table = {&levels::Backyard, &levels::Rooftop};
    int current_level = 0;

    // Loads a level by index: unloads whatever is current (handing its GPU geometry
    // and physics actors back), builds the new one, and drops the player at its
    // spawn facing its way. The renderer uploads the scene, aims the sun and the
    // static colliders become PhysX actors inside the World constructor.
    auto load_level = [&](int index) {
        current_level = index;
        const LevelDef level = levels_table[index]();
        game.world.reset();
        game.world.emplace(level, game.renderer, game.physics);
        game.camera.Respawn(level.player_spawn, level.player_facing);
    };
    load_level(0);

    ShowWindow(hwnd, show_command);

    LARGE_INTEGER frequency{};
    LARGE_INTEGER previous{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&previous);

    // Level controls, all edge-triggered so a held key fires once: 1 and 2 switch to
    // the backyard and the rooftop, R reloads whatever is current (restoring a level
    // knocked about in play). `edge` reads a key and reports the frame it goes down.
    bool key1_was = false;
    bool key2_was = false;
    bool keyr_was = false;
    auto edge = [&game](int vk, bool& was) {
        const bool down = game.input.IsKeyDown(vk);
        const bool went_down = down && !was;
        was = down;
        return went_down;
    };

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

        // Read all three every frame so each key's state stays current, then act on
        // at most one. Swapping here, before anything reads the world this frame,
        // means the step and draw below run entirely on the freshly loaded level.
        const bool pick_backyard = edge('1', key1_was);
        const bool pick_rooftop = edge('2', key2_was);
        const bool reload = edge('R', keyr_was);
        if (pick_backyard) {
            load_level(0);
        } else if (pick_rooftop) {
            load_level(1);
        } else if (reload) {
            load_level(current_level);
        }

        // Advance the physics scene on its fixed clock: the props, furniture and the
        // player's controller all move on it. Simulate first, then read poses.
        game.physics.Step(dt);

        float mouse_dx = 0.0f;
        float mouse_dy = 0.0f;
        game.input.ConsumeMouseDelta(mouse_dx, mouse_dy);
        game.camera.Look(mouse_dx, mouse_dy);
        game.camera.Update(game.input, dt);

        // The camera-to-world matrix is the viewmodel's pose, the listener's ear
        // and facing, and the reach a grab is measured along, so it is built
        // once and shared.
        const XMMATRIX camera_to_world = game.camera.CameraToWorldMatrix();
        game.audio.Update(camera_to_world, dt);
        // Sound the impacts this step's simulation reported, now that Update has
        // posed the listener so each one pans from where it landed.
        for (const Impact& impact : game.physics.Impacts()) {
            game.audio.PlayImpact(impact.position, impact.strength, impact.sound);
        }
        game.world->props().Update(camera_to_world, game.input);
        // Read the dynamic furniture's body poses back into their draw instances.
        game.world->furniture().Update();

        const XMMATRIX view_projection =
            game.camera.ViewMatrix() * game.camera.ProjectionMatrix(game.renderer.AspectRatio());
        Props& props = game.world->props();
        game.renderer.Render(game.world->scene(), props.WorldInstances(),
                             props.HighlightInstances(),
                             game.viewmodel.Pose(camera_to_world), props.HeldInstances(),
                             view_projection, game.camera.Position(), props.PromptText());
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
