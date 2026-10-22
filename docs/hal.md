# Hardware Abstraction Layer (HAL) — Plan

> Status: **plan, not implemented.** Last updated: 2026-10-22.
> Context: [../SPEC.md](../SPEC.md) §3, §4.2, §6.4 · code as built: [architecture.md](architecture.md).

## 1. Why

`App` now fakes everything below the audio pipeline. `statusReport()` says
"Battery 80 percent, simulated", and the Wi-Fi menu speaks fixed strings. The device
needs real battery, charger, headphone jack, Wi-Fi and Bluetooth, and the simulator and
tests need controllable stand-ins for them. The HAL extends the pattern `services.h`
already uses: **core declares interfaces, backends implement them, and tests use fakes.**

The hardware will vary. Our device gets its buttons, battery and haptics from a USB
microcontroller and manages Wi-Fi and Bluetooth over D-Bus. An **Android phone app**
(Play Store) runs the same core, does everything through Android APIs, and adds touch
gestures as a way to control it. A desktop has none of this hardware but has a keyboard. So the HAL is not
one "platform" that is swapped as a whole. **Each part picks its backend separately**, and
the build includes only the backends that were chosen (§3).

Input is part of the HAL: device buttons, keyboards, touch gestures and Bluetooth headset
buttons all arrive as HAL events (§5). A plugged-in keyboard also gives **text entry**, which
setup needs for Wi-Fi passphrases and server URLs long before spelling mode or STT exist.

Out of scope: decoding and TTS (`app/media/`). They are chosen in the same way (§3.5), but
their interfaces stay in `services.h`.

## 2. Rules

1. **The UI thread never blocks.** Every HAL call from `App` returns at once. Slow work
   (scan, connect, pair, update) starts with a call, and its result comes back later as an event.
2. **One event queue.** Backends run on their own threads (D-Bus, netlink, the MCU link,
   the Android looper) and `post()` events into a mutex-protected queue. Posting wakes the
   front end's main loop. `App::tick()` drains the queue on the UI thread, so `App` stays
   single-threaded (architecture §3).
3. **Cached state for getters.** `battery()` or `wifi.state()` return the last known
   snapshot. No I/O happens on the UI thread.
4. **Every part is optional.** A part that is not built, is turned off in `hal.toml`, or
   whose hardware is missing at run time, is `nullptr`, and its menus are left out. A
   backend that can do only some of an interface says so with capability flags (§4), and
   `App` hides the actions it lacks. The simulator can switch parts off to test that path
   (`oap-sim --hal no-bluetooth.toml`, §3.3).
5. **Each part is chosen on its own.** Backends do not assume each other. The build picks
   the backends per part. `hal.toml` picks among them only when one image runs on several
   board revisions (§3.3).
6. **Core stays dependency-free.** Interfaces live in `app/core/hal.h`. Backends live in
   `app/hal/<backend>/`. Core never includes them and never reads `hal.toml`.
