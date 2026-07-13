#pragma once

#include <windows.h>

#include <bitset>

// Keyboard state plus accumulated raw mouse motion. The window procedure feeds
// it; the game loop drains the mouse delta once per frame.
//
// Look uses WM_INPUT rather than WM_MOUSEMOVE: cursor positions are clamped to
// the screen and quantised by pointer acceleration, so a fast flick that hits
// the edge of the monitor loses the rest of its motion.
class Input {
public:
    void OnKey(WPARAM key, bool down);
    void OnRawInput(const RAWINPUT& raw);

    // A left-button click awaiting a reader. WM_LBUTTONDOWN records it while the
    // menu is up; the menu reads it (ConsumeLeftClick) to confirm the hovered entry.
    // Kept apart from mouse-look so a click means "confirm" on the menu and "capture
    // the cursor" in play.
    void OnLeftButtonDown() { left_click_pending_ = true; }
    // Whether a left click has arrived since the last call; clears it.
    bool ConsumeLeftClick() {
        const bool clicked = left_click_pending_;
        left_click_pending_ = false;
        return clicked;
    }

    bool IsKeyDown(int virtual_key) const;
    // Forgets every held key. Keyups are not delivered while the window is in
    // the background, so without this the player keeps walking after Alt+Tab.
    void ReleaseAllKeys();

    // Returns the motion accumulated since the last call, in mouse counts, and
    // clears it.
    void ConsumeMouseDelta(float& dx, float& dy);

    bool mouse_look() const { return mouse_look_; }
    // Hides the cursor and confines it to the client area, so a look that runs
    // past the window edge does not click on whatever is behind it.
    void SetMouseLook(HWND hwnd, bool enabled);
    // The clip rectangle is in screen coordinates, so it goes stale whenever the
    // window moves or resizes.
    void UpdateClip(HWND hwnd);

private:
    std::bitset<256> keys_;

    float mouse_dx_ = 0.0f;
    float mouse_dy_ = 0.0f;

    // Remote desktop sessions and tablets report absolute positions instead of
    // deltas, so the previous sample is the only way to recover motion.
    POINT last_absolute_{};
    bool has_absolute_ = false;

    bool mouse_look_ = false;

    // Set on a left-button press, cleared when the menu consumes it.
    bool left_click_pending_ = false;
};
