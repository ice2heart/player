#include "buttons.h"

namespace oap {

const char* buttonName(Button button) {
    switch (button) {
    case Button::PlayPause: return "Play/Pause";
    case Button::Back:      return "Back";
    case Button::Forward:   return "Forward";
    case Button::Up:        return "Up";
    case Button::Down:      return "Down";
    case Button::Ok:        return "OK";
    case Button::Home:      return "Home";
    case Button::Talk:      return "Talk";
    case Button::Power:     return "Power";
    case Button::BackDeep:    return "Back deep";
    case Button::ForwardDeep: return "Forward deep";
    case Button::Count:     break;
    }
    return "?";
}

namespace {
std::optional<Button> shallowOf(Button deep) {
    if (deep == Button::BackDeep) return Button::Back;
    if (deep == Button::ForwardDeep) return Button::Forward;
    return std::nullopt;
}
} // namespace

std::optional<ButtonEvent> GestureDetector::press(Button button, Clock::time_point now) {
    State& s = states_[static_cast<size_t>(button)];
    s.down = true;
    s.since = now;
    auto shallow = shallowOf(button);
    s.consumed = shallow.has_value(); // the deep stage never fires a short or long press of its own
    if (!shallow) return std::nullopt;
    State& first = states_[static_cast<size_t>(*shallow)];
    bool longFired = first.down && first.consumed; // pushed deeper only after the long press: one step is enough
    first.consumed = true;
    if (longFired) return std::nullopt;
    return ButtonEvent{*shallow, Gesture::Deep};
}

std::optional<ButtonEvent> GestureDetector::release(Button button, Clock::time_point) {
    State& s = states_[static_cast<size_t>(button)];
    if (!s.down) return std::nullopt;
    s.down = false;
    if (s.consumed) return std::nullopt;
    return ButtonEvent{button, Gesture::Short};
}

std::vector<ButtonEvent> GestureDetector::poll(Clock::time_point now) {
    std::vector<ButtonEvent> events;
    for (size_t i = 0; i < states_.size(); ++i) {
        State& s = states_[i];
        if (s.down && !s.consumed && now - s.since >= longPress_) {
            s.consumed = true;
            events.push_back({static_cast<Button>(i), Gesture::Long});
        }
    }
    return events;
}

} // namespace oap