7. **Policy stays in `App`.** The HAL reports facts ("jack unplugged", "key A pressed",
   "swipe left"). `App` decides what to do about them ("pause and say 'Headphones
   unplugged, paused.'", "type a into the passphrase", "skip back") so that it can be tested.

## 3. Choosing backends per part

### 3.1 Parts and backends

| Part | `none` | `sim` | Linux | Android |
|---|---|---|---|---|
| Input (several allowed) | — | SDL keyboard + ImGui buttons | `evdev` (GPIO keys, MCU buttons over USB HID, keyboards, AVRCP), `mcu` (buttons over the serial protocol) | `android` (touch gestures, KeyEvent, MediaSession) |
| `AudioRouting` | ✓ | ✓ | `pulse` (PipeWire or PulseAudio), `alsa` (evdev jack switch + bluez-alsa) | `android` (AudioManager, read-only) |
| `Power` | ✓ | ✓ | `sysfs`, `mcu` | `android` (BatteryManager, read-only) |
| `Wifi` | ✓ | ✓ | `iwd`, `networkmanager` | `android` (status only, §7.2) |
| `Bluetooth` | ✓ | ✓ | `bluez` | `none` (the phone's own settings) |
| `Haptics` | ✓ | ✓ (window border) | `mcu`, `gpio` (PWM motor) | `android` (Vibrator) |
| `Storage` | ✓ | ✓ | `linux` (statvfs + USB gadget) | `android` (StatFs, no mass storage) |

`none` means the part is not there. `sim` backends can be built on any OS, so a Linux
laptop can run the simulator with **real iwd Wi-Fi and sim everything else**. This is the
main payoff of choosing per part.

### 3.2 Build selection

One CMake cache variable per part. A backend that is not chosen is not compiled, so its
dependencies (libdbus, libpulse, the Android NDK) are not needed either:

```cmake
set(OAP_HAL_INPUT     "sdl"  CACHE STRING "List of: sdl evdev mcu android")
set(OAP_HAL_AUDIO     "sim"  CACHE STRING "none sim pulse alsa android")
set(OAP_HAL_POWER     "sim"  CACHE STRING "none sim sysfs mcu android")
set(OAP_HAL_WIFI      "sim"  CACHE STRING "none sim iwd networkmanager android")
set(OAP_HAL_BLUETOOTH "sim"  CACHE STRING "none sim bluez")
set(OAP_HAL_HAPTICS   "none" CACHE STRING "none sim mcu gpio android")
set(OAP_HAL_STORAGE   "sim"  CACHE STRING "none sim linux android")
```

Boards are **CMake presets** (`CMakePresets.json`) that set these variables, for example
`sim-macos`, `sim-linux`, `radxa-zero3w`, `android`. Any variable can still be overridden
on the command line (`--preset radxa-zero3w -DOAP_HAL_BLUETOOTH=none`).

Normally a part gets exactly one backend, and nothing more has to be configured. A part may
list several backends (`OAP_HAL_POWER="sysfs;mcu"`) only for firmware that serves several
board revisions. `hal.toml` then picks one at run time (§3.3).

Each backend is its own static library (`oaphal-iwd`, `oaphal-bluez`, ...) and has one
factory function:

```cpp
// app/hal/iwd/iwd_wifi.h
std::unique_ptr<Wifi> makeIwdWifi(HalContext& ctx);   // nullptr if iwd is not running
```

CMake generates `hal_registry.cpp` from the chosen variables. It includes the chosen
factories and has one function, `HalParts makeHal(HalContext&, const HalConfig&)`, which
the front end calls. No `#ifdef`s appear in backends or core. Tests do not link the
registry; they build `Services` from fakes directly.

### 3.3 `hal.toml`: one image, several board revisions

`hal.toml` is optional. Without it, every part uses the backend that was built for it.
The file is for the case where one firmware image runs on several hardware revisions:

```toml
# /etc/oap/hal.toml — read by the front end, never by core

[defaults]                      # applies to all boards below
input = ["evdev"]
wifi = "iwd"

[[board]]
match = "radxa,zero3w-rev1"     # against /proc/device-tree/compatible
power = "sysfs"                 # rev 1: fuel gauge on I2C, no MCU
haptics = "none"
[board.buttons]                 # gpio-keys device
device = "gpio-keys"
keys = { KEY_PLAYPAUSE = "PlayPause", KEY_PREVIOUSSONG = "Back", KEY_NEXTSONG = "Forward" }

[[board]]
match = "radxa,zero3w-rev2"
power = "mcu"                   # rev 2: MCU over USB
haptics = "mcu"
mcu = { usb = "cafe:4f41" }
[board.buttons]                 # the MCU's HID interface
usb = "cafe:4f41"
keys = { KEY_F13 = "BackDeep", KEY_F14 = "ForwardDeep" }
hold = "SW_MUTE_DEVICE"         # a switch, not a key (§5.1)
```

- The first `[[board]]` whose `match` fits wins. If none fits, `[defaults]` and the built
  backends apply.
- A name that was not compiled in is an error at startup, and the device speaks it ("Hardware
  config error: power backend mcu is not built in"). It never falls back silently.
- The button device table and key map (§5.1) live here too, since they differ between
  revisions. On a single-revision build they come from the preset's built-in default
  `hal.toml`.
- Parsing uses a small header-only TOML library (toml++) in the HAL layer. User settings
  such as the keyboard layout stay in the normal settings, not here.
- The simulator reads `hal.toml` too (`oap-sim --hal <file>`), so a part can be set to
  `"none"` there to test the menus and Status sentence without it, with no rebuild.

### 3.4 Shared connections

Several backends often share one resource: `iwd` and `bluez` share a D-Bus connection,
and the `mcu` power and haptics backends share one serial link. These resources live in
`HalContext`, are created lazily by the first backend that asks, and are used only by
backends:

```cpp
struct HalContext {
    HalEvents& events;
    Clock& clock;
    EventLoop& loop;          // the HAL thread's poll() loop: backends add fds and timers
    DBusConnection& dbus();   // created on first use (Linux)
    McuLink& mcu();           // created on first use; one protocol, many backends
    JavaVM* jvm = nullptr;    // Android only
};
```

On Linux all backends run on **one HAL thread** (`EventLoop` around `poll()`). On Android
the Java side calls back on its own threads, and the JNI glue posts into `HalEvents`.

Backends call `clock.now()` from the HAL thread while `App` calls it from the UI thread, so
`Clock::now()` must be thread-safe. `SystemClock` already is. `FakeClock` keeps its time in a
`std::atomic`.

### 3.5 Same mechanism for services.h

`Speech`, `MediaPlayer` and `AudioOutput` are chosen the same way (`OAP_TTS=espeak|android`,
`OAP_AUDIO_OUT=coreaudio|pulse|alsa|aaudio`). This replaces today's `if(APPLE)` in
`CMakeLists.txt`. `app/platform/audio_coreaudio.cpp` moves to `app/hal/coreaudio/`.

## 4. Interfaces (sketch)

```cpp
// app/core/hal.h — all calls from the UI thread, all non-blocking.

enum class Output { Speaker, Headphones, Bluetooth, Usb };

class AudioRouting {                 // "audio controls"
public:
    struct Caps { bool select; };                // Android: false, the system routes
    virtual Caps caps() const = 0;
    virtual std::vector<Output> available() const = 0;
    virtual Output current() const = 0;
    virtual void select(std::optional<Output> output) = 0; // nullopt = automatic (newest wins)
};

struct BatteryState { int percent; bool charging; bool charger; bool full; };

class Power {
public:
    struct Caps { bool suspend, powerOff, reboot, wakeAlarm; };
    virtual Caps caps() const = 0;               // Android: suspend only (§7.2)
    virtual BatteryState battery() const = 0;
    virtual void requestSuspend() = 0;          // front end suspends after tick() returns, see §8
    virtual void requestPowerOff() = 0;
    virtual void requestReboot() = 0;
    virtual void setWakeAlarm(std::optional<std::time_t> at) = 0; // RTC: podcast downloads
};

enum class WifiState { Off, Disconnected, Connecting, Connected };
struct WifiNetwork { std::string ssid; int signal; bool secured; bool known; };

class Wifi {
public:
    struct Caps { bool toggle, scan, connect, forget; };   // Android: all false
    virtual Caps caps() const = 0;
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
    struct Caps { bool massStorage; };
    virtual Caps caps() const = 0;
    virtual StorageState state() const = 0;
    virtual void setMassStorage(bool on) = 0;   // export the data partition over USB
};
```

Input has no interface to call. Input backends only post events (§5).

Events, drained in `tick()`:

```cpp
// Input (§5). `at` is when the key changed, on the Clock::now() time base, so queue
// delay does not stretch a short press into a long one.
struct ButtonInput       { Button button; bool down; Clock::TimePoint at; };      // device keys, already mapped
struct KeyInput          { Key key; bool down; Modifiers mods; Clock::TimePoint at; }; // a keyboard
struct TextInput         { std::string utf8; };                                   // layout applied, may be several chars
struct TouchInput        { Touch gesture; };                                      // a finished gesture (§5.4)
struct HoldChanged       { bool on; };
struct KeyboardChanged   { bool connected; std::string name; };

struct OutputChanged     { Output from, to; bool lost; };   // lost = jack pulled / BT dropped
struct BatteryChanged    { BatteryState state; };           // backend posts on 1 % steps and charger edges
struct WifiChanged       { WifiState state; std::string ssid; std::string error; };
struct WifiScanDone      { std::vector<WifiNetwork> networks; };
struct BtDeviceFound     { BtDevice device; };
struct BtPairing         { std::string address; bool ok; std::string pin; std::string error; };
struct BtConnection      { BtDevice device; };
struct UsbHostChanged    { bool connected; };
struct HalError          { std::string message; };          // e.g. bad hal.toml, MCU not answering
using HalEvent = std::variant<ButtonInput, KeyInput, TextInput, TouchInput, HoldChanged,
                              KeyboardChanged, OutputChanged, BatteryChanged, WifiChanged,
                              WifiScanDone, BtDeviceFound, BtPairing, BtConnection,
                              UsbHostChanged, HalError>;

class HalEvents {            // thread-safe; backends post, App drains
public:
    explicit HalEvents(std::function<void()> wake);   // called after each post()
    void post(HalEvent e);
    std::vector<HalEvent> drain();
};
```

`HalEvents` stays plain C++. The front end passes the `wake` callback, which writes an
eventfd, pushes an SDL user event or posts to an Android `Handler`. Tests pass an empty one.

Grouping. `App`'s constructor already takes seven arguments. Replace them with one struct:

```cpp
struct Services {
    Speech& speech; MediaPlayer& player; SoundEffects& sounds; VolumeControl& volume; Clock& clock;
    HalEvents& events;
    AudioRouting* audio = nullptr; Power* power = nullptr; Wifi* wifi = nullptr;
    Bluetooth* bluetooth = nullptr; Haptics* haptics = nullptr; Storage* storage = nullptr;
};
```

`keyDown`/`keyUp`/`setHold` stay public, so tests and scripted sim steps can still call them
directly. `ButtonInput` events call the same functions. `keyDown`/`keyUp` (and
`GestureDetector::press`/`release`) gain an optional time argument. `ButtonInput` passes `at`,
so a down and up drained in the same `tick()` keep their real duration. Without the
argument they use `clock_.now()` as today.

## 5. Input

### 5.1 Sources

Several input backends can be active at once, and their events are merged in the queue:

| Source | Backend | Posts |
|---|---|---|
| Device buttons on GPIO (`gpio-keys`, dev boards) | `evdev` | `ButtonInput`, `HoldChanged` |
| Device buttons on our microcontroller | `evdev` (its USB HID interface) | `ButtonInput`, `HoldChanged` |
| USB or Bluetooth keyboard | `evdev` / `android` / `sdl` | `KeyInput`, `TextInput`, `KeyboardChanged` |
| Bluetooth headset buttons (AVRCP) | `evdev` / `android` (MediaSession) | `KeyInput` (media keys) |
| Phone touch screen | `android` | `TouchInput` (§5.4) |

The phone's volume keys are not input. Android changes the media volume itself, also with the
screen off.

The `evdev` backend sorts devices by the device table in `hal.toml` (§3.3). Matching devices
are device buttons and use its keycode → `Button` map. Every other device with letter keys
is a keyboard (`KeyInput` + `TextInput`), and every other device with media keys
(`KEY_PLAYPAUSE`, `KEY_NEXTSONG`, ...), such as a Bluetooth headset, posts `KeyInput`
only. Autorepeat events (`value == 2`) are dropped for device buttons. It watches udev for hotplug.

**Hold is a switch, not a key.** The `hold` entry maps an `EV_SW` code (or a key held
down) to `HoldChanged`, never to a `Button`. The backend reads its state at startup
(`EVIOCGSW` / `EVIOCGKEY`) and posts `HoldChanged` right away, so a device that boots with
Hold on is locked from the first moment.

**Our microcontroller is one USB composite device with several functions:**

- **HID** for the buttons. They need no driver code on Linux or Android, and they even work
  on a desktop. Two-stage keys use otherwise unused HID codes (F13–F24), which the key map
  turns into `BackDeep` and `ForwardDeep`. The Hold switch is reported as a HID switch,
  and the MCU sends its current state when it enumerates.
- **CDC-ACM serial** with our own framed protocol (`McuLink`) for battery and charger,
  power off, the RTC wake alarm, haptics and the LED.

The `mcu` input backend also reads buttons from the serial protocol. It is for boards whose
MCU sits on a UART instead of USB.

### 5.2 Keyboard as buttons

When no text field is open, `App` maps `KeyInput` to buttons with one table in core,
`defaultKeyMap()`. The simulator's key bindings (architecture §10: Space = Play/Pause,
arrows, Enter = OK, ...) move there, so every front end and the tests use the same map. Media
keys from headsets map to Play/Pause, Back and Forward. Keys that are not mapped are ignored
silently.

### 5.3 Keyboard as text entry

A new screen type, **text field**, is opened by setup steps that need text: Wi-Fi
passphrase, server URL, device name, and later search.

- `TextInput` inserts characters. Backspace, Delete, Left/Right, Home/End edit. Enter
  confirms, Escape cancels. Device buttons keep working: OK confirms, Home cancels, Left/Right
  move by character, Play long reads the whole field back.
- **Echo**: each typed character is spoken, as screen readers do ("a", "capital B", "7",
  "at"). Backspace says the deleted character. Password fields echo characters too for now.
  A setting to hide them is deferred (§10).
- Layout is the backend's job. `TextInput` already carries characters. On Linux the
  `evdev` backend runs keycodes through **libxkbcommon** with the layout from settings.
  The setup asks for the keyboard layout once, after the language.
- When there is no keyboard, the text field says "Connect a keyboard, or use spelling
  mode" (spelling mode arrives with STT, M3). Until then, Wi-Fi also has the keyboard-free
  paths from SPEC §6.3: `config.toml` copied over USB file transfer, and AP setup. When a keyboard is connected, the device says
  "Keyboard connected" (`KeyboardChanged`). If a text field is waiting, it adds "Type now".

This makes a keyboard the **first way to enter a Wi-Fi passphrase**, so H4 no longer waits for M3.
On Android the phone's own keyboard (with TalkBack) is used instead, through a native text field.

### 5.4 Touch gestures (Android)

The whole screen is one gesture pad. There is nothing to find by touch. The `android`
backend recognizes finished gestures and posts `TouchInput`. Core maps them with
`defaultTouchMap()` to the same `{Button, Gesture}` events that buttons produce, so menus,
the player and all their tests stay the same. Gestures skip `GestureDetector`, because a
swipe is already complete.

Draft map, to be tried with blind users:

| Gesture | Acts as |
|---|---|
| Swipe left / right | Back / Forward (short) |
| Two-finger swipe left / right | Back / Forward deep (chapter) |
| Swipe up / down | Up / Down |
| Double tap | OK |
| Two-finger double tap | Play/Pause (the screen readers' "magic tap") |
| Touch and hold | OK long (back one level) |
| Two-finger swipe down | Home |
| Three-finger tap | Play long (where am I) |

**TalkBack conflict**: when TalkBack is on, it takes these gestures for itself, and the app only
sees TalkBack's actions. Options: tell the user to pause TalkBack while in the player
(TalkBack has a shortcut for this), or handle TalkBack's actions (focus moves, activate,
scroll) as input too. This needs a prototype (§10).

## 6. What App does with it

| Event / need | App behaviour |
|---|---|
| `ButtonInput` / `HoldChanged` | Same as `keyDown`/`keyUp`/`setHold` today. |
| `KeyInput` | Text field open: edit keys (§5.3). Otherwise: `defaultKeyMap()` → button. |
| `TextInput` | Text field open: insert and echo. Otherwise: ignore. |
| `TouchInput` | `defaultTouchMap()` → button gesture (§5.4). Unknown gestures play the Edge earcon. |
| `KeyboardChanged` | "Keyboard connected." / "Keyboard disconnected." Low priority. |
| `HalError` | Spoken at startup and in Status, so the user can report it. |
| `OutputChanged{lost=true}` | Pause, save the position, say "Headphones unplugged, paused." or "Bluetooth disconnected, paused." (SPEC §3.4). Never switch to the speaker while playing. |
| `OutputChanged` (new output) | Restore that output's own volume (`volume.<output>`). Short announcement. |
| `BatteryChanged` | Say it at 20 %, 10 % and 5 % (once each, re-armed after charging). At 3 %, save and power off if `caps().powerOff`, else say "Battery critical". Say when charging starts and when it is full. |
| `WifiChanged` | Speak only if the user is in a Wi-Fi menu or started the connect. Errors are always spoken. |
| `WifiScanDone` / `BtDeviceFound` | Refresh the open list in place and keep the selection. Say "Found 3 networks." |
| `BtPairing{pin}` | Speak the PIN digit by digit, then "Confirm on the other device". |
| Status (System menu, Play long) | Built from `power->battery()`, `wifi->connected()` and `storage->state()`. A missing part is left out of the sentence. |

Speech priority. Events must not talk over the user. Low-priority messages (connected,
found) wait until speech is idle and are dropped when they go stale. Critical ones (battery
5 %, output lost) interrupt, like a key press does. Typing echo counts as a key press.

Menu changes this enables. Items appear only when the part exists and has the capability:

```
System
├── Status
├── Wi-Fi        → On/off · Networks → <scan results> → <passphrase field> · Saved networks → <ssid> → Forget
├── Bluetooth    → On/off · Pair new device → <found> · Paired devices → <dev> → Connect / Disconnect / Forget
├── Audio output → Automatic · Speaker · Headphones · <BT device>
├── USB file transfer  (mass storage on/off)
└── …
Settings
├── Maximum volume       (hearing protection)
├── Pause when headphones are unplugged
└── Keyboard layout
```

On the Android app, System keeps only Status, since Wi-Fi, Bluetooth, power and USB belong to the phone.

`MenuLevel` needs a `refresh` builder so that a list such as scan results can be rebuilt
while it is open (the `submenu` lambda is only called once today).

## 7. Backend notes

### 7.1 Linux and simulator

| Backend | Notes |
|---|---|
| `sim` | A **Hardware** ImGui panel (H2) shows and edits every sim part: jack and BT checkboxes, battery slider, charger, drain over time, fake networks with passwords and connect delays, fake BT devices with an optional PIN, "keyboard connected" toggle. A separate **Touch pad** panel (H1) turns mouse drags into gestures. Script steps `jack:out`, `battery:15`, `charger:on`, `type:hunter2`, `swipe:left` make walkthroughs repeatable. Sim backends use `Clock` for their delays, so the same classes can serve as rich test fakes. |
| `sdl` input | SDL key events → `KeyInput`, `SDL_TEXTINPUT` → `TextInput`, mouse drags on the touch pad → `TouchInput`. The simulator's own key handling goes away. |
| `evdev` input | `/dev/input/event*` + udev hotplug, `EVIOCSCLOCKID` set to `CLOCK_MONOTONIC` so event times match `Clock`. libxkbcommon for text. |
| `mcu` | One `McuLink` (USB CDC-ACM or UART, framed messages with a version handshake) shared by the power, haptics and (UART boards) input backends. |
| `pulse` | libpulse: sink and port events (PipeWire through pipewire-pulse handles jack detection and BT A2DP sinks). |
| `alsa` | `SW_HEADPHONE_INSERT` from evdev plus bluez-alsa. |
| `sysfs` power | `power_supply` (fuel gauge, charger) plus udev netlink for changes. Suspend through `/sys/power/state`. RTC through `/sys/class/rtc/rtc0/wakealarm`. |
| `iwd` | D-Bus `net.connman.iwd`: small, fast, no wpa_supplicant config files. |
| `networkmanager` | D-Bus `org.freedesktop.NetworkManager`, for desktop Linux and boards that already ship it. |
| `bluez` | D-Bus `org.bluez`, with our own Agent (NoInputNoOutput or DisplayOnly so the PIN can be spoken). Audio routing through PipeWire. |
| `linux` storage | `statvfs`, USB gadget (`g_mass_storage` or ConfigFS) with the data partition unmounted while exported. |

### 7.2 Android (Play Store app)

The same `app/core` runs in a normal Play Store app. It is a separate front end, and it takes real work beyond the HAL:

- **Process**: a Kotlin activity (full-screen gesture pad, screen may be dimmed) plus a
  foreground media service that hosts `App` through JNI and calls `tick()` from a `Handler`.
  A **MediaSession** gives headset buttons, the lock screen and Bluetooth car controls.
- **Speech and audio**: `OAP_TTS=android` uses the phone's TextToSpeech, so users get the
  voices they already know. Playback keeps FFmpeg (NDK build) for chapters and formats, and
  outputs through AAudio. `MediaPlayer` and `Speech` keep their interfaces.
- **Library**: the user picks a folder once through the Storage Access Framework. The
  library code needs a file abstraction instead of plain `std::filesystem` paths there.
- **What a normal app cannot do**: power off or reboot; toggle Wi-Fi or join a network
  (Android 10+); manage Bluetooth pairing in a reasonable way; USB mass storage. These parts
  report read-only `caps()`, and the menus leave them out. The phone's own settings,
  used with TalkBack, handle them.
- **Sleep**: `caps().suspend` is true. `requestSuspend()` releases the wake lock and turns the
  screen off, and the system decides the rest.

## 8. Suspend needs the front end

On Linux, writing `mem` to `/sys/power/state` blocks until resume. `App` must therefore
not suspend from inside `tick()`:

1. `App::sleep()` saves state, plays the sleep earcon and calls `power->requestSuspend()`.
2. Once speech has finished, the front end's main loop sees the request and suspends.
3. After resume, the front end calls `app.onResume()`, which says where the user is (as `wake()` does now).

`Mode::Sleeping` stays as it is in the simulator, where a key press is the "resume".

## 9. Phases

| Phase | Work | Fits milestone |
|---|---|---|
| H0 | `Services` struct, `HalEvents`, `hal.h`, `HalContext`, per-part CMake variables, generated registry, presets `sim-macos` and `sim-linux`. Sim and fake `Power`/`Storage` replace the hard-coded status strings. Tests for battery announcements. | M0 |
| H1 | Input through the HAL: `ButtonInput`/`KeyInput`/`TextInput`/`TouchInput`, timestamps on `keyDown`/`keyUp`, `defaultKeyMap()` and `defaultTouchMap()` in core, `sdl` input backend, sim Touch pad panel. **Text field screen with typing echo.** | M0 |
| H2 | `AudioRouting` (jack, pause on unplug, volume per output), sim Hardware panel | M0 / M2 |
| H3 | Linux `evdev` input (device buttons + keyboards, xkbcommon), `sysfs` `Power`, `pulse` `AudioRouting`, real suspend through the front end. First board preset. `hal.toml` with board matching. | M2, M5 |
| H4 | `Wifi`: sim, then iwd. Live menu refresh. Passphrase by keyboard (H1), `config.toml` over USB, or AP setup (SPEC §6.3); later by spelling mode (M3) | M4 |
| H5 | `Bluetooth`: sim, then BlueZ. BT as an audio output. | M4+ |
| H6 | Microcontroller: HID buttons, `McuLink` over CDC-ACM for power, RTC, haptics and LED. | M5 |
| A0 | Android spike: NDK build of core + FFmpeg, JNI host service, Android TTS, MediaSession, touch gestures. Answers the TalkBack question (§10). | after M2 |
| A1 | Android app: SAF library, read-only power/storage/audio parts, Play Store release. | later |

Each phase follows the usual rule: tests drive `App` with fakes, and every test is checked with a sabotage.

## 10. Decisions and open questions

Decided (2026-10-22):

- **Backends are chosen per part at build time.** `hal.toml` is used only when one image serves
  several board revisions. It picks among backends that were built in and holds the button key map.
- **Android target: a normal Play Store app** with the same core and touch gestures. It is a
  separate front end, scheduled after the device work reaches M2.
- **Microcontroller: one USB composite device with several functions**: HID for buttons and CDC-ACM serial for
  power, RTC, haptics and LED.

Open:

1. **D-Bus client library**: sd-bus without systemd (`basu`) plus `sdbus-c++`, or plain `libdbus-1`? GDBus pulls in GLib.
2. **PipeWire or PulseAudio** on the device. PipeWire with WirePlumber gives BT audio with
   little work but costs boot time and RAM. Bare ALSA would mean bluez-alsa. Measure in M2.
3. **Bluetooth in v1?** SPEC does not list it yet. The front-runner board (Radxa Zero 3W) has
   BT on its Wi-Fi module. A2DP adds about 150–250 ms of latency, which is noticeable for TTS feedback.
4. Should haptics be a decorator over `SoundEffects` (every earcon also buzzes) instead of a separate call?
5. **TalkBack and touch gestures** on Android: ask the user to pause TalkBack, or treat TalkBack's
   actions as input? Prototype in A0 and test with TalkBack users.
6. **Touch gesture map** (§5.4): draft only. Test with blind users.
7. **Password echo** *(deferred)*: characters are spoken for now. Later, add a setting to hide them,
   and ask blind testers which default they want.
