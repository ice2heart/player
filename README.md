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

Debian / Ubuntu, everything (simulator, headless, tests):

```sh
sudo apt install build-essential cmake ninja-build pkgconf \
    libavformat-dev libavcodec-dev libswresample-dev libavutil-dev \
    libespeak-ng-dev espeak-ng libsdl2-dev libpulse-dev libasound2-dev
```

| Package | Needed for |
|---------|------------|
| `build-essential cmake ninja-build pkgconf` | the build (CMake 3.20+, a C++20 compiler) |
| `libavformat-dev libavcodec-dev libswresample-dev libavutil-dev` | FFmpeg decoding (`oap-sim`, `oap-headless`) |
| `libespeak-ng-dev espeak-ng` | speech (`oap-sim`, `oap-headless`); `espeak-ng` brings the voice data |
| `libsdl2-dev` | `oap-sim` only |
| `libpulse-dev` / `libasound2-dev` | the `pulse` / `alsa` outputs; drop one by leaving it out of `OAP_AUDIO_BACKENDS` |

Only the core and tests (`-DOAP_BUILD_SIM=OFF -DOAP_BUILD_HEADLESS=OFF`) need just
`build-essential cmake ninja-build`.

On the Raspberry Pi (Raspberry Pi OS 64-bit, trixie), a cross-built
`oap-headless` needs only the runtime libraries (the Docker `pi-out` build writes
this line to `DEPENDENCIES.txt`):

```sh
sudo apt install libasound2t64 libavcodec61 libavformat61 libavutil59 libc6 \
    libespeak-ng1 libgcc-s1 libpulse0 libstdc++6 libswresample5 espeak-ng-data
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

### Docker

[docker/Dockerfile](docker/Dockerfile) builds in a clean Debian and runs the tests,
so the build fails if a test fails. Run from the repository root:

```sh
docker build -f docker/Dockerfile --target native .   # Debian trixie, x86-64: sim, headless, tests
docker build -f docker/Dockerfile --target pi .       # Debian trixie, Pi Zero 2 W cross build, tests under qemu
```

To get the binaries, build the matching `-out` target into a folder. The `pi-out`
folder also gets `DEPENDENCIES.txt`, the runtime packages read from the binary's
linked libraries:

```sh
docker build -f docker/Dockerfile --target pi-out --output build-pi .
scp build-pi/oap-headless pi@raspberrypi.local:
```

The `pi` target builds `oap-headless` and `oap-tests` for arm64 tuned for the
Cortex-A53 (`-mcpu=cortex-a53`), on Debian trixie so the binaries match the glibc
and FFmpeg of Raspberry Pi OS (trixie). They do not run on the older bookworm release. It uses
[cmake/toolchains/pi-zero-2w.cmake](cmake/toolchains/pi-zero-2w.cmake), which also
works outside Docker on a Debian trixie host with these packages:

```sh
sudo dpkg --add-architecture arm64 && sudo apt update
sudo apt install crossbuild-essential-arm64 qemu-user-static cmake ninja-build pkgconf \
    libavformat-dev:arm64 libavcodec-dev:arm64 libswresample-dev:arm64 libavutil-dev:arm64 \
    libespeak-ng-dev:arm64 libpulse-dev:arm64 libasound2-dev:arm64
cmake -S . -B build-pi -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/pi-zero-2w.cmake -DOAP_BUILD_SIM=OFF
cmake --build build-pi && ctest --test-dir build-pi --output-on-failure
```

### Releases

Pushing a tag like `v0.1.0` runs [.github/workflows/release.yml](.github/workflows/release.yml):
it builds and tests on x86-64 and for the Pi (under qemu), and publishes a GitHub
release with `oap-headless` for the Pi Zero 2 W and `DEPENDENCIES.txt`, the
`apt install` line for its runtime packages on Raspberry Pi OS.

```sh
git tag v0.1.0 && git push origin v0.1.0
```

### Audio output

Both front ends take `--audio NAME`:

| Name | Output |
|------|--------|
| `auto` (default) | the first built-in device backend that works, in `OAP_AUDIO_BACKENDS` order |
| `pulse` | PulseAudio, or PipeWire through pipewire-pulse |
| `alsa`, `alsa:DEVICE` | ALSA PCM, `default` unless named (`alsa:plughw:1,0`) |
| `coreaudio` | macOS default output |
| `null` | no sound; audio is consumed in real time, so playback still advances (servers, CI) |

**DualSense audio.** Over USB the PS5 controller is also a sound card with 4 channels
(1–2 audio, 3–4 the haptic motors). Use `--audio alsa:plughw:CARD=Controller`: ALSA's
plug layer sends our stereo to channels 1–2 and leaves the motors silent. Check the
card name with `aplay -l`. With PipeWire or PulseAudio, make the controller the
default sink and use `--audio pulse`. Bluetooth carries no audio to the controller on Linux.

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

### Game controller

Both front ends take a game controller (PS5 DualSense, Xbox and others): SDL
game controllers in the simulator, evdev devices in `oap-headless`.

| Control | Device button |
|---------|---------------|
| D-pad | Up / Down / Back / Forward |
| Square (X on Xbox) | Play/Pause |
| Cross (A) | OK |
| Circle (B) | Home |
| L1 + D-pad ←/→ | deep press (previous / next chapter) |

Hold a control for a long press. To remap, edit `defaultGamepadLayout()` in
[app/frontend/gamepad.cpp](app/frontend/gamepad.cpp).

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
- A **game controller** is picked up by evdev too (see [Game controller](#game-controller)).
  A DualSense works over USB or Bluetooth.
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
