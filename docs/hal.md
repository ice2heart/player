# Hardware Abstraction Layer (HAL) — Plan

> Status: **plan, not implemented.** Last updated: 2026-10-30.
> Context: [../SPEC.md](../SPEC.md) §3, §4.2, §6.4 · code as built: [architecture.md](architecture.md) · keys: [navigation.md](navigation.md).

## 1. Why

`App` uses real audio and existing front-end input adapters, but hardware status is simulated. `statusReport()` says
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
keys = { KEY_PLAYPAUSE = "PlayPause", KEY_PREVIOUSSONG = "Back", KEY_NEXTSONG = "Forward",
         KEY_VOLUMEUP = "VolumeUp", KEY_VOLUMEDOWN = "VolumeDown", KEY_MENU = "Options" }

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

Grouping. `App` already has a long constructor. Group its existing service dependencies
without dropping the server, library, scanner, settings, file or path arguments. The
following sketch shows the HAL additions, not the complete constructor contract:

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
| Game controller (DualSense, Xbox; dev and testing) | `evdev` / `sdl` | `ButtonInput` through `GamepadMapper` (built, see architecture §11) |
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
keys from headsets map to Play/Pause, Back and Forward, and their volume keys (`KEY_VOLUMEUP`,
`KEY_VOLUMEDOWN`) to Volume up / down. `KEY_MENU` is Options. The held volume keys repeat in
`GestureDetector`, not through the keyboard's own autorepeat, which the front end ignores. Keys that are not mapped are ignored
silently.

### 5.3 Keyboard as text entry

Text entry uses the `TextEntry` screen in [input-methods.md](input-methods.md),
including its validation, cancellation, UTF-8 limits and secret handling. Button entry
is the baseline; keyboard events feed the same draft. For non-secret fields, inserted
characters may be spoken. For secret fields, insertion says "Character added" and
readback says only "Hidden, N characters." Secrets never enter speech, logs or snapshots.

Backspace, Delete, Left/Right, Home/End, Enter and Escape edit or confirm/cancel through
the same screen; the keyboard adapter must not change the button editor's controls.
Linux applies the layout with libxkbcommon; keyboard layout is a user setting. A keyboard
connection says "Keyboard connected." and disconnection says "Keyboard disconnected."
The existing button editor works without a keyboard. Android's native keyboard must
feed the same validation and secret rules.

Wi-Fi setup in phase 5 depends on the button editor and keyboard adapter, not on speech
recognition (M3). USB configuration import and AP setup need their own transport and
credential-lifecycle designs; SPEC §6.3 is not an implementation of those paths.

### 5.4 Touch gestures (Android)

The whole screen is one gesture pad. There is nothing to find by touch. The `android`
backend recognizes finished gestures and posts `TouchInput`. Core maps them with
`defaultTouchMap()` to the same `{Button, Gesture}` events that buttons produce, so menus,
the player and all their tests stay the same. Gestures skip `GestureDetector`, because a
swipe is already complete. The built button-remap contract now includes ongoing
seek depth and release/cancel events. A completed swipe cannot begin an unbounded
seek: before implementing player touch seeking, decide whether touch maps to
bounded seeks or emits a start/change/release lifecycle. Tests must prove release
and cancellation stop seeking. Raw button adapters retain that lifecycle today;
SDL and evdev share `NavigationKeys` for both Shift keys and late depth changes.

Draft map, to be tried with blind users:

| Gesture | Acts as |
|---|---|
| Swipe left / right | Back / Forward (short) |
| Two-finger swipe left / right | Menu ten-item jump; player seek gesture requires a lifecycle decision |
| Swipe up / down | Unassigned; Up/Down are inactive |
| Double tap | OK |
| Two-finger double tap | Play/Pause (the screen readers' "magic tap") |
| Touch and hold | OK long (Options) |
| Two-finger swipe down | Home |
| Three-finger tap | Play long (where am I) |

**TalkBack conflict**: when TalkBack is on, it takes these gestures for itself, and the app only
sees TalkBack's actions. Options: tell the user to pause TalkBack while in the player
(TalkBack has a shortcut for this), or handle TalkBack's actions (focus moves, activate,
scroll) as input too. This needs a prototype (step 8.3).

## 6. What App does with it

| Event / need | App behaviour |
|---|---|
| `ButtonInput` / `HoldChanged` | Same as `keyDown`/`keyUp`/`setHold` today. |
| `KeyInput` | Text field open: edit keys (§5.3). Otherwise: `defaultKeyMap()` → button. |
| `TextInput` | TextEntry open: insert with the field’s feedback and secret rules. Otherwise: ignore. |
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

