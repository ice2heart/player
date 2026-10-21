#pragma once

#include <functional>
#include <string>
#include <vector>

namespace oap {

// One entry in a spoken list. Exactly one of submenu / action / adjust is
// normally set; value is spoken after the label ("Speech rate, 220 words per minute").
struct MenuItem {
    std::string label;
    std::function<std::string()> value;
    std::function<void(int direction)> adjust;      // Back/Forward change the value
    std::function<std::vector<MenuItem>()> submenu; // built lazily when opened
    std::function<void()> action;
};

struct MenuLevel {
    std::string title;
    std::vector<MenuItem> items;
    size_t index = 0;
};

} // namespace oap
