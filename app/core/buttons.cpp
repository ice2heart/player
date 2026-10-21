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
    case Button::Count:     break;
    }
    return "?";
}

void GestureDetector::press(Button button, Clock::time_point now) {
    State& s = states_[static_cast<size_t>(button)];
    s.down = true;
    s.longSent = false;
    s.since = now;
}

std::optional<ButtonEvent> GestureDetector::release(Button button, Clock::time_point) {
    State& s = states_[static_cast<size_t>(button)];
    if (!s.down) return std::nullopt;
    s.down = false;
    if (s.longSent) return std::nullopt;
    return ButtonEvent{button, Gesture::Short};
}

std::vector<ButtonEvent> GestureDetector::poll(Clock::time_point now) {
    std::vector<ButtonEvent> events;
    for (size_t i = 0; i < states_.size(); ++i) {
        State& s = states_[i];
        if (s.down && !s.longSent && now - s.since >= longPress_) {
            s.longSent = true;
            events.push_back({static_cast<Button>(i), Gesture::Long});
        }
    }
    return events;
}

} // namespace oap