On Android, System omits unsupported hardware actions; Status and other existing non-hardware items remain.

`MenuLevel` needs a `refresh` builder so that a list such as scan results can be rebuilt
while it is open (the `submenu` lambda is only called once today).

## 7. Backend notes

### 7.1 Linux and simulator

| Backend | Notes |
|---|---|
| `sim` | A **Hardware** ImGui panel starts in phase 3 and gains controls with later parts: jack and BT checkboxes, battery slider, charger, drain over time, fake networks with passwords and connect delays, fake BT devices with an optional PIN, "keyboard connected" toggle. A separate **Touch pad** panel (phase 2) turns mouse drags into gestures. Script steps `jack:out`, `battery:15`, `charger:on`, `type:hunter2`, `swipe:left` make walkthroughs repeatable. Sim backends use `Clock` for their delays, so the same classes can serve as rich test fakes. |
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

## Phase 1: foundations and status (H0)

Goal: the simulator reports battery and storage from replaceable HAL services, while
existing playback and input still work. The Wi-Fi placeholder remains until phase 5.

| # | Step | Tests (each checked with a sabotage, as REPO.md requires) |
|---|------|------|
| 1.1 | **Navigation contract first.** Update [navigation.md](navigation.md) for the planned input, text entry, capability-dependent menus and output-loss behaviour; retain its as-built status and identify the phases that implement each change. Align the editor with [input-methods.md](input-methods.md). | Manual walkthrough of the proposed keys, menus and secret handling against both plans; record unresolved choices before code. |
| 1.2 | **Service grouping.** Add `Services` to `app/core/services.h`, preserving every existing App dependency and lifetime; migrate both front ends and test fixtures. Existing behaviour stays the same. | Existing App, front-end and core tests pass with the grouped services, including server playback and saved state. |
| 1.3 | **Event queue.** Add `app/core/hal.h` and `HalEvents` with thread-safe post, drain and wake; drain in `App::tick()`. Keep direct key calls working. Make the test clock safe across threads. | Posted events arrive in order on tick, wake is called, and concurrent posting loses no events; direct key tests still pass. |
| 1.4 | **Backend selection.** Add `HalContext`, per-part CMake selection, generated registry and `sim-macos`/`sim-linux` presets. Initially only compile available sim and none backends; retain current input and audio paths until their migration steps. Reject unknown selections. | Core-only and simulator configurations build; none omits the part; an unknown backend fails configuration; selected factories alone are linked. |
| 1.5 | **Power and storage status.** Add `Power` and `Storage` interfaces and sim/fake snapshots. Replace the hard-coded battery and filesystem-space status portions; omit missing parts and retain the existing Wi-Fi placeholder until step 5.3. | Status speaks the supplied battery percentage and free space; changing snapshots changes status; missing power or storage is omitted. |
| 1.6 | **Battery events.** Handle `BatteryChanged` thresholds and charger edges as in §6; save before requesting power off at 3% when supported. | Crossing 20%, 10% and 5% announces once, charging rearms them; 3% saves and requests power off when supported, otherwise says "Battery critical". |
| 1.7 | **Docs for phase 1 as built:** update architecture.md, navigation.md and SPEC. | – |

**Device check (manual):** run the simulator, open System → Status, change the simulated
battery and storage snapshots and hear the new values. Repeat with either part absent.
Exercise playback and existing keyboard controls.

## Phase 2: input events and text (H1)

Goal: simulator input travels through the HAL and keyboard text reaches the shared
accessible editor. This phase depends on [input-methods.md](input-methods.md) phase 1.

| # | Step | Tests (each checked with a sabotage, as REPO.md requires) |
|---|------|------|
| 2.1 | **Timestamped button events.** Add `ButtonInput` and `HoldChanged`; route them to the existing handlers with event timestamps, including `GestureDetector`. Preserve direct calls and `NavigationKeys` depth and release lifecycle. | A queued down/up retains its actual short or long duration; Hold locks input from startup; staged deep presses and releases still stop seeking. |
| 2.2 | **Keyboard navigation.** Add `KeyInput` and the core default key map; move SDL keyboard mapping to the HAL while preserving shared Shift/deep transitions, media keys and existing scripts. Ignore keyboard autorepeat for mapped buttons. | Simulator key sequences produce the existing button actions; both Shift keys and late depth changes work; unmapped keys are silent and held volume repeats once per gesture timer. |
| 2.3 | **Keyboard text adapter.** Add `TextInput` and `KeyboardChanged`, SDL text input and editing keys for the existing TextEntry screen. Use its request, validation and secret rules; do not add a second editor. | Keyboard insertion, cursor edits, confirm and cancel use the same draft as buttons; UTF-8 limits apply; secret text never appears in speech or logs; disconnect does not commit. |
| 2.4 | **Touch simulation.** Add completed `TouchInput` gestures, core mapping for the draft non-seeking gestures in §5.4 for simulator trials and an SDL Touch pad panel. Leave player touch seeking unavailable until its lifecycle is decided; preserve regular controls. | Mapped gestures perform their specified actions; unknown gestures play Edge; no completed gesture starts an unbounded seek. |
| 2.5 | **Docs for phase 2 as built:** update architecture.md, navigation.md and SPEC. | – |

