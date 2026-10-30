#include "presses.h"

#include <charconv>
#include <cstdio>
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
        {"home", Button::Home}, {"talk", Button::Talk}, {"power", Button::Power},
        {"volup", Button::VolumeUp}, {"voldown", Button::VolumeDown}, {"options", Button::Options}};
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
        if (name == "wait") {
            auto [end, error] = std::from_chars(arg.data(), arg.data() + arg.size(), step.waitMs);
            if (error != std::errc() || end != arg.data() + arg.size() || step.waitMs < 0) {
                std::fprintf(stderr, "script: '%s' needs a number of milliseconds\n", word.c_str());
                continue;
            }
        } else if (name == "quit" && arg.empty()) step.quit = true;
        else {
            for (const auto& [n, b] : kNames)
                if (name == n) step.button = b;
            if (!step.button) {
                std::fprintf(stderr, "script: unknown step '%s'\n", word.c_str());
                continue;
            }
            if (!arg.empty() && arg != "long" && arg != "deep" && arg != "long:deep" && arg != "deep:long") {
                std::fprintf(stderr, "script: unknown modifiers in '%s'\n", word.c_str());
                continue;
            }
            step.longPress = arg == "long" || arg == "long:deep" || arg == "deep:long";
            step.deep = arg == "deep" || arg == "long:deep" || arg == "deep:long";
            if (step.deep && !deepOf(*step.button)) {
                std::fprintf(stderr, "script: '%s' has no deep stage\n", word.c_str());
                continue;
            }
        }
        steps.push_back(step);
    }
    return steps;
}

bool PressSequencer::tick(App& app, HeldButtons* held, std::chrono::steady_clock::time_point now) {
    if (now < next_ || steps_.empty()) return true;
    const PressStep& step = steps_.front();
    if (step.quit) return false;
    auto down = [&](Button b) {
        if (!held || held->press(b)) app.keyDown(b);
    };
    auto up = [&](Button b) {
        if (!held || held->release(b)) app.keyUp(b);
    };
    if (!step.button) { // a pause; a step with neither a button nor a wait does nothing
        next_ = now + std::chrono::milliseconds(step.waitMs);
        steps_.pop_front();
    } else if (!pressed_) {
        std::printf("press: %s%s\n", buttonName(*step.button),
                    step.deep && step.longPress ? " (deep hold)" : step.longPress ? " (long)" : step.deep ? " (deep)" : "");
        down(*step.button);
        if (step.deep) down(*deepOf(*step.button));
        pressed_ = true;
        next_ = now + std::chrono::milliseconds(step.longPress ? 1600 : 80);
    } else {
        if (step.deep) up(*deepOf(*step.button));
        up(*step.button);
        pressed_ = false;
        steps_.pop_front();
        next_ = now + gap_;
    }
    return true;
}

} // namespace oap
