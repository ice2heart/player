#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace oap {

// How a list is sorted, which decides what Left / Right do in it (docs/navigation.md): in a
// list sorted by name they jump to the previous / next letter, in any other list by
// kMenuJump items. A list that is not marked jumps by items.
enum class ListOrder {
    None,     // not sorted by name
    ByName,   // by title, "The Wall" under T
    ByArtist, // by artist, "The Band" under B; "Unknown artist" is a group of its own
};

// One entry in a spoken list. Which parts are set decides the item's type
// (docs/navigation.md): value is spoken after the label ("Speech rate, 220 words
// per minute"), and OK opens the submenu or runs the action.
struct MenuItem {
    std::string label;
    std::function<std::string()> value;
    std::function<std::vector<MenuItem>()> submenu; // built lazily when opened
    std::function<size_t()> selected;               // submenu: entry to start on (the current value)
    ListOrder submenuOrder = ListOrder::None;       // submenu: how its list is sorted
    bool unlettered = false; // in a list sorted by name: a group of its own, spoken with no letter
    std::function<void()> action;
};

// One entry of the screen stack: a spoken list, or a program such as the player.
// OK pushes a screen, Home pops one.
struct Screen {
    enum class Kind { Menu, Player };
    Kind kind = Kind::Menu;
    std::string title;
    std::vector<MenuItem> items;                  // Menu only
    std::function<std::vector<MenuItem>()> build; // Menu only: rebuilds items when returned to; none for a list from the server
    size_t index = 0;
    std::uint64_t id = 0; // unique per App: how an answer from the server finds its screen
    ListOrder order = ListOrder::None; // Menu only
};

} // namespace oap
