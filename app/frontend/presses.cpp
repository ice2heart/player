#include "presses.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace oap {

std::optional<Button> deepOf(Button b) {
    if (b == Button::Back) return Button::BackDeep;
    if (b == Button::Forward) return Button::ForwardDeep;
    return std::nullopt;
}

std::vector<PressStep> parseScript(const std::string& text) {
    static const std::pair<const char*, Button> kNames[] = {
        {"play", Button::PlayPause}, {"left", Button::Back}, {"right", Button::Forward},
        {"up", Button::Up}, {"down", Button::Down}, {"ok", Button::Ok},
        {"home", Button::Home}, {"talk", Button::Talk}, {"power", Button::Power}};
    std::vector<PressStep> steps;
    std::istringstream in(text);
    std::string word;
    while (in >> word) {
        PressStep step;
        std::string name = word, arg;
        if (auto colon = word.find(':'); colon != std::string::npos) {
            name = word.substr(0, colon);
            arg = word.substr(colon + 1);
        }
        if (name == "wait") step.waitMs = std::atoi(arg.c_str());
        else if (name == "quit") step.quit = true;
        else {
            for (const auto& [n, b] : kNames)
                if (name == n) step.button = b;
            if (!step.button) {
                std::fprintf(stderr, "script: unknown step '%s'\n", word.c_str());
                continue;
            }
            step.longPress = arg == "long";
            step.deep = arg == "deep" && deepOf(*step.button);
        }
        steps.push_back(step);
    }
    return steps;
}

bool PressSequencer::tick(App& app, std::set<Button>* held) {
    auto now = std::chrono::steady_clock::now();
    if (now < next_ || steps_.empty()) return true;
    const PressStep& step = steps_.front();
    if (step.quit) return false;
    if (step.waitMs) {
        next_ = now + std::chrono::milliseconds(step.waitMs);
        steps_.pop_front();
    } else if (!pressed_) {
        std::printf("press: %s%s\n", buttonName(*step.button),
                    step.longPress ? " (long)" : step.deep ? " (deep)" : "");
        if (held) held->insert(*step.button);
        app.keyDown(*step.button);
        if (step.deep) app.keyDown(*deepOf(*step.button));
        pressed_ = true;
        next_ = now + std::chrono::milliseconds(step.longPress ? 1600 : 80);
    } else {
        if (held) held->erase(*step.button);
        if (step.deep) app.keyUp(*deepOf(*step.button));
        app.keyUp(*step.button);
        pressed_ = false;
        steps_.pop_front();
        next_ = now + gap_;
    }
    return true;
}

} // namespace oap