**Device check (manual):** use existing simulator keys and a touch-pad gesture to
navigate, then enter and correct non-secret text in the shared editor. Enter a dummy
secret and verify that neither speech nor simulator output reveals it.

## Phase 3: simulated audio routing (H2)

Goal: simulated output changes pause playback safely and retain each output's volume.

| # | Step | Tests (each checked with a sabotage, as REPO.md requires) |
|---|------|------|
| 3.1 | **Simulated routing.** Add `AudioRouting`, `OutputChanged` and a simulator backend with cached available/current outputs. | Available and current outputs track simulated connect/disconnect; missing routing leaves existing playback usable. |
| 3.2 | **Output-loss policy.** On a lost headphone or Bluetooth output pause, save position and speak the corresponding sentence in §6; never resume automatically on speaker. | Pulling headphones while playing saves and says "Headphones unplugged, paused."; Bluetooth loss says its sentence; neither resumes on speaker. |
| 3.3 | **Output selection and volume.** Add the capability-dependent System → Audio output list and `volume.<output>` settings; automatic selects the newest output. Preserve existing volume until an output is known. | Selection invokes the requested output; changing outputs restores each saved volume; unavailable selection hides the menu; persistence survives restart. |
| 3.4 | **Hardware panel.** Add simulator battery, charger, jack and output controls plus `jack:out`, `battery:15` and `charger:on` scripts; leave Wi-Fi and Bluetooth controls for their phases. | Scripts and controls post the same changes; battery and unplug behaviour match steps 1.6 and 3.2. |
| 3.5 | **Docs for phase 3 as built:** update architecture.md, navigation.md and SPEC. | – |

**Device check (manual):** play audio, remove the simulated headphones and hear the
pause announcement. Reconnect, select outputs and verify their separate volume levels.

## Phase 4: Linux hardware (H3)

Goal: a selected Linux board can use real input, power and audio routing and suspend
without blocking App. The board, audio stack and board configuration must be decided first.

| # | Step | Tests (each checked with a sabotage, as REPO.md requires) |
|---|------|------|
| 4.1 | **Hardware configuration.** Parse `hal.toml` in `app/hal/` with board matching and compiled-backend validation (§3.3); wire `--hal` into the simulator. Keep built defaults without a file. | First matching board wins; defaults apply without a match; `none` disables a part; an unbuilt backend reports the specified error instead of falling back. |
| 4.2 | **Linux input.** Adapt existing `app/headless/evdev_input.*` into an evdev HAL backend: device maps, initial Hold state, hotplug and monotonic timestamps. Preserve gamepad and key release behaviour. | Injected evdev events map device keys and Hold, hotplug releases held keys, timestamps preserve gestures and deep release stops seeking. |
| 4.3 | **Linux keyboard text.** Add xkbcommon text translation and the keyboard layout setting, feeding step 2.3. | Two selected layouts produce their expected text; media keys remain controls; a disconnected keyboard cannot commit text. |
| 4.4 | **Sysfs power.** Add the `sysfs` Power backend with cached battery/charger snapshots and change notifications. | Fixture power-supply changes reach status and battery announcements; absent hardware yields no power part; no getter performs I/O. |
| 4.5 | **Linux audio routing.** After the audio stack decision, implement the selected routing backend and the first board preset; keep sim and none selectable. | Fake backend events exercise connect, select and loss through App; the preset builds only selected dependencies and missing hardware omits routing. |
| 4.6 | **Suspend.** Implement §8: request during tick, suspend after speech on the front end, call `onResume()` after return. Simulator retains key-to-wake. | Sleep saves before suspend; the front end does not suspend during tick or before speech finishes; resume speaks location; sim still wakes on a key. |
| 4.7 | **Docs for phase 4 as built:** update architecture.md, navigation.md and SPEC. | – |

