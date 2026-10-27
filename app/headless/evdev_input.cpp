#include "evdev_input.h"

#include "frontend/presses.h"
#include "frontend/gamepad.h"

#include <linux/input.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>

namespace oap {

namespace {

constexpr auto kRescanInterval = std::chrono::seconds(3);

std::optional<Button> mapKey(unsigned code) {
    switch (code) {
    case KEY_PLAYPAUSE:
    case KEY_SPACE: return Button::PlayPause;
    case KEY_PREVIOUSSONG:
    case KEY_LEFT: return Button::Back;
    case KEY_NEXTSONG:
    case KEY_RIGHT: return Button::Forward;
    case KEY_UP: return Button::Up;
    case KEY_DOWN: return Button::Down;
    case KEY_ENTER:
    case KEY_KPENTER:
    case KEY_OK:
    case KEY_SELECT: return Button::Ok;
    case KEY_ESC:
    case KEY_BACKSPACE:
    case KEY_HOMEPAGE:
    case KEY_BACK: return Button::Home;
    case KEY_T:
    case KEY_VOICECOMMAND: return Button::Talk;
    case KEY_P:
    case KEY_POWER: return Button::Power;
    case KEY_VOLUMEUP:
    case KEY_KPPLUS:
    case KEY_EQUAL: return Button::VolumeUp;
    case KEY_VOLUMEDOWN:
    case KEY_KPMINUS:
    case KEY_MINUS: return Button::VolumeDown;
    case KEY_MENU:
    case KEY_O: return Button::Options;
    case KEY_F13: return Button::BackDeep;
    case KEY_F14: return Button::ForwardDeep;
    default: return std::nullopt;
    }
}

// Game controllers (hid-playstation, xpad, ...) use the BTN_* gamepad codes.
// Which control does what is in defaultGamepadLayout() (frontend/gamepad.cpp).
std::optional<PadControl> mapPad(unsigned code) {
    switch (code) {
    case BTN_DPAD_UP: return PadControl::DpadUp;
    case BTN_DPAD_DOWN: return PadControl::DpadDown;
    case BTN_DPAD_LEFT: return PadControl::DpadLeft;
    case BTN_DPAD_RIGHT: return PadControl::DpadRight;
    case BTN_SOUTH: return PadControl::South;
    case BTN_EAST: return PadControl::East;
    case BTN_WEST: return PadControl::West;
    case BTN_NORTH: return PadControl::North;
    case BTN_TL: return PadControl::L1;
    case BTN_TR: return PadControl::R1;
    case BTN_TL2: return PadControl::L2;
    case BTN_TR2: return PadControl::R2;
    case BTN_SELECT: return PadControl::Select;
    case BTN_START: return PadControl::Start;
    case BTN_MODE: return PadControl::Mode;
    case BTN_THUMBL: return PadControl::ThumbL;
    case BTN_THUMBR: return PadControl::ThumbR;
    default: return std::nullopt;
    }
}

void applyChanges(App& app, const std::vector<ButtonChange>& changes) {
    for (const auto& c : changes) {
        if (c.down) app.keyDown(c.button);
        else app.keyUp(c.button);
    }
}

bool testBit(const unsigned long* bits, unsigned bit) {
    constexpr unsigned kBits = sizeof(unsigned long) * 8;
    return (bits[bit / kBits] >> (bit % kBits)) & 1;
}

} // namespace

EvdevInput::~EvdevInput() {
    for (auto& [path, d] : devices_) closeDevice(d);
}

size_t EvdevInput::open(const std::string& only) {
    only_ = only;
    if (!only_.empty()) {
        openDevice(only_);
    } else {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator("/dev/input", ec))
            if (entry.path().filename().string().rfind("event", 0) == 0) openDevice(entry.path().string());
    }
    nextScan_ = std::chrono::steady_clock::now() + kRescanInterval;
    return devices_.size();
}

