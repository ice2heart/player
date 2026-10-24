# Open Audio Player (working name)

An open-source audiobook and podcast player for blind users, operated with
buttons and speech, with no screen.

- [SPEC.md](SPEC.md): product spec (goals, hardware, audio UI, roadmap)
- [docs/architecture.md](docs/architecture.md): software architecture

The program has two front ends around the same core:

- **`oap-sim`**, the desktop simulator (macOS and Linux). The keyboard stands in for
  the device buttons, and a window shows the menu system, playback, the speech log
  and how often each button is used.
- **`oap-headless`**, the front end for the device (Linux). It has no window. Buttons come
  from evdev input devices (GPIO keys, the MCU, a USB keyboard), from the terminal
  (handy over SSH) or from a script. Everything spoken is printed to stdout.

### Dependencies

macOS (simulator only):

```sh
brew install cmake pkgconf ffmpeg espeak-ng sdl2
```

Debian / Ubuntu:

```sh
sudo apt install build-essential cmake ninja-build pkgconf \
    libavformat-dev libavcodec-dev libswresample-dev libavutil-dev \
    libespeak-ng-dev espeak-ng libsdl2-dev libpulse-dev libasound2-dev
```

Dear ImGui and GoogleTest are downloaded by CMake into the build folder.

### Build and run

```sh
cmake -S . -B build
cmake --build build -j
./tools/make_sample_library.sh library music   # spoken test books and tone "music"
./build/oap-sim library
./build/oap-headless library                    # Linux
```

Build switches:

| Option | Default | Meaning |
|--------|---------|---------|
| `OAP_BUILD_SIM` | `ON` | `oap-sim` (needs SDL2) |
| `OAP_BUILD_HEADLESS` | `ON` on Linux | `oap-headless` |
| `OAP_AUDIO_BACKENDS` | `coreaudio` (macOS), `pulse;alsa` (Linux) | audio outputs to build in |
| `OAP_BUILD_TESTS` | `ON` | `oap-tests` |

For the device, build only the headless front end, for example with ALSA only:
`cmake -S . -B build -DOAP_BUILD_SIM=OFF -DOAP_AUDIO_BACKENDS=alsa`.

### Audio output

Both front ends take `--audio NAME`:

| Name | Output |
|------|--------|
| `auto` (default) | the first built-in device backend that works, in `OAP_AUDIO_BACKENDS` order |
| `pulse` | PulseAudio, or PipeWire through pipewire-pulse |
| `alsa`, `alsa:DEVICE` | ALSA PCM, `default` unless named (`alsa:plughw:1,0`) |
| `coreaudio` | macOS default output |
| `null` | no sound; audio is consumed in real time, so playback still advances (servers, CI) |

### Tests

```sh
ctest --test-dir build --output-on-failure
```

The core tests need no audio libraries:
`cmake -S . -B build -DOAP_BUILD_SIM=OFF -DOAP_BUILD_HEADLESS=OFF` builds just the
core and `oap-tests`.

### Simulator keys

| Key | Device button |
|-----|---------------|
| Space | Play/Pause (hold: where am I) |
| ← / → | Back / Forward (hold: previous / next chapter) |
| ↑ / ↓ | Up / Down |
| Enter | OK |
| Esc / Backspace | Home |
| T | Talk |
| P | Power (hold: power menu) |
| H | Hold switch (toggle) |

Hold any key for a long press, and use Shift+←/→ for a deep press. The
on-screen buttons can also be clicked or held with the mouse.

### Headless

```sh
oap-headless [--config FILE] [--music DIR] [--audio NAME] [--input LIST]
             [--evdev-device PATH] [--script STEPS] [LIBRARY_DIR]
```

- Settings default to `~/.config/oap/settings.tsv` (or `$XDG_CONFIG_HOME/oap/`).
- `--input` is a comma-separated list of `evdev`, `terminal` and `none`. The default is
  `terminal` when stdin is a terminal, and `evdev` otherwise (for example under systemd).
  Use `--input evdev,terminal` to have both.
- **evdev** opens every `/dev/input/event*` device with a mapped key, and looks for new
  ones every 3 s. The user needs read access (`sudo usermod -aG input $USER`).
  The key map is in [app/headless/evdev_input.h](app/headless/evdev_input.h): media keys,
  `KEY_POWER`, `KEY_F13`/`KEY_F14` for the deep stages, switch `SW_MUTE_DEVICE` for hold,
  plus the same keyboard keys as the simulator. Real key releases mean long presses work as on the device.
- **terminal** keys: Space, arrows, Enter, Esc/Backspace, `t` Talk, `p` Power, `h` hold.
  A terminal sends no key releases, so **Alt+key** (or `l` then the key) gives a long press,
  and **Shift+←/→** (or `d` then the arrow) a deep press. Ctrl+C quits and saves the position.
- SIGTERM and SIGINT save the position and settings before exiting.

### Scripted run

Both front ends run scripts:

```sh
./build/oap-sim --config /tmp/test.tsv \
    --script "wait:1500 ok ok wait:3000 play:long right:long wait:1500 quit" library
./build/oap-headless --audio null --input none --config /tmp/test.tsv \
    --script "wait:1500 ok ok wait:3000 play:long right:long wait:1500 quit" library
```

Spoken text is also printed to stdout as `say: …`.
