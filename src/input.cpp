#include "input.hpp"

void Input::OnKey(WPARAM key, bool down) {
    // A rebind in progress swallows the next fresh press: it becomes the captured key
    // and never lands in keys_, so the key chosen for, say, Move Forward does not also
    // register as held the instant it is bound. A key already down when capture began is
    // ignored -- otherwise the Enter or Space used to open the rebind would be captured
    // by its own auto-repeat before the player pressed anything new.
    if (capturing_ && down) {
        if (key < keys_.size() && keys_.test(key)) {
            return;
        }
        captured_key_ = static_cast<int>(key);
        capturing_ = false;
        return;
    }
    if (key < keys_.size()) {
        keys_.set(key, down);
    }
}

void Input::OnRawInput(const RAWINPUT& raw) {
    if (!mouse_look_ || raw.header.dwType != RIM_TYPEMOUSE) {
        return;
    }

    const RAWMOUSE& mouse = raw.data.mouse;
    if (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
        const POINT position{mouse.lLastX, mouse.lLastY};
        if (has_absolute_) {
            mouse_dx_ += static_cast<float>(position.x - last_absolute_.x);
            mouse_dy_ += static_cast<float>(position.y - last_absolute_.y);
        }
        last_absolute_ = position;
        has_absolute_ = true;
    } else {
        mouse_dx_ += static_cast<float>(mouse.lLastX);
        mouse_dy_ += static_cast<float>(mouse.lLastY);
        has_absolute_ = false;
    }
}

bool Input::IsKeyDown(int virtual_key) const {
    return virtual_key >= 0 && static_cast<size_t>(virtual_key) < keys_.size() &&
           keys_.test(static_cast<size_t>(virtual_key));
}

void Input::ReleaseAllKeys() {
    keys_.reset();
}

void Input::ConsumeMouseDelta(float& dx, float& dy) {
    dx = mouse_dx_;
    dy = mouse_dy_;
    mouse_dx_ = 0.0f;
    mouse_dy_ = 0.0f;
}

void Input::SetMouseLook(HWND hwnd, bool enabled) {
    if (enabled == mouse_look_) {
        return;
    }

    // ShowCursor keeps an internal counter, so the hide and the show have to be
    // paired. Toggling only on a real state change is what keeps them balanced.
    mouse_look_ = enabled;
    if (enabled) {
        ShowCursor(FALSE);
        UpdateClip(hwnd);
    } else {
        ClipCursor(nullptr);
        ShowCursor(TRUE);
    }

    // Motion captured across the transition belongs to neither mode.
    has_absolute_ = false;
    mouse_dx_ = 0.0f;
    mouse_dy_ = 0.0f;
}

void Input::UpdateClip(HWND hwnd) {
    if (!mouse_look_) {
        return;
    }

    RECT client{};
    if (!GetClientRect(hwnd, &client)) {
        return;
    }

    POINT top_left{client.left, client.top};
    POINT bottom_right{client.right, client.bottom};
    ClientToScreen(hwnd, &top_left);
    ClientToScreen(hwnd, &bottom_right);

    const RECT screen{top_left.x, top_left.y, bottom_right.x, bottom_right.y};
    ClipCursor(&screen);
}