bool EvdevInput::openDevice(const std::string& path) {
    if (devices_.count(path)) return true;
    int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errno == EACCES && !only_.empty())
            std::fprintf(stderr, "input: %s: permission denied (add yourself to the 'input' group)\n", path.c_str());
        return false;
    }

    unsigned long keys[KEY_CNT / (sizeof(unsigned long) * 8) + 1] = {};
    unsigned long switches[SW_CNT / (sizeof(unsigned long) * 8) + 1] = {};
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
    ioctl(fd, EVIOCGBIT(EV_SW, sizeof switches), switches);
    bool useful = testBit(switches, SW_MUTE_DEVICE) || testBit(keys, KEY_H) || testBit(keys, BTN_GAMEPAD);
    for (unsigned code = 0; code < KEY_CNT && !useful; ++code)
        useful = testBit(keys, code) && mapKey(code);
    if (!useful) {
        ::close(fd);
        return false;
    }

    if (testBit(switches, SW_MUTE_DEVICE)) { // the hold switch may already be on at startup
        unsigned long state[SW_CNT / (sizeof(unsigned long) * 8) + 1] = {};
        ioctl(fd, EVIOCGSW(sizeof state), state);
        pendingHold_ = testBit(state, SW_MUTE_DEVICE);
    }

    char name[256] = "?";
    ioctl(fd, EVIOCGNAME(sizeof name), name);
    std::printf("input: %s (%s)\n", path.c_str(), name);
    devices_[path] = Device{.fd = fd, .path = path};
    return true;
}

void EvdevInput::closeDevice(Device& d) {
    if (d.fd >= 0) ::close(d.fd);
    d.fd = -1;
}

void EvdevInput::addPollFds(std::vector<pollfd>& fds) const {
    for (const auto& [path, d] : devices_) fds.push_back({d.fd, POLLIN, 0});
}

void EvdevInput::process(App& app) {
    if (pendingHold_) {
        app.setHold(*pendingHold_);
        pendingHold_.reset();
    }
    for (auto it = devices_.begin(); it != devices_.end();) {
        Device& d = it->second;
        input_event ev[32];
        bool gone = false;
        for (;;) {
            ssize_t n = ::read(d.fd, ev, sizeof ev);
            if (n < 0) {
                gone = errno != EAGAIN && errno != EINTR; // ENODEV: unplugged
                break;
            }
            for (size_t i = 0; i < size_t(n) / sizeof(input_event); ++i)
                handle(d, app, ev[i].type, ev[i].code, ev[i].value);
        }
        if (gone) {
            std::printf("input: %s removed\n", d.path.c_str());
            applyChanges(app, d.pad.releaseAll()); // a controller that dropped off mid-press
            for (Button b : d.deepHeld) app.keyUp(b);  // and keys: a held volume key would repeat for ever
            for (Button b : d.keysHeld) app.keyUp(b);
            closeDevice(d);
            it = devices_.erase(it);
        } else {
            ++it;
        }
    }

    if (only_.empty() && std::chrono::steady_clock::now() >= nextScan_) open();
}

void EvdevInput::handle(Device& d, App& app, unsigned type, unsigned code, int value) {
    if (type == EV_SW && code == SW_MUTE_DEVICE) {
        app.setHold(value != 0);
        return;
    }
    if (type == EV_ABS && code == ABS_HAT0X) { // D-pad as an axis (DualSense, Xbox pads)
        applyChanges(app, d.pad.setAxis(PadControl::DpadLeft, PadControl::DpadRight, value));
        return;
    }
    if (type == EV_ABS && code == ABS_HAT0Y) {
        applyChanges(app, d.pad.setAxis(PadControl::DpadUp, PadControl::DpadDown, value));
        return;
    }
    if (type != EV_KEY || value == 2) return; // 2 = autorepeat
    bool down = value == 1;
    if (auto control = mapPad(code)) {
        applyChanges(app, d.pad.set(*control, down));
        return;
    }
    if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) {
        d.shift = down;
        return;
    }
    if (code == KEY_H) {
        if (down) app.setHold(!app.hold());
        return;
    }
    auto b = mapKey(code);
    if (!b) return;
    auto deep = deepOf(*b);
    if (down) {
        d.keysHeld.insert(*b);
        app.keyDown(*b);
        if (deep && d.shift) {
            d.deepHeld.insert(*deep);
            app.keyDown(*deep);
        }
    } else {
        if (deep && d.deepHeld.erase(*deep)) app.keyUp(*deep);
        d.keysHeld.erase(*b);
        app.keyUp(*b);
    }
}

} // namespace oap
