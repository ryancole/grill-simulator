#pragma once

#include <string>
#include <vector>

// Which top-level mode the game is in. It launches into Menu; confirming a level
// entry switches to Playing, and Escape out of play raises the menu again.
enum class GameState {
    Menu,
    Playing,
};

// The launch menu's model: a vertical list of selectable entries and the one
// currently highlighted. It deliberately knows nothing about what an entry *does*
// -- the game loop maps the chosen index onto loading a level or exiting -- so a
// future Options screen is one more entry here and one more case there, no change
// to how the list is drawn or driven.
class Menu {
public:
    explicit Menu(std::vector<std::string> entries);

    // Moves the highlight one entry up or down, wrapping around the ends so the
    // list behaves as a loop. A no-op when the menu is empty.
    void MoveUp();
    void MoveDown();
    // Sets the highlight directly -- a mouse hover or click landing on an entry.
    // Clamps into range; a no-op when the menu is empty.
    void SetSelected(int index);

    int selected() const { return selected_; }
    const std::vector<std::string>& entries() const { return entries_; }

private:
    std::vector<std::string> entries_;
    int selected_ = 0;
};
