#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <filesystem>
#include <string>
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
    // Held to crouch: shrinks the player -- capsule and eyeline both drop -- for as
    // long as it is down, and slows the walk while shrunk. A hold, not a toggle.
    Crouch,
    Interact,
    // The main action, on the left mouse button by default. Triggers the ability of
    // whatever carryable the player is holding -- different per item, defined by the
    // item's catalog type -- and does nothing empty-handed. Rebindable like the rest.
    PrimaryAction,
    // Developer shortcuts: reload the current level, jump straight to a level by number,
    // and toggle the bottom-left debug overlay. Bound in controls.toml alongside the
    // gameplay actions so no raw key code is left hardcoded in the game loop.
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

    // One rebindable action as the keybinds screen sees it: the action itself, the
    // human label to show ("Move Forward"), and the name of its current primary key
    // ("W", or "Unbound" when nothing is bound). Only the gameplay actions are
    // rebindable; the developer shortcuts and menu navigation are omitted.
    struct Binding {
        Action action;
        std::string display;
        std::string key;
    };

    // The rebindable actions in menu order, each with its label and current primary
    // key name. Rebuilt on demand -- cheap, a handful of entries -- so it always
    // reflects the latest Rebind/ResetToDefaults.
    std::vector<Binding> RebindableBindings() const;

    // Makes `key` (a Win32 virtual-key code) the sole binding for `action`. First
    // strips `key` from any other rebindable action that held it, so no two gameplay
    // actions answer to the same key -- the last one to claim it wins, the other is
    // left unbound. A no-op if `action` is not rebindable.
    void Rebind(Action action, int key);

    // Restores every action to its built-in default binding, discarding any file or
    // in-game overrides currently in effect.
    void ResetToDefaults();

    // Writes the current primary key of each rebindable action to a controls.toml-shaped
    // [bindings] table at `path` (an "unbound" action is written as an empty list). This
    // is the per-user override file the game loads over the committed defaults, so a
    // rebind survives a relaunch. Silently does nothing if the file cannot be written --
    // a failed save must not crash the game mid-menu.
    void SaveUserOverrides(const std::filesystem::path& path) const;

    // The display name of the action's primary (first) key -- "E", "Mouse1" -- for a HUD
    // prompt that names the real binding rather than a hardcoded key. Empty when the
    // action is unbound, "?" for a key with no writable name (only a hand-edited config
    // can produce one). Reflects the current bindings, so it tracks a rebind.
    std::string KeyName(Action action) const;

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
