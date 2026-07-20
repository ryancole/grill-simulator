#include "actions.hpp"
#include "audio.hpp"
#include "camera.hpp"
#include "dx_common.hpp"
#include "flame.hpp"
#include "flow_volume.hpp"
#include "fluid.hpp"
#include "furniture.hpp"
#include "input.hpp"
#include "level.hpp"
#include "menu.hpp"
#include "physics.hpp"
#include "profiler.hpp"
#include "props.hpp"
#include "renderer.hpp"
#include "scene.hpp"
#include "soft_body.hpp"
#include "viewmodel.hpp"
#include "world.hpp"

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cmath>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

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
    // The GPU fluid rides the physics scene, so it sits between Physics (whose CUDA
    // context and scene it borrows -- declared after, destroyed before) and the world.
    Fluid fluid{physics};
    // The lighter's muzzle flame: a CPU particle system (no GPU, no physics -- rising
    // specks with nothing to collide against), session state like the fluid, parked by the
    // same level swap. The fuller fires (logs, grill) are volumetric Flow; this is only the
    // lighter's little pilot tongue, which sits too close to the eye for the Flow pass.
    Flame flame;
    Camera camera{physics};
    Viewmodel viewmodel{Scene::kCubeModel};
    Input input;
    // Maps the raw keyboard the window feeds `input` into the game's logical
    // actions, loaded from controls.toml in Run before the loop starts.
    Actions actions;
    Audio audio;
    // Where the frame's time goes. Session state like the rest; its readout rides the
    // debug overlay (F3).
    Profiler profiler;

    // The top-level mode. Launches into the menu; a level is loaded behind it (for
    // the font atlas the menu draws with, and so play can start the instant an entry
    // is chosen). WindowProc reads and flips this on Escape, so it lives here rather
    // than as a Run local.
    GameState state = GameState::Menu;

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
        if (game->state == GameState::Playing) {
            if (game->input.mouse_look()) {
                // Already looking: the click is a gameplay button. Record it as
                // VK_LBUTTON so the action layer sees the primary-action press (the
                // held item's ability). WM_LBUTTONUP below clears it.
                game->input.OnKey(VK_LBUTTON, true);
            } else {
                // The first click after dropping into play (or returning from the
                // menu) recaptures the cursor for mouse-look. It must not also fire
                // the primary action, so it is deliberately not recorded as a press.
                game->input.SetMouseLook(hwnd, true);
            }
        } else {
            // On the menu the cursor stays free; the click confirms whatever entry
            // it lands on, which the menu loop resolves and acts on.
            game->input.OnLeftButtonDown();
        }
        return 0;
    case WM_LBUTTONUP:
        // Always release, even in the menu, so a button held across an Esc-to-menu
        // cannot stick down. A release with no matching recorded press is a no-op.
        game->input.OnKey(VK_LBUTTON, false);
        return 0;
    case WM_RBUTTONDOWN:
        // The other mouse buttons are gameplay buttons too (bindable as Mouse2 /
        // Mouse3), recorded only in play so a menu click never drives an action.
        if (game->state == GameState::Playing) {
            game->input.OnKey(VK_RBUTTON, true);
        }
        return 0;
    case WM_RBUTTONUP:
        game->input.OnKey(VK_RBUTTON, false);
        return 0;
    case WM_MBUTTONDOWN:
        if (game->state == GameState::Playing) {
            game->input.OnKey(VK_MBUTTON, true);
        }
        return 0;
    case WM_MBUTTONUP:
        game->input.OnKey(VK_MBUTTON, false);
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            // Escape backs out one level. In play it drops the mouse look and raises the
            // menu. On the menu it means "back", but what that backs out of depends on
            // which screen is up (cancel a rebind, step back a submenu, or quit) -- so
            // it is latched for the menu loop to resolve rather than acted on here.
            if (game->state == GameState::Playing) {
                game->input.SetMouseLook(hwnd, false);
                game->state = GameState::Menu;
            } else {
                game->input.OnEscape();
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
                                L"Grill Simulator - WASD walk, E grab, R reload",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr,
                                nullptr, instance, &game);
    if (hwnd == nullptr) {
        throw HrError(HRESULT_FROM_WIN32(GetLastError()), "CreateWindowEx");
    }

    RegisterRawMouse(hwnd);
    // The device and pipelines first -- the session's, independent of any level.
    game.renderer.Initialize(hwnd, kDefaultWidth, kDefaultHeight);

    // The levels the player can load, as the .toml files staged under assets/levels, in
    // the order the number keys select them. Loading is by file so a level is a text
    // edit, not a rebuild. An array of one for now; add a file and a name to grow it.
    const std::array<const char*, 2> level_files = {"backyard.toml", "campsite.toml"};
    // The names the menu shows for each level, parallel to level_files.
    const std::array<const char*, 2> level_names = {"Backyard", "Campsite"};
    const std::filesystem::path levels_dir = ExecutableDirectory() / "assets" / "levels";
    int current_level = 0;
    // Whether the bottom-left debug overlay is drawn. Toggled by the ToggleDebug action
    // (backtick); starts hidden, so the polished HUD is what shows by default.
    bool show_debug = false;

    // The control bindings for the whole session, in three layers: the built-in
    // defaults the Actions constructor seeds, the committed controls.toml over them,
    // then the per-user controls.user.toml the in-game keybinds editor writes -- so a
    // rebind survives a relaunch without touching the shipped defaults. A missing or
    // partial file at either layer just leaves the layer beneath it standing; a syntax
    // error or unknown key name throws out to the fatal-error box, naming the file.
    const std::filesystem::path controls_dir = ExecutableDirectory() / "assets";
    const std::filesystem::path user_controls_path = controls_dir / "controls.user.toml";
    game.actions.LoadFromFile(controls_dir / "controls.toml");
    game.actions.LoadFromFile(user_controls_path);

    // Loads a level by index: parses its file, unloads whatever is current (handing
    // its GPU geometry and physics actors back), builds the new one, and drops the
    // player at its spawn facing its way. Re-reading the file each time means an
    // edited level is picked up on the next switch, and a level knocked about in play
    // is restored rather than resumed. The renderer uploads the scene, aims the sun
    // and the static colliders become PhysX actors inside the World constructor.
    auto load_level = [&](int index) {
        current_level = index;
        const LevelDef level = levels::LoadFromFile(levels_dir / level_files[index]);
        game.world.reset();
        game.world.emplace(level, game.renderer, game.physics);
        game.camera.Respawn(level.player_spawn, level.player_facing);
        // Park any droplets still in flight: the fluid is session state, and a puddle
        // sprayed in one level must not survive into the next. (The Flow fire is reset by
        // the renderer's ReleaseScene as the level swaps.)
        game.fluid.Clear();
        // Likewise the flame: a lighter lit as the level changed must not leave its fire
        // hanging in the air of the new one.
        game.flame.Clear();
    };
    // A level is loaded at startup even though the game launches into the menu: the
    // menu draws with the font atlas that rides a level's upload, and having one
    // resident means play begins the instant an entry is chosen.
    load_level(0);
    game.state = GameState::Playing; // TEMP verification

    // TEMP increment-1 harness: cook every meat into a deformable volume and drop the
    // first over the patio, so the fall and settle can be watched in the log.
    std::ofstream soft_log("soft_body.log", std::ios::trunc);
    std::unique_ptr<SoftBody> soft_test;
    UINT soft_mesh = 0;
    std::vector<SoftMeshInstance> soft_draws;
    float soft_log_timer = 0.0f;
    {
        // Cook from the scene's own copy of the model, not a fresh load from disk: the
        // renderer's materials are indexed by position in Scene::Models(), so the soft
        // body has to name the same one to be drawn with the right textures. The burger
        // is found by matching the file's vertex count -- crude, but this whole block is
        // scaffolding that increment 4 replaces with Props owning the bodies.
        const Model on_disk = LoadGltfModel("assets/models/burger-raw.glb");
        const std::vector<Model>& scene_models = game.world->scene().Models();
        for (std::uint32_t i = 0; i < scene_models.size(); ++i) {
            if (scene_models[i].vertices.size() != on_disk.vertices.size()) {
                continue;
            }
            DirectX::XMFLOAT4X4 pose;
            DirectX::XMStoreFloat4x4(&pose, DirectX::XMMatrixTranslation(0.0f, 1.2f, -5.4f));
            auto body = std::make_unique<SoftBody>(game.physics, scene_models[i], pose, 1.0f);
            soft_log << "burger model=" << i << " active=" << body->Active()
                     << " status=\"" << body->Status() << "\""
                     << " sim_verts=" << body->SimPositions().size()
                     << " tets=" << (body->SimTetrahedra().size() / 4)
                     << " skin_verts=" << body->SkinnedVertices().size()
                     << " skin_tris=" << (body->SkinnedIndices().size() / 3)
                     << " runs=" << body->SkinnedPrimitives().size() << std::endl;
            if (body->Active()) {
                soft_mesh = game.renderer.CreateDeformableMesh(
                    i, body->SkinnedIndices(), body->SkinnedPrimitives(),
                    static_cast<UINT>(body->SkinnedVertices().size()));
                soft_test = std::move(body);
            }
            break;
        }
    }

    // Which menu screen is showing while game.state is Menu. Main is the launch list;
    // Options and Keybinds are reached from it and back out with Escape. A refinement of
    // the Menu state, kept as a loop local because only the loop navigates it.
    enum class MenuScreen { Main, LevelSelect, Options, Keybinds };
    MenuScreen screen = MenuScreen::Main;

    // The highlighted action on the level-complete results screen: 0 Replay, 1 Back to
    // Menu. Reset to Replay each time that screen is raised. A loop local like `screen`,
    // since only the loop navigates it.
    int results_selected = 0;

    // The pending level swap, driven by the Loading state. Rather than block on
    // load_level the instant a level is chosen -- which freezes the menu for the second
    // or two the build takes -- the choice records the target here and switches to
    // Loading. The Loading block then draws one "loading" frame, presents it, and only on
    // the next iteration performs the blocking build, so that frame is what sits on screen
    // through the stall. `loading_shown` tracks whether that frame has been presented yet.
    int pending_level = 0;
    bool loading_shown = false;

    // The main menu: Select Level opens the level list, Options opens that screen, Exit
    // closes the game. A fixed three entries, so their indices are constants.
    Menu menu({"Select Level", "Options", "Exit"});
    constexpr int kMainSelectLevel = 0;
    constexpr int kMainOptions = 1;
    constexpr int kMainExit = 2;

    // The level-select screen: one entry per level in level_files order, then Back. So a
    // chosen index below the back row is a level to load; the back row returns to Main.
    std::vector<std::string> level_entries;
    for (const char* name : level_names) {
        level_entries.emplace_back(name);
    }
    level_entries.emplace_back("Back");
    const int level_back_entry = static_cast<int>(level_names.size());
    Menu level_menu(std::move(level_entries));

    // The Options screen: for now just the keybinds editor and a way back.
    Menu options_menu({"Keybinds", "Back"});
    constexpr int kOptionsKeybinds = 0;
    constexpr int kOptionsBack = 1;

    // The keybinds screen's own selection and capture state. Its rows are the rebindable
    // actions (rebuilt each frame from the live bindings so the display stays current)
    // followed by Reset to Defaults and Back, so the two trailing indices are known
    // relative to the action count. `capturing` is true while waiting for the player to
    // press the key to bind to the selected action.
    int keybind_selected = 0;
    bool capturing = false;

    ShowWindow(hwnd, show_command);

    // The cursor's client position last frame, so the menu applies a hover only when
    // the mouse actually moves -- a resting mouse must not override the keyboard's
    // selection. Starts off-screen so the first real position counts as a move.
    POINT last_cursor{-1, -1};

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

        // Latch this frame's action states once, from the keyboard the message pump
        // just drained, so every reader below -- the level controls here, the walk
        // and the grab further down -- sees the same snapshot and the edge queries
        // fire exactly once per press.
        game.actions.Update(game.input);

        // The loading screen owns the frame while a level is being built. It is drawn and
        // presented once, then -- with that frame on screen -- the blocking build runs on
        // the next iteration and play begins. Splitting it across two iterations is what
        // makes the loading frame visible: were the build called in the same pass, it would
        // stall before its own present ever reached the display.
        if (game.state == GameState::Loading) {
            if (!loading_shown) {
                game.renderer.RenderLoading("LOADING", level_names[pending_level]);
                loading_shown = true;
                continue;
            }
            load_level(pending_level);
            game.state = GameState::Playing;
            loading_shown = false;
            continue;
        }

        // The menu owns the whole frame while it is up: navigate whichever screen is
        // showing (by mouse or keyboard), act on a confirm or a back, and draw it. The
        // world behind is neither stepped nor drawn, so nothing below this block runs
        // until play resumes.
        if (game.state == GameState::Menu) {
            // The client-space cursor, shared by whichever screen's hit-test runs below.
            // A hover only re-selects when the mouse actually moved, so a resting mouse
            // never fights the keyboard's selection.
            POINT cursor{};
            const bool have_cursor = GetCursorPos(&cursor) && ScreenToClient(hwnd, &cursor);
            const bool cursor_moved =
                have_cursor && (cursor.x != last_cursor.x || cursor.y != last_cursor.y);
            if (have_cursor) {
                last_cursor = cursor;
            }

            // The Main, Select Level and Options screens are all plain vertical lists, so
            // they share one block: the same Menu navigation, drawn with RenderMenu,
            // differing only in what a confirm and a back do. `active_screen` is the screen
            // this frame acts on and draws -- captured up front so that a confirm which
            // switches `screen` (e.g. into play) still renders a self-consistent hand-off
            // frame with the title and entries that belong together.
            if (screen == MenuScreen::Main || screen == MenuScreen::LevelSelect ||
                screen == MenuScreen::Options) {
                const MenuScreen active_screen = screen;
                Menu& active = active_screen == MenuScreen::Main         ? menu
                               : active_screen == MenuScreen::LevelSelect ? level_menu
                                                                          : options_menu;
                const int entry_count = static_cast<int>(active.entries().size());
                bool confirm = false;

                int hovered = -1;
                if (have_cursor) {
                    hovered = game.renderer.MenuEntryAt(cursor.x, cursor.y, entry_count);
                    if (hovered >= 0 && cursor_moved) {
                        active.SetSelected(hovered);
                    }
                }
                if (game.input.ConsumeLeftClick() && hovered >= 0) {
                    active.SetSelected(hovered);
                    confirm = true;
                }

                if (game.actions.WasPressed(Action::MenuUp)) {
                    active.MoveUp();
                }
                if (game.actions.WasPressed(Action::MenuDown)) {
                    active.MoveDown();
                }
                if (game.actions.WasPressed(Action::MenuConfirm)) {
                    confirm = true;
                }

                // Escape steps a sub-screen back to Main; on Main -- the top level -- it quits.
                if (game.input.ConsumeEscape()) {
                    if (screen == MenuScreen::Main) {
                        PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    } else {
                        screen = MenuScreen::Main;
                    }
                } else if (confirm && screen == MenuScreen::Main) {
                    const int choice = active.selected();
                    if (choice == kMainExit) {
                        PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    } else if (choice == kMainOptions) {
                        screen = MenuScreen::Options;
                    } else if (choice == kMainSelectLevel) {
                        screen = MenuScreen::LevelSelect;
                    }
                } else if (confirm && screen == MenuScreen::LevelSelect) {
                    if (active.selected() == level_back_entry) {
                        screen = MenuScreen::Main;
                    } else {
                        // Hand the chosen level to the Loading state, which draws a loading
                        // frame before performing the blocking build. Reset the screen to
                        // Main so a later Escape out of play lands on the top menu.
                        pending_level = active.selected();
                        game.state = GameState::Loading;
                        loading_shown = false;
                        screen = MenuScreen::Main;
                    }
                } else if (confirm) { // Options
                    if (active.selected() == kOptionsKeybinds) {
                        keybind_selected = 0;
                        capturing = false;
                        screen = MenuScreen::Keybinds;
                    } else if (active.selected() == kOptionsBack) {
                        screen = MenuScreen::Main;
                    }
                }

                const char* title = active_screen == MenuScreen::Main ? "GRILL SIMULATOR"
                                    : active_screen == MenuScreen::LevelSelect ? "SELECT LEVEL"
                                                                              : "OPTIONS";
                game.renderer.RenderMenu(title, active.entries(), active.selected());
                continue;
            }

            // The Keybinds screen. Its rows are the rebindable actions -- rebuilt each
            // frame from the live bindings so a rebind shows at once -- then Reset to
            // Defaults and Back.
            const std::vector<Actions::Binding> binds = game.actions.RebindableBindings();
            const int action_count = static_cast<int>(binds.size());
            const int reset_row = action_count;
            const int back_row = action_count + 1;
            const int row_count = action_count + 2;

            std::vector<std::string> labels;
            std::vector<std::string> values;
            labels.reserve(row_count);
            values.reserve(row_count);
            for (const Actions::Binding& b : binds) {
                labels.push_back(b.display);
                values.push_back(b.key);
            }
            labels.emplace_back("Reset to Defaults");
            values.emplace_back("");
            labels.emplace_back("Back");
            values.emplace_back("");

            if (capturing) {
                // Waiting for the player to press the key to bind. Escape cancels; any
                // other fresh key press becomes the binding, is saved, and ends capture.
                // Navigation and clicks are swallowed meanwhile so nothing moves the
                // selection out from under the pending bind.
                game.input.ConsumeLeftClick();
                if (game.input.ConsumeEscape()) {
                    game.input.CancelKeyCapture();
                    capturing = false;
                } else if (const std::optional<int> key = game.input.ConsumeCapturedKey()) {
                    if (keybind_selected >= 0 && keybind_selected < action_count) {
                        game.actions.Rebind(binds[keybind_selected].action, *key);
                        game.actions.SaveUserOverrides(user_controls_path);
                    }
                    capturing = false;
                }
            } else {
                bool confirm = false;
                int hovered = -1;
                if (have_cursor) {
                    hovered = game.renderer.KeybindRowAt(cursor.x, cursor.y, row_count);
                    if (hovered >= 0 && cursor_moved) {
                        keybind_selected = hovered;
                    }
                }
                if (game.input.ConsumeLeftClick() && hovered >= 0) {
                    keybind_selected = hovered;
                    confirm = true;
                }

                // Arrows/WS wrap through the rows, mirroring Menu's own wrapping.
                if (game.actions.WasPressed(Action::MenuUp)) {
                    keybind_selected = (keybind_selected + row_count - 1) % row_count;
                }
                if (game.actions.WasPressed(Action::MenuDown)) {
                    keybind_selected = (keybind_selected + 1) % row_count;
                }
                if (game.actions.WasPressed(Action::MenuConfirm)) {
                    confirm = true;
                }

                if (game.input.ConsumeEscape()) {
                    screen = MenuScreen::Options;
                } else if (confirm) {
                    if (keybind_selected == back_row) {
                        screen = MenuScreen::Options;
                    } else if (keybind_selected == reset_row) {
                        game.actions.ResetToDefaults();
                        game.actions.SaveUserOverrides(user_controls_path);
                    } else {
                        // An action row: listen for the next key press.
                        capturing = true;
                        game.input.BeginKeyCapture();
                    }
                }
            }

            game.renderer.RenderKeybinds("KEYBINDS", labels, values, keybind_selected, capturing);
            continue;
        }

        // The results screen owns the frame once the tray has been turned in: navigate its
        // two actions, act on a confirm or a back, and draw the pass/fail breakdown read
        // live from the level's Objectives (the world stays loaded behind it). Like the
        // menu, nothing below runs while it is up.
        if (game.state == GameState::LevelComplete) {
            const Objectives& objectives = game.world->objectives();
            const std::span<const FoodGoal> goals = objectives.Goals();
            // A clear needs a real ticket met: an empty-goal (sandbox) level cannot pass.
            const bool passed = !goals.empty() && objectives.Complete();

            // One breakdown line per order -- its name, band range and how many the
            // turned-in tray filled of the wanted count -- coloured met/missed by the row.
            std::vector<Renderer::ResultLine> lines;
            lines.reserve(goals.size());
            for (std::size_t i = 0; i < goals.size(); ++i) {
                const FoodGoal& goal = goals[i];
                std::string name = goal.type;
                for (char& c : name) {
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                }
                std::string band(DonenessName(goal.min));
                if (goal.max != goal.min) {
                    band += " to ";
                    band += DonenessName(goal.max);
                }
                const int filled = objectives.Filled(i);
                std::string text = name + " (" + band + ")   " + std::to_string(filled) + "/" +
                                   std::to_string(goal.count);
                lines.push_back(Renderer::ResultLine{std::move(text), filled >= goal.count});
            }

            const std::vector<std::string> actions = {"Replay", "Back to Menu"};
            const int action_count = static_cast<int>(actions.size());

            // Mouse hover re-selects only on real movement, so a resting mouse never fights
            // the keyboard -- the same rule the launch menu follows.
            POINT cursor{};
            const bool have_cursor = GetCursorPos(&cursor) && ScreenToClient(hwnd, &cursor);
            const bool cursor_moved =
                have_cursor && (cursor.x != last_cursor.x || cursor.y != last_cursor.y);
            if (have_cursor) {
                last_cursor = cursor;
            }

            bool confirm = false;
            int hovered = -1;
            if (have_cursor) {
                hovered = game.renderer.ResultsActionAt(cursor.x, cursor.y, action_count);
                if (hovered >= 0 && cursor_moved) {
                    results_selected = hovered;
                }
            }
            if (game.input.ConsumeLeftClick() && hovered >= 0) {
                results_selected = hovered;
                confirm = true;
            }
            if (game.actions.WasPressed(Action::MenuUp)) {
                results_selected = (results_selected + action_count - 1) % action_count;
            }
            if (game.actions.WasPressed(Action::MenuDown)) {
                results_selected = (results_selected + 1) % action_count;
            }
            if (game.actions.WasPressed(Action::MenuConfirm)) {
                confirm = true;
            }

            // Escape backs out to the launch menu, exactly like choosing Back to Menu.
            if (game.input.ConsumeEscape()) {
                game.state = GameState::Menu;
                screen = MenuScreen::Main;
            } else if (confirm) {
                if (results_selected == 0) { // Replay: reload the same level through Loading.
                    pending_level = current_level;
                    game.state = GameState::Loading;
                    loading_shown = false;
                } else { // Back to Menu.
                    game.state = GameState::Menu;
                    screen = MenuScreen::Main;
                }
            }

            const char* title = passed ? "ORDERS UP!" : "SERVICE FAILED";
            game.renderer.RenderResults(title, passed, lines, actions, results_selected);
            continue;
        }

        // The level controls are edge-triggered so a held key fires once: 1 selects the
        // backyard, 2 the campsite, R reloads whatever is current (restoring a level
        // knocked about in play). Read every frame so each stays current, then act on at
        // most one. Swapping
        // here, before anything reads the world this frame, means the step and draw below
        // run entirely on the freshly loaded level.
        const bool pick_backyard = game.actions.WasPressed(Action::SelectLevel1);
        const bool pick_campsite = game.actions.WasPressed(Action::SelectLevel2);
        const bool reload = game.actions.WasPressed(Action::ReloadLevel);
        // Backtick flips the debug overlay on the rising edge, so a single tap toggles.
        if (game.actions.WasPressed(Action::ToggleDebug)) {
            show_debug = !show_debug;
        }
        // A hotkey swap goes through the Loading state as well, so the same loading frame
        // covers the build here as when a level is chosen from the menu. Mouse look is left
        // as it is -- the loading screen does not use the cursor, and leaving it engaged
        // means play resumes seamlessly once the build finishes, as an in-place reload did
        // before this state existed.
        if (pick_backyard || pick_campsite || reload) {
            pending_level = pick_backyard ? 0 : pick_campsite ? 1 : current_level;
            game.state = GameState::Loading;
            loading_shown = false;
            continue;
        }

        // Advance the physics scene on its fixed clock: the props, furniture and the
        // player's controller all move on it. Simulate first, then read poses.
        game.profiler.BeginFrame();
        {
            Profiler::Scope scope{game.profiler, "physics"};
            game.physics.Step(dt);
        }
        // The fluid reads its droplet positions back from the stepped scene and injects
        // whatever last frame's spray queued -- after the step, which is what the readback
        // wants this frame's results of.
        {
            Profiler::Scope scope{game.profiler, "fluid"};
            game.fluid.Update(dt);
        }
        // TEMP harness: read the soft body back, log it, and queue it for drawing.
        soft_draws.clear();
        if (soft_test != nullptr && soft_test->Active()) {
            {
                Profiler::Scope scope{game.profiler, "soft skin"};
                soft_test->Update();
            }
            SoftMeshInstance draw;
            draw.mesh = soft_mesh;
            draw.vertices = soft_test->SkinnedVertices();
            draw.tint = DirectX::XMFLOAT3(1.0f, 1.0f, 1.0f);
            soft_draws.push_back(draw);
            soft_log_timer += dt;
            if (soft_log_timer >= 0.25f) {
                soft_log_timer = 0.0f;
                const DirectX::XMFLOAT3 c = soft_test->Centroid();
                DirectX::XMFLOAT3 lo = soft_test->SimPositions()[0];
                DirectX::XMFLOAT3 hi = lo;
                for (const DirectX::XMFLOAT3& p : soft_test->SimPositions()) {
                    lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y); lo.z = std::min(lo.z, p.z);
                    hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y); hi.z = std::max(hi.z, p.z);
                }
                DirectX::XMFLOAT3 slo{1e9f, 1e9f, 1e9f};
                DirectX::XMFLOAT3 shi{-1e9f, -1e9f, -1e9f};
                int nan_count = 0;
                for (const Vertex& v : soft_test->SkinnedVertices()) {
                    if (!std::isfinite(v.position.x) || !std::isfinite(v.position.y) ||
                        !std::isfinite(v.position.z) || !std::isfinite(v.normal.y)) {
                        ++nan_count;
                        continue;
                    }
                    slo.x = std::min(slo.x, v.position.x); slo.y = std::min(slo.y, v.position.y); slo.z = std::min(slo.z, v.position.z);
                    shi.x = std::max(shi.x, v.position.x); shi.y = std::max(shi.y, v.position.y); shi.z = std::max(shi.z, v.position.z);
                }
                for (const std::string& line : game.profiler.Report()) {
                    soft_log << "  " << line << std::endl;
                }
                soft_log << "sim c " << c.y << " ext " << (hi.y - lo.y)
                         << " | skin verts " << soft_test->SkinnedVertices().size()
                         << " tris " << (soft_test->SkinnedIndices().size() / 3)
                         << " runs " << soft_test->SkinnedPrimitives().size()
                         << " y[" << slo.y << "," << shi.y << "] ext "
                         << (shi.y - slo.y) << " x-ext " << (shi.x - slo.x)
                         << " nan " << nan_count << std::endl;
            }
        }
        // The flame ages its specks on the frame clock, not the physics one: it collides
        // with nothing, so it has no reason to wait for a step. Unconditional -- specks
        // already in the air burn out whether or not the lighter is lit this frame.
        game.flame.Update(dt);

        float mouse_dx = 0.0f;
        float mouse_dy = 0.0f;
        game.input.ConsumeMouseDelta(mouse_dx, mouse_dy);
        game.camera.Look(mouse_dx, mouse_dy);
        game.camera.Update(game.actions, dt);

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
        // The furniture's one player interaction: standing a toppled grill back up. Read
        // before the furniture and props updates so a right this frame is reflected in both
        // this frame's poses and cook. The hands are free only when nothing is carried, in
        // which case E rights the aimed-at grill rather than dropping a held item.
        game.world->furniture().Interact(camera_to_world, game.actions,
                                         !game.world->props().Carrying());
        // Read the dynamic furniture's body poses back into their draw instances, and
        // place its heat sources at wherever those bodies now sit -- before the props
        // update, so the meats cook against this frame's grate position.
        game.world->furniture().Update();
        {
            Profiler::Scope props_scope{game.profiler, "props"};
            game.world->props().Update(camera_to_world, game.actions, dt,
                                       game.world->furniture().HeatSources(),
                                       game.world->turn_in_zone(), game.world->fire_pit_zone(),
                                       game.world->objectives(), &game.fluid, &game.flame);
        }
        // Light the grill from a held flame: warm its grate toward the lighter (or a lit
        // log) the player holds to it and switch it on when it catches -- the same ignition
        // that lights the logs, now for the furniture. After the props update so the flame
        // sits where it does this frame, and before the Flow fires below so a grate that
        // just caught shows fire this frame.
        game.world->furniture().UpdateIgnition(game.world->props().ItemHeats(), dt);

        // Turning the loaded tray in at the delivery zone ends the level: Props latches it
        // during the Update above. Drop the mouse look and raise the results screen, whose
        // block runs next frame; the world stays loaded behind it so the breakdown reads
        // live from its Objectives. Skip the rest of this frame's world draw.
        if (game.world->props().TurnedIn()) {
            game.input.SetMouseLook(hwnd, false);
            game.state = GameState::LevelComplete;
            results_selected = 0;
            continue;
        }

        // Kept separate as well as combined: the scene passes want the product, but Flow
        // reconstructs its rays from the view and projection individually (see RenderFlow).
        const XMMATRIX view = game.camera.ViewMatrix();
        const XMMATRIX projection = game.camera.ProjectionMatrix(game.renderer.AspectRatio());
        const XMMATRIX view_projection = view * projection;
        Props& props = game.world->props();

        // The order ticket data, read once for the polished orders rail further down.
        const Objectives& objectives = game.world->objectives();
        const std::span<const FoodGoal> goals = objectives.Goals();

        // The debug overlay, anchored bottom-left and toggled by backtick (ToggleDebug):
        // each heat source's emitting temperature -- the one cook input the polished HUD
        // does not surface. Meat doneness, temperature and the order ticket now live on the
        // meats panel and orders list, and the win condition on the level-complete screen,
        // so they are gone from here. Left empty while hidden, which draws no overlay.
        std::vector<std::string> debug_lines;
        if (show_debug) {
            // Where last frame's time went, first: it is the reason the overlay is up
            // more often than not.
            const std::span<const std::string> timings = game.profiler.Report();
            debug_lines.assign(timings.begin(), timings.end());
            const std::span<const HeatSource> heat_sources = game.world->furniture().HeatSources();
            for (std::size_t i = 0; i < heat_sources.size(); ++i) {
                debug_lines.push_back(
                    "heat " + std::to_string(i) + ": " +
                    std::to_string(static_cast<int>(heat_sources[i].EmitterTempF())) + "F");
            }
        }

        // The same order ticket, presented for the player rather than the debugger: one
        // bulleted line per goal on the top-right list. Just the standing orders -- no
        // progress or pass/fail -- so here we only shape the loud uppercased name and the
        // quiet doneness caption, and hand across the wanted count. Built fresh each frame;
        // it is a handful of goals.
        std::vector<Renderer::OrderCard> order_cards;
        order_cards.reserve(goals.size());
        for (const FoodGoal& goal : goals) {
            std::string name = goal.type;
            for (char& c : name) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            std::string band(DonenessName(goal.min));
            if (goal.max != goal.min) {
                band += " to ";
                band += DonenessName(goal.max);
            }
            order_cards.push_back(Renderer::OrderCard{std::move(name), std::move(band), goal.count});
        }

        // The polished, always-on meats panel on the top-left: one card per cooking meat
        // in the yard, showing its doneness and live temperature -- the player-facing twin
        // of the debug overlay's meat lines. The renderer draws a gauge from the band
        // index, so here we only shape the loud uppercased name, the quiet band caption and
        // the temperature string. Built fresh each frame; it is a handful of meats.
        const int meat_band_count = static_cast<int>(CookInformation::Doneness::Burnt) + 1;
        std::vector<Renderer::MeatCard> meat_cards;
        for (const Props::MeatStatus& meat : props.MeatStatuses()) {
            std::string name = meat.name;
            for (char& c : name) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            std::string band(DonenessName(static_cast<CookInformation::Doneness>(meat.band)));
            std::string temp = std::to_string(meat.temp_f) + "F";
            meat_cards.push_back(Renderer::MeatCard{std::move(name), std::move(band),
                                                    std::move(temp), meat.band, meat_band_count,
                                                    meat.served});
        }

        // Merge the props' HUD affordances with the furniture's grill-righting one. Only
        // one is ever live at a time -- looking at a prop and at the grill are mutually
        // exclusive aims, and righting is offered only with empty hands -- so the props'
        // prompt and outline take precedence and the furniture's fill in when they are
        // empty. Combined here rather than in either owner, which each know only their own.
        Furniture& furniture = game.world->furniture();
        std::string prompt = props.PromptText();
        if (prompt.empty()) {
            prompt = furniture.PromptText();
        }
        std::vector<MeshInstance> highlights(props.HighlightInstances().begin(),
                                             props.HighlightInstances().end());
        const std::span<const MeshInstance> grill_highlight = furniture.HighlightInstances();
        highlights.insert(highlights.end(), grill_highlight.begin(), grill_highlight.end());

        // The world draw list: the props where they lie plus the lighter's muzzle flame --
        // plain mesh instances, merged here so the renderer keeps its one world span. Empty
        // appends cost nothing. The lighter-fluid droplets are no longer in this list: they
        // are screen-space sphere impostors now, drawn by the renderer's own fluid pass from
        // fluid.Points() below, not tinted cubes. (The fuller fires -- logs, grill -- are
        // volumetric Flow, not mesh instances; see flow_emitters below. The lighter's flame
        // stays a CPU particle because its muzzle sits too close to the eye for Flow.)
        std::vector<MeshInstance> world_instances(props.WorldInstances().begin(),
                                                  props.WorldInstances().end());
        const std::span<const MeshInstance> flame = game.flame.Instances();
        world_instances.insert(world_instances.end(), flame.begin(), flame.end());

        // The frame's fire/smoke sources for NVIDIA Flow: the grate of any lit furniture
        // (the grill, once the player has lit it) plus every burning log the props report.
        // Merged into the one list the renderer steps the sim from.
        std::vector<FlowEmitter> flow_emitters;
        for (const HeatSource& hot : game.world->furniture().HeatSources()) {
            if (hot.IsOn()) {
                const XMFLOAT3 o = hot.Origin();
                // A small, low coal bed seated down inside the grill bowl (below the grate
                // origin), so the flames rise up out of the pit rather than sitting on top of
                // it as a bright block. Kept modest -- a bed of coals, not a bonfire -- and in
                // the ballpark of a single lit log so it reads as cooking heat, not a pyre.
                XMFLOAT4X4 grate;
                XMStoreFloat4x4(&grate, XMMatrixTranslation(o.x, o.y - 0.02f, o.z));
                flow_emitters.push_back({grate, {0.17f, 0.07f, 0.17f}, 0.70f, 0.11f, 0.13f, 0.58f});
            }
        }
        const std::span<const FlowEmitter> log_fires = props.FlowEmitters();
        flow_emitters.insert(flow_emitters.end(), log_fires.begin(), log_fires.end());

        {
            // Submission plus the swapchain wait, so with vsync on this is most of the
            // frame however little work there is. It is here to be subtracted, not read.
            Profiler::Scope scope{game.profiler, "render"};
            game.renderer.Render(game.world->scene(), world_instances, highlights,
                                 game.viewmodel.Pose(camera_to_world), props.HeldInstances(),
                                 view_projection, game.camera.Position(), prompt,
                                 debug_lines, order_cards, meat_cards, view, projection, dt,
                                 flow_emitters, game.fluid.Points(), soft_draws);
        }
        game.profiler.EndFrame();
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
