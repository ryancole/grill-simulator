#include "menu.hpp"

#include <utility>

Menu::Menu(std::vector<std::string> entries) : entries_(std::move(entries)) {}

void Menu::MoveUp() {
    if (entries_.empty()) {
        return;
    }
    const int count = static_cast<int>(entries_.size());
    // + count before the modulo so stepping up off the top lands on the last entry
    // rather than a negative index.
    selected_ = (selected_ + count - 1) % count;
}

void Menu::MoveDown() {
    if (entries_.empty()) {
        return;
    }
    selected_ = (selected_ + 1) % static_cast<int>(entries_.size());
}

void Menu::SetSelected(int index) {
    if (entries_.empty()) {
        return;
    }
    const int count = static_cast<int>(entries_.size());
    selected_ = index < 0 ? 0 : (index >= count ? count - 1 : index);
}