**Device check (manual):** on the selected board, boot with Hold on, unlock and use the
buttons and a keyboard; inspect battery status, unplug headphones during playback,
then sleep and wake. Confirm the saved position survives suspend.

## Phase 5: Wi-Fi (H4)

Goal: scan and join a network using the shared editor, first in simulation and then on
Linux with iwd. Choose the D-Bus library and specify the Wi-Fi menu contract first.

| # | Step | Tests (each checked with a sabotage, as REPO.md requires) |
|---|------|------|
| 5.1 | **Wi-Fi simulator.** Add `Wifi`, events and simulated networks with configurable credentials and delayed results using `Clock`; add simulator controls. | Scan and connect return asynchronously; success, wrong password and disabled Wi-Fi update cached state; no secret appears in output. |
| 5.2 | **Refreshable lists.** Add a refresh builder to `MenuLevel` for live network lists, keeping the selected item by identity when possible. | A scan result refresh keeps the selected network; a removed selection clamps safely; leaving the list ignores late results. |
| 5.3 | **Wi-Fi menus.** Replace the placeholder with capability-dependent status, toggle, scan, connect and forget actions; use TextEntry for secured networks. Record exact prompts, error handling and confirmation in navigation.md before implementation. | Scan results update in place; connecting uses accepted text; cancellation sends no request; forget requires confirmation; missing capabilities hide actions; errors are spoken without revealing credentials. |
| 5.4 | **iwd backend.** Implement `app/hal/iwd/` over the chosen D-Bus library and shared HAL event loop. | Mock D-Bus scan, connect, error and disconnect reach the same menu behaviour; unavailable iwd yields no part; getters use cached state. |
| 5.5 | **Docs for phase 5 as built:** update architecture.md, navigation.md, streaming.md and SPEC, including whether network-connect sync is implemented. | – |

**Device check (manual):** scan, enter a dummy test network password, connect, disconnect
and forget it; repeat with an incorrect password and confirm keys remain responsive.

## Phase 6: Bluetooth (H5)

Goal: pair and manage a Bluetooth audio device using spoken feedback. Decide whether
Bluetooth is in scope for the device and specify pairing prompts before implementation.

| # | Step | Tests (each checked with a sabotage, as REPO.md requires) |
|---|------|------|
| 6.1 | **Bluetooth simulator.** Add `Bluetooth` and its events; simulate discovery, pairing including PIN, connection and loss, with Hardware panel controls. | Discovery and pairing complete asynchronously; PIN and connection events carry the simulated device; disconnect updates routing. |
| 6.2 | **Bluetooth menus.** Add capability-dependent discovery, paired devices, connect, disconnect and confirmed forget; refresh lists by device identity. Record exact speech in navigation.md first. | Discovery updates the open list without losing selection; PIN is spoken digit by digit; cancel and forget behave as specified; lost audio pauses as in step 3.2. |
| 6.3 | **BlueZ backend.** Implement `app/hal/bluez/` and the D-Bus Agent using the library selected in phase 5. | Mock BlueZ discovery, PIN, connection, failure and removal reach App; an absent service leaves the part out. |
| 6.4 | **Docs for phase 6 as built:** update architecture.md, navigation.md and SPEC. | – |

**Device check (manual):** discover, pair and connect a test headset, hear any PIN,
play audio, disconnect it and verify playback pauses. Forget it and check the list.

## Phase 7: microcontroller (H6)

Goal: the device uses the selected MCU for its buttons, battery, wake alarm and haptics.
The board, protocol, LED behaviour and haptic policy need decisions first.

| # | Step | Tests (each checked with a sabotage, as REPO.md requires) |
|---|------|------|
| 7.1 | **MCU link.** Add shared `McuLink` with the agreed framed protocol, version handshake and reconnect/error handling. | Simulated serial frames survive partial reads; bad version and disconnect report errors; reconnect restores communication without blocking App. |
| 7.2 | **MCU power.** Add the MCU Power backend for battery, charger, power off and wake alarm. | Simulated frames update status; power-off and alarm requests send the specified commands; missing MCU omits the part. |
| 7.3 | **MCU input.** Support the agreed HID mappings and UART serial button backend, including initial Hold and two-stage release. | Simulated button and Hold events match step 2.1; disconnect releases held buttons and stops seek. |
| 7.4 | **Haptics and LED.** Add MCU haptics and the agreed earcon/LED policy. | Simulated commands match the chosen patterns; unavailable haptics leaves audio feedback intact. |
| 7.5 | **Docs for phase 7 as built:** update architecture.md, navigation.md and SPEC, including the protocol and board wiring. | – |

