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
    State& contact = states_[static_cast<size_t>(button)];
    if (contact.down) return std::nullopt;
    auto shallow = shallowOf(button);
    auto deep = deepOf(button);
    const bool navigation = shallow || deep;
    State& first = shallow ? states_[static_cast<size_t>(*shallow)] : contact;
    const bool otherDown = shallow ? first.down : deep && states_[static_cast<size_t>(*deep)].down;
    contact.down = true;
    if (!navigation || !otherDown) {
        first.since = now;
        first.longFired = false;
        first.consumed = false;
        first.cancelled = false;
    }
    if (first.cancelled) return std::nullopt;
    if (repeats(button)) {
        first.consumed = true;
        first.nextRepeat = now + kRepeatDelay;
        return ButtonEvent{button, Gesture::Short};
    }
    if (shallow) {
        first.consumed = true;
        return ButtonEvent{*shallow, Gesture::Deep};
    }
    return std::nullopt;
}

std::optional<ButtonEvent> GestureDetector::release(Button button, Clock::time_point) {
    State& contact = states_[static_cast<size_t>(button)];
    if (!contact.down) return std::nullopt;
    contact.down = false;
    auto shallow = shallowOf(button);
    auto deep = deepOf(button);
    State& first = shallow ? states_[static_cast<size_t>(*shallow)] : contact;
    const bool navigation = shallow || deep;
    const bool otherDown = shallow ? first.down : deep && states_[static_cast<size_t>(*deep)].down;
    if (otherDown) {
        if (shallow && !first.cancelled) return ButtonEvent{*shallow, Gesture::Light};
        return std::nullopt;
    }
    const bool cancelled = first.cancelled;
    const bool consumed = first.consumed;
    first.cancelled = false;
    first.consumed = false;
    first.longFired = false;
    if (cancelled) return std::nullopt;
    if (navigation && consumed) return ButtonEvent{shallow.value_or(button), Gesture::Release};
    if (!consumed) return ButtonEvent{button, Gesture::Short};
    return std::nullopt;
}

std::vector<ButtonEvent> GestureDetector::poll(Clock::time_point now) {
    std::vector<ButtonEvent> events;
    for (size_t i = 0; i < states_.size(); ++i) {
        const Button button = static_cast<Button>(i);
        if (shallowOf(button)) continue; // deep contacts share their light contact's deadline
        State& s = states_[i];
        auto deep = deepOf(button);
        const bool down = s.down || (deep && states_[static_cast<size_t>(*deep)].down);
        if (!down || s.cancelled) continue;
        if (repeats(button)) {
            if (now >= s.nextRepeat) {
                s.nextRepeat = now + kRepeatInterval;
                events.push_back({button, Gesture::Repeat});
            }
        } else if (!s.longFired && now - s.since >= longPress_) {
            s.consumed = true;
            s.longFired = true;
            events.push_back({button, Gesture::Long});
        }
    }
    return events;
}

std::vector<ButtonEvent> GestureDetector::cancel(bool preservePower) {
    std::vector<ButtonEvent> events;
    for (size_t i = 0; i < states_.size(); ++i) {
        const Button button = static_cast<Button>(i);
        if (shallowOf(button) || (preservePower && button == Button::Power)) continue;
        State& s = states_[i];
        auto deep = deepOf(button);
        if (!s.down && !(deep && states_[static_cast<size_t>(*deep)].down)) continue;
        if (deep && s.consumed && !s.cancelled) events.push_back({button, Gesture::Cancel});
        s.cancelled = true;
    }
    return events;
}

} // namespace oap
