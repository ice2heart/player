# Hardware Abstraction Layer (HAL) — Plan

> Status: **plan, not implemented.** Last updated: 2026-10-21.
> Context: [../SPEC.md](../SPEC.md) §3, §4.2, §6.4 · code as built: [architecture.md](architecture.md).

## 1. Why

`App` now fakes everything below the audio pipeline. `statusReport()` says
"Battery 80 percent, simulated", and the Wi-Fi menu speaks fixed strings. The device
needs real battery, charger, headphone jack, Wi-Fi and Bluetooth, and the simulator and
tests need controllable stand-ins for them. The HAL extends the pattern `services.h`
already uses: **core declares interfaces, platforms implement them, and tests use fakes.**

Out of scope: audio output (`AudioOutput` already exists), decoding and TTS (`app/media/`),
and button input. Front ends still call `keyDown`/`keyUp`/`setHold`. Bluetooth headset
buttons (AVRCP) reach Linux as an evdev device, so the input front end handles them too.

## 2. Rules

1. **The UI thread never blocks.** Every HAL call from `App` returns at once. Slow work
   (scan, connect, pair, update) starts with a call, and its result comes back later as an event.
2. **One event queue.** Backends run on their own threads (D-Bus, netlink, the MCU link) and
   `post()` events into a mutex-protected queue. `App::tick()` drains it on the UI thread,
   so `App` stays single-threaded (architecture §3).
3. **Cached state for getters.** `battery()` or `wifi.state()` return the last known
   snapshot. No I/O happens on the UI thread.
4. **Every part is optional.** A board without Bluetooth passes `nullptr`, and its menus are
   left out. The simulator can switch parts off to test that path.
5. **Core stays dependency-free.** Interfaces live in `app/core/hal.h`. Backends live in
   `app/hal/sim/` and `app/hal/linux/`. Core never includes them.
6. **Policy stays in `App`.** The HAL reports facts ("jack unplugged"). `App` decides what
   to do about them ("pause and say 'Headphones unplugged, paused.'") so that it can be tested.

## 3. Interfaces (sketch)

```cpp
// app/core/hal.h — all calls from the UI thread, all non-blocking.

enum class Output { Speaker, Headphones, Bluetooth, Usb };

class AudioRouting {                 // "audio controls"
public:
    virtual std::vector<Output> available() const = 0;
    virtual Output current() const = 0;
    virtual void select(std::optional<Output> output) = 0; // nullopt = automatic (newest wins)
};

struct BatteryState { int percent; bool charging; bool charger; bool full; };

class Power {
public:
    virtual BatteryState battery() const = 0;
    virtual void requestSuspend() = 0;          // front end suspends after tick() returns, see §6
    virtual void requestPowerOff() = 0;
    virtual void requestReboot() = 0;
    virtual void setWakeAlarm(std::optional<std::time_t> at) = 0; // RTC: podcast downloads
};

enum class WifiState { Off, Disconnected, Connecting, Connected };
struct WifiNetwork { std::string ssid; int signal; bool secured; bool known; };

class Wifi {
public:
    virtual WifiState state() const = 0;
    virtual std::optional<WifiNetwork> connected() const = 0;
    virtual void setEnabled(bool on) = 0;
    virtual void scan() = 0;                                   // -> WifiScanDone
    virtual void connect(const std::string& ssid, const std::string& passphrase) = 0; // -> WifiChanged
    virtual std::vector<std::string> knownNetworks() const = 0;
    virtual void forget(const std::string& ssid) = 0;
};

struct BtDevice { std::string address; std::string name; bool paired; bool connected; bool audio; };

class Bluetooth {
public:
    virtual bool enabled() const = 0;
    virtual void setEnabled(bool on) = 0;
    virtual void startDiscovery() = 0;                      // -> BtDeviceFound ...
    virtual void stopDiscovery() = 0;
    virtual void pair(const std::string& address) = 0;      // -> BtPairing (may carry a PIN to speak)
    virtual void connect(const std::string& address) = 0;   // -> BtConnection
    virtual void disconnect(const std::string& address) = 0;
    virtual void forget(const std::string& address) = 0;
    virtual std::vector<BtDevice> devices() const = 0;      // paired + found
};

class Haptics { public: virtual void pulse(Earcon pattern) = 0; };   // mirrors earcons

struct StorageState { uint64_t freeBytes, totalBytes; bool usbHostConnected; };
class Storage {
public:
    virtual StorageState state() const = 0;
    virtual void setMassStorage(bool on) = 0;   // export the data partition over USB
};
```

