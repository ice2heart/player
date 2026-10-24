#pragma once

#include "core/app.h"

#include <chrono>
#include <deque>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace oap {

// One automated button press (or a pause between presses).
struct PressStep {
    std::optional<Button> button;
    bool longPress = false;
    bool deep = false; // Back / Forward pressed through to the second stage
    int waitMs = 0;
    bool quit = false;
};

// The deep (second) stage of a two-stage key, if it has one.
std::optional<Button> deepOf(Button b);

// "down ok wait:3000 right:long right:deep quit". Names: play left right up down ok home talk power.
std::vector<PressStep> parseScript(const std::string& text);

// Presses buttons one after another with realistic timing: a short press is held
// 80 ms, a long press 1600 ms. Used by --script and by the headless terminal input,
// which only sees key presses, never releases.
class PressSequencer {
public:
    explicit PressSequencer(std::chrono::milliseconds gap) : gap_(gap) {}

    void push(PressStep step) { steps_.push_back(step); }
    void push(const std::vector<PressStep>& steps) { steps_.insert(steps_.end(), steps.begin(), steps.end()); }

    // Call every tick. Returns false once a quit step is reached.
    // `held`, if given, tracks the buttons currently pressed (for drawing).
    bool tick(App& app, std::set<Button>* held = nullptr);

private:
    std::deque<PressStep> steps_;
    std::chrono::milliseconds gap_;
    bool pressed_ = false;
    std::chrono::steady_clock::time_point next_{};
};

} // namespace oap
