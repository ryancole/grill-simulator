#include "actions.hpp"
#include "audio.hpp"
#include "camera.hpp"
#include "dx_common.hpp"
#include "fluid.hpp"
#include "furniture.hpp"
#include "input.hpp"
#include "level.hpp"
#include "menu.hpp"
#include "physics.hpp"
#include "props.hpp"
#include "renderer.hpp"
#include "scene.hpp"
#include "viewmodel.hpp"
#include "world.hpp"

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <fstream>
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
    Camera camera{physics};
    Viewmodel viewmodel{Scene::kCubeModel};
    Input input;
    // Maps the raw keyboard the window feeds `input` into the game's logical
    // actions, loaded from controls.toml in Run before the loop starts.
    Actions actions;
    Audio audio;

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
        // must not survive into (or pre-soak the fire pit of) the fresh level.
        game.fluid.Clear();
    };
    // A level is loaded at startup even though the game launches into the menu: the
    // menu draws with the font atlas that rides a level's upload, and having one
    // resident means play begins the instant an entry is chosen.
    load_level(0);

    // Which menu screen is showing while game.state is Menu. Main is the launch list;
    // Options and Keybinds are reached from it and back out with Escape. A refinement of
    // the Menu state, kept as a loop local because only the loop navigates it.
    enum class MenuScreen { Main, LevelSelect, Options, Keybinds };
    MenuScreen screen = MenuScreen::Main;

    // The highlighted action on the level-complete results screen: 0 Replay, 1 Back to
    // Menu. Reset to Replay each time that screen is raised. A loop local like `screen`,
    // since only the loop navigates it.
    int results_selected = 0;

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
                        // Load the chosen level (re-reading its file) and drop into play.
                        // The switch takes effect next frame; this one still draws the
                        // menu, which is identical, so the hand-off is invisible. Reset the
                        // screen to Main so a later Escape out of play lands on the top menu.
                        load_level(active.selected());
                        game.state = GameState::Playing;
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
                if (results_selected == 0) { // Replay: reload the same level and drop in.
                    load_level(current_level);
                    game.state = GameState::Playing;
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
        if (pick_backyard) {
            load_level(0);
        } else if (pick_campsite) {
            load_level(1);
        } else if (reload) {
            load_level(current_level);
        }

        // Advance the physics scene on its fixed clock: the props, furniture and the
        // player's controller all move on it. Simulate first, then read poses.
        game.physics.Step(dt);
        // The fluid reads its droplet positions back from the stepped scene and injects
        // whatever last frame's spray queued -- after the step (the readback wants this
        // frame's results) and before the props update (which tests the fire pit's
        // wetness against the fresh positions).
        game.fluid.Update(dt);

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
        game.world->props().Update(camera_to_world, game.actions, dt,
                                   game.world->furniture().HeatSources(),
                                   game.world->turn_in_zone(), game.world->fire_pit_zone(),
                                   game.world->objectives(), &game.fluid);

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

        const XMMATRIX view_projection =
            game.camera.ViewMatrix() * game.camera.ProjectionMatrix(game.renderer.AspectRatio());
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

        // The world draw list: the props where they lie plus any lighter-fluid droplets
        // in flight -- both plain mesh instances, merged here so the renderer keeps its
        // one world span. Empty droplets cost an empty append.
        std::vector<MeshInstance> world_instances(props.WorldInstances().begin(),
                                                  props.WorldInstances().end());
        const std::span<const MeshInstance> droplets = game.fluid.Instances();
        world_instances.insert(world_instances.end(), droplets.begin(), droplets.end());

        game.renderer.Render(game.world->scene(), world_instances, highlights,
                             game.viewmodel.Pose(camera_to_world), props.HeldInstances(),
                             view_projection, game.camera.Position(), prompt,
                             debug_lines, order_cards, meat_cards);
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
