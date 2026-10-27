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
    case Button::VolumeUp:    return "Volume up";
    case Button::VolumeDown:  return "Volume down";
    case Button::Options:     return "Options";
    case Button::Count:     break;
    }
    return "?";
}

namespace {
bool repeats(Button button) { return button == Button::VolumeUp || button == Button::VolumeDown; }

std::optional<Button> shallowOf(Button deep) {
    if (deep == Button::BackDeep) return Button::Back;
    if (deep == Button::ForwardDeep) return Button::Forward;
    return std::nullopt;
}

std::optional<Button> deepOf(Button shallow) {
    if (shallow == Button::Back) return Button::BackDeep;
    if (shallow == Button::Forward) return Button::ForwardDeep;
    return std::nullopt;
}
} // namespace

std::optional<ButtonEvent> GestureDetector::press(Button button, Clock::time_point now) {
    State& s = states_[static_cast<size_t>(button)];
    s.down = true;
    s.since = now;
    s.longFired = false;
    if (repeats(button)) {
        s.consumed = true; // no long press, and the release says nothing
        s.nextRepeat = now + kRepeatDelay;
        return ButtonEvent{button, Gesture::Short};
    }
    auto shallow = shallowOf(button);
    if (!shallow) {
        // A light press while its deep stage is down (stages out of order, or the
        // first contact bouncing) belongs to the deep press that already fired.
        auto deep = deepOf(button);
        s.consumed = deep && states_[static_cast<size_t>(*deep)].down;
        return std::nullopt;
    }
    s.consumed = true; // the deep stage never fires a short or long press of its own
    State& first = states_[static_cast<size_t>(*shallow)];
    if (!first.down) return ButtonEvent{*shallow, Gesture::Deep};
    first.consumed = true;
    if (first.longFired) { // pushed deeper only after the long press: one step is enough
        first.longFired = false; // but pushing deeper again is a new step
        return std::nullopt;
    }
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
        if (s.down && repeats(static_cast<Button>(i))) {
            if (now >= s.nextRepeat) {
                s.nextRepeat = now + kRepeatInterval;
                events.push_back({static_cast<Button>(i), Gesture::Repeat});
            }
            continue;
        }
        if (s.down && !s.consumed && now - s.since >= longPress_) {
            s.consumed = true;
            s.longFired = true;
            events.push_back({static_cast<Button>(i), Gesture::Long});
        }
    }
    return events;
}

} // namespace oap
