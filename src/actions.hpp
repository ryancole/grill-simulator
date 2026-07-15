#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <filesystem>
#include <vector>

class Input;

// The logical inputs the game reads, one step removed from the physical keys that
// drive them. Game code asks "is MoveForward active?" rather than "is 'W' down?",
// so a rebind is a data edit in controls.toml and a future gamepad source can feed
// the same actions without touching a single consumer.
//
// Count is the sentinel end of the list, and its integer value is the number of
// actions -- it is used to size the binding and state arrays, so every real action
// must sit above it.
enum class Action {
    MoveForward,
    MoveBack,
    MoveLeft,
    MoveRight,
    Jump,
    Sprint,
    Interact,
    // Developer shortcuts: reload the current level, jump straight to one of the
    // levels, and toggle the top-left debug overlay. Bound in controls.toml alongside
    // the gameplay actions so no raw key code is left hardcoded in the game loop.
    ReloadLevel,
    SelectLevel1,
    SelectLevel2,
    ToggleDebug,
    // Menu navigation, read only while the launch/pause menu is up. They share
    // keys with movement (Up/W, Down/S, Enter/Space) with no conflict, since the
    // menu and the gameplay walk are never read on the same frame.
    MenuUp,
    MenuDown,
    MenuConfirm,
    Count,
};

// Maps physical keys to logical actions and latches their state once per frame, so
// consumers can ask whether an action is held (IsActive) or was just pressed or
// released this frame (WasPressed / WasReleased).
//
// Bindings come from controls.toml (see LoadFromFile). A missing file, a missing
// [bindings] table, or an action the file does not name all fall back to the
// built-in defaults the constructor seeds, so the game always has controls.
class Actions {
public:
    // Seeds the built-in default bindings, so an Actions that never loads a file is
    // already the WASD / Space / Shift / E layout the game shipped with.
    Actions();

    // Reads bindings from a controls.toml over the defaults already in place: an
    // action the file names is rebound, one it omits keeps its default. Throws
    // std::runtime_error -- naming the file -- on a TOML syntax error or an unknown
    // key name, the same way the level loader does. An absent file is not an error:
    // the defaults stand and nothing is thrown.
    void LoadFromFile(const std::filesystem::path& path);

    // Latches this frame's action states from the keyboard. Call once per frame,
    // after the message pump has updated `input` and before any consumer reads an
    // action, so the held- and edge-queries below all see the same snapshot.
    void Update(const Input& input);

    // Held this frame: any key bound to the action is down.
    bool IsActive(Action action) const;
    // The frame the action goes down (a rising edge): true only on the transition,
    // so a held key fires once -- this is the "one grab per press" the interact and
    // level controls want.
    bool WasPressed(Action action) const;
    // The frame the action goes up (a falling edge).
    bool WasReleased(Action action) const;

private:
    static constexpr std::size_t kCount = static_cast<std::size_t>(Action::Count);

    // The keys (Win32 virtual-key codes) bound to each action; any one of them
    // drives it, so an action can answer to several keys at once. Seeded with the
    // defaults in the constructor, replaced per action by LoadFromFile.
    std::array<std::vector<int>, kCount> bindings_;

    // This frame's and last frame's held state, one bit per action. The edge
    // queries are the difference between the two.
    std::bitset<kCount> current_;
    std::bitset<kCount> previous_;
};
