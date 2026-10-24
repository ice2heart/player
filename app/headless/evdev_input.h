#pragma once

#include "core/app.h"

#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

struct pollfd;

namespace oap {

// Buttons from Linux input devices: gpio-keys, the MCU's USB HID interface,
// media keys and ordinary keyboards. Devices are found under /dev/input and
// rescanned every few seconds, so a keyboard can be plugged in later.
//
// Key map (until hal.toml exists, see docs/hal.md §3.3):
//   PlayPause  KEY_PLAYPAUSE KEY_SPACE        Back/Forward  KEY_PREVIOUSSONG/NEXTSONG, Left/Right
//   Up/Down    KEY_UP KEY_DOWN                OK            KEY_ENTER KEY_KPENTER KEY_OK KEY_SELECT
//   Home       KEY_ESC KEY_BACKSPACE KEY_HOMEPAGE KEY_BACK
//   Talk       KEY_T KEY_VOICECOMMAND         Power         KEY_P KEY_POWER
//   BackDeep/ForwardDeep  KEY_F13/KEY_F14, or Shift+Left/Right on a keyboard
//   Hold       switch SW_MUTE_DEVICE, or KEY_H toggles it
class EvdevInput {
public:
    ~EvdevInput();

    // Opens every input device with at least one mapped key. With `only`
    // non-empty, opens just that device. Returns the number of devices open.
    size_t open(const std::string& only = {});
    // For poll(): one entry per open device.
    void addPollFds(std::vector<pollfd>& fds) const;
    // Reads pending events from every device and feeds them to `app`.
    void process(App& app);

private:
    struct Device {
        int fd = -1;
        std::string path;
        bool shift = false;
        std::set<Button> deepHeld; // deep stages pressed via Shift+arrow
    };

    bool openDevice(const std::string& path);
    void closeDevice(Device& d);
    void handle(Device& d, App& app, unsigned type, unsigned code, int value);

    std::map<std::string, Device> devices_;
    std::string only_;
    std::optional<bool> pendingHold_; // switch state read at open, applied on the UI thread
    std::chrono::steady_clock::time_point nextScan_{};
};

} // namespace oap