Events, drained in `tick()`:

```cpp
struct OutputChanged     { Output from, to; bool lost; };   // lost = jack pulled / BT dropped
struct BatteryChanged    { BatteryState state; };           // backend posts on 1 % steps and charger edges
struct WifiChanged       { WifiState state; std::string ssid; std::string error; };
struct WifiScanDone      { std::vector<WifiNetwork> networks; };
struct BtDeviceFound     { BtDevice device; };
struct BtPairing         { std::string address; bool ok; std::string pin; std::string error; };
struct BtConnection      { BtDevice device; };
struct UsbHostChanged    { bool connected; };
using HalEvent = std::variant<OutputChanged, BatteryChanged, WifiChanged, WifiScanDone,
                              BtDeviceFound, BtPairing, BtConnection, UsbHostChanged>;

class HalEvents {            // thread-safe; backends post, App drains
public:
    void post(HalEvent e);
    std::vector<HalEvent> drain();
};
```

Grouping. `App`'s constructor already takes seven arguments. Replace them with one struct:

```cpp
struct Services {
    Speech& speech; MediaPlayer& player; SoundEffects& sounds; VolumeControl& volume; Clock& clock;
    HalEvents& events;
    AudioRouting* audio = nullptr; Power* power = nullptr; Wifi* wifi = nullptr;
    Bluetooth* bluetooth = nullptr; Haptics* haptics = nullptr; Storage* storage = nullptr;
};
```

## 4. What App does with it

| Event / need | App behaviour |
|---|---|
| `OutputChanged{lost=true}` | Pause, save the position, say "Headphones unplugged, paused." or "Bluetooth disconnected, paused." (SPEC §3.4). Never switch to the speaker while playing. |
| `OutputChanged` (new output) | Restore that output's own volume (`volume.<output>`). Short announcement. |
| `BatteryChanged` | Say it at 20 %, 10 % and 5 % (once each, re-armed after charging). At 3 %, save and power off. Say when charging starts and when it is full. |
| `WifiChanged` | Speak only if the user is in a Wi-Fi menu or started the connect. Errors are always spoken. |
| `WifiScanDone` / `BtDeviceFound` | Refresh the open list in place and keep the selection. Say "Found 3 networks." |
| `BtPairing{pin}` | Speak the PIN digit by digit, then "Confirm on the other device". |
| Status (System menu, Play long) | Built from `power->battery()`, `wifi->connected()` and `storage->state()`. |

Speech priority. Events must not talk over the user. Low-priority messages (connected,
found) wait until speech is idle and are dropped when they go stale. Critical ones (battery
5 %, output lost) interrupt, like a key press does.

Menu changes this enables:

```
System
├── Status
├── Wi-Fi        → On/off · Networks → <scan results> · Saved networks → <ssid> → Forget
├── Bluetooth    → On/off · Pair new device → <found> · Paired devices → <dev> → Connect / Disconnect / Forget
├── Audio output → Automatic · Speaker · Headphones · <BT device>
├── USB file transfer  (mass storage on/off)
└── …
Settings
├── Maximum volume       (hearing protection)
└── Pause when headphones are unplugged
```

`MenuLevel` needs a `refresh` builder so that a list such as scan results can be rebuilt
while it is open (the `submenu` lambda is only called once today).

## 5. Backends

