# Open Audio Player (working name)

An open-source audiobook and podcast player for blind users, operated with
buttons and speech, with no screen.

- [SPEC.md](SPEC.md): product spec (goals, hardware, audio UI, roadmap)
- [docs/architecture.md](docs/architecture.md): software architecture

## Simulator (macOS)

The first target is a desktop simulator. The keyboard stands in for the device
buttons, and a window shows the menu system, playback, the speech log and how
often each button is used.

### Dependencies

```sh
brew install cmake pkgconf ffmpeg espeak-ng sdl2
```

Dear ImGui is downloaded by CMake into the build folder.

### Build and run

```sh
cmake -S . -B build
cmake --build build -j
./tools/make_sample_library.sh library   # spoken test books
./build/oap-sim library
```

### Keys

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

Hold any key for a long press. The on-screen buttons can also be clicked or
held with the mouse.

### Scripted run

```sh
./build/oap-sim --config /tmp/test.tsv \
    --script "wait:1500 ok ok wait:3000 play:long right:long wait:1500 quit" library
```

Spoken text is also printed to stdout as `say: …`.