**Device check (manual):** exercise each button and Hold, change charger state, trigger
an earcon with haptics, set a test wake alarm and verify power-off and wake on the board.

## Phase 8: Android spike (A0)

Goal: establish whether the core, audio, MediaSession and accessible gestures work on
Android. This is a prototype after M2; it does not promise a Play Store release.

| # | Step | Tests (each checked with a sabotage, as REPO.md requires) |
|---|------|------|
| 8.1 | **Android build and host.** Build core and FFmpeg with the NDK; host App in a Kotlin foreground media service through JNI and tick on its handler. | A test activity starts the service, plays a local test file and receives asynchronous events without blocking the UI. |
| 8.2 | **Speech and media controls.** Add Android TTS, AAudio output and MediaSession key events. | Spoken feedback completes and media controls play/pause through App; service recreation releases resources. |
| 8.3 | **Touch and TalkBack trial.** Prototype the draft gestures with TalkBack on and off; document observed conflicts and choose the production contract before implementing touch seeking. | Manual trial with TalkBack users records actions received, conflicts and the selected approach; automated gesture tests cover the chosen mapping and seek cancellation. |
| 8.4 | **Docs for phase 8 as built:** update architecture.md, navigation.md and SPEC; record trial results and remaining gaps. | – |

**Device check (manual):** play a test book, use headset controls and try the prototype
gestures with TalkBack both enabled and disabled; record what a TalkBack user can operate.

## Phase 9: Android app (A1)

Goal: a usable Android app with a selected library and phone-provided hardware status.
SAF file access, lifecycle, text input and release details must be designed after phase 8.

| # | Step | Tests (each checked with a sabotage, as REPO.md requires) |
|---|------|------|
| 9.1 | **SAF library.** Add the agreed file-access abstraction and persist the user's folder grant. | Selected documents are indexed and played after restart; revoked access is reported without corrupting the library. |
| 9.2 | **Android HAL.** Add read-only power, storage, Wi-Fi and audio routing plus Android haptics and suspend as specified in §7.2. | Status reflects supplied snapshots; unsupported actions are absent; sleep releases the wake lock and resume restores location. |
| 9.3 | **Android text and lifecycle.** Connect the native keyboard to the shared editor and validation; handle foreground service and activity recreation. | Cancel does not commit; secret content stays out of speech/logs; playback and state survive activity recreation. |
| 9.4 | **Docs for phase 9 as built:** update architecture.md, navigation.md and SPEC and record device and accessibility checks. | – |

**Device check (manual):** pick a folder, play and resume a book across activity
recreation, enter dummy text, inspect Status and use the selected TalkBack interaction.
Play Store release follows only after the release requirements are specified and met.

## Decisions

- Backends are chosen per part at build time; `hal.toml` selects among compiled
  backends for board revisions and holds their button maps.
- Android is a normal Play Store app using the same core, after M2.
- The MCU is a USB composite device: HID buttons and CDC-ACM serial for power,
  RTC, haptics and LED. UART input is supported for boards that need it.
- Text entry follows [input-methods.md](input-methods.md): button entry is the baseline,
  all input uses the same editor, and secrets are never spoken or logged.

## Open questions

The affected step stops until its choice is recorded here and the design and navigation
contract agree. Prototype and manual trial steps may investigate a question without
choosing a production implementation.

1. Before step 4.5: which board and audio stack (PipeWire/PulseAudio or ALSA)? Measure
   boot time and RAM on the board.
2. Before step 5.3: exact Wi-Fi prompts, error handling and forget confirmation; before
   step 5.4: D-Bus client library (basu plus sdbus-c++ or libdbus-1).
3. Before phase 6: whether Bluetooth is in v1, its menu prompts and pairing policy.
4. Before phase 7: MCU protocol framing, commands, timeouts, board wiring, LED behaviour
   and whether haptics decorates `SoundEffects` or is called separately.
5. Before production Android touch input: TalkBack interaction, tested gesture map and
   a bounded seek or start/change/release lifecycle. Step 8.3 investigates these.
6. Before phase 9: SAF file abstraction, Android text input and service lifecycle details,
   and Play Store release requirements.
7. Maximum volume and the pause-on-unplug setting in §6 need values, defaults and safety
   policy before they can be scheduled. USB mass storage needs an ownership/unmount and
   failure-recovery design; AP and USB configuration import need separate provisioning
   designs. None is implemented by the phases above.