| Interface | Simulator (`app/hal/sim/`) | Linux device (`app/hal/linux/`) |
|---|---|---|
| `AudioRouting` | Checkboxes for jack and BT headphones | libpulse: sink and port events (PipeWire through pipewire-pulse handles jack detection and BT A2DP sinks). With bare ALSA: `SW_HEADPHONE_INSERT` from evdev plus bluez-alsa. |
| `Power` | Battery slider, charger checkbox, drain over time | sysfs `power_supply` (fuel gauge, charger) plus udev netlink for changes. Suspend through `/sys/power/state` or an MCU command. RTC through `/sys/class/rtc/rtc0/wakealarm` or the MCU. |
| `Wifi` | Editable list of fake networks and passwords, with connect delays | **iwd** over D-Bus (`net.connman.iwd`): small, fast, no wpa_supplicant config files. |
| `Bluetooth` | Fake devices, pairing delay, optional PIN | **BlueZ** over D-Bus (`org.bluez`), with our own Agent (NoInputNoOutput or DisplayOnly so the PIN can be spoken). Audio routing through PipeWire. |
| `Haptics` | Flash the window border | MCU command, or a GPIO / PWM vibration motor. |
| `Storage` | `std::filesystem::space` | `statvfs`, USB gadget (`g_mass_storage` or ConfigFS) with the data partition unmounted while exported. |

Simulator UI: a **Hardware** ImGui panel shows and edits every sim backend, and script steps
`jack:out`, `battery:15` and `charger:on` make walkthroughs repeatable.
Sim backends use `Clock` for their delays, so the same classes can serve as rich test fakes.

Linux threading: one **HAL thread** runs a `poll()` loop over the D-Bus connection, the
netlink socket and the MCU UART. Commands from the UI thread go to it through a small
command queue, and results come back through `HalEvents`.

## 6. Suspend needs the front end

On Linux, writing `mem` to `/sys/power/state` blocks until resume. `App` must therefore
not suspend from inside `tick()`:

1. `App::sleep()` saves state, plays the sleep earcon and calls `power->requestSuspend()`.
2. Once speech has finished, the front end's main loop sees the request and suspends.
3. After resume, the front end calls `app.onResume()`, which says where the user is (as `wake()` does now).

`Mode::Sleeping` stays as it is in the simulator, where a key press is the "resume".

## 7. Phases

| Phase | Work | Fits milestone |
|---|---|---|
| H0 | `Services` struct, `HalEvents`, `hal.h`. Sim and fake `Power`/`Storage` replace the hard-coded status strings. Tests for battery announcements. | M0 |
| H1 | `AudioRouting` (jack, pause on unplug, volume per output), sim Hardware panel | M0 / M2 |
| H2 | Linux `Power` + `AudioRouting` (sysfs, libpulse), real suspend through the front end | M2, M5 |
| H3 | `Wifi`: sim, then iwd. Live menu refresh. Passphrase entry waits on spelling mode (M3), or `config.toml` / AP setup (SPEC §6.3) | M4 |
| H4 | `Bluetooth`: sim, then BlueZ. BT as an audio output. | M4+ |
| H5 | `Haptics`, LED, MCU protocol | M5 |

Each phase follows the usual rule: tests drive `App` with fakes, and every test is checked with a sabotage.

## 8. Open questions

1. **D-Bus client library**: sd-bus without systemd (`basu`) plus `sdbus-c++`, or plain `libdbus-1`? GDBus pulls in GLib.
2. **PipeWire or PulseAudio** on the device. PipeWire with WirePlumber gives BT audio with
   little work but costs boot time and RAM. Bare ALSA would mean bluez-alsa. Measure in M2.
3. **Bluetooth in v1?** SPEC does not list it yet. The front-runner board (Radxa Zero 3W) has
   BT on its Wi-Fi module. A2DP adds about 150–250 ms of latency, which is noticeable for TTS feedback.
4. Should haptics be a decorator over `SoundEffects` (every earcon also buzzes) instead of a separate call?
