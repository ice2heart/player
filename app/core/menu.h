#pragma once

#include <functional>
#include <string>
#include <vector>

namespace oap {

// One entry in a spoken list. Which parts are set decides the item's type
// (docs/navigation.md): value is spoken after the label ("Speech rate, 220 words
// per minute"), adjust makes Left / Right change it in place, and OK opens the
// submenu or runs the action.
struct MenuItem {
    std::string label;
    std::function<std::string()> value;
    std::function<bool(int direction)> adjust;      // Left / Right; false at the end of the range
    std::function<std::vector<MenuItem>()> submenu; // built lazily when opened
    std::function<size_t()> selected;               // submenu: entry to start on (the current value)
    std::function<void()> action;
};

// One entry of the screen stack: a spoken list, or a program such as the player.
// OK pushes a screen, Home pops one.
struct Screen {
    enum class Kind { Menu, Player };
    Kind kind = Kind::Menu;
    std::string title;
    std::vector<MenuItem> items;                  // Menu only
    std::function<std::vector<MenuItem>()> build; // Menu only: rebuilds items when the screen is returned to
    size_t index = 0;
};

} // namespace oap
