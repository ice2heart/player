# Software Architecture

> Status: describes the code as of v0.1 (simulator on macOS and Linux, headless Linux front end). Last updated: 2026-10-24.
> For product goals, hardware and the overall plan see [../SPEC.md](../SPEC.md).

## 1. Overview

The software is one C++20 program with three layers:

```
┌───────────────────────────────────────────────────────────────────────┐
│ Front end (per target)                                                │
│   app/sim/       desktop simulator: SDL2 window, Dear ImGui, keyboard │
│   app/headless/  device: evdev buttons, terminal keys, no screen      │
│   app/frontend/  shared: scripted presses, terminal keys, gamepad map │
├───────────────────────────────────────────────────────────────────────┤
│ Core  (app/core/, plain C++, no dependencies, library "oapcore")      │
│   App ── menus, player screen, power states, settings                 │
│    ├── GestureDetector   press/release → short / long press           │
│    ├── Library           scans folders into books                     │
│    ├── Settings          key/value file, also stores positions        │
│    └── services.h        Speech · MediaPlayer · SoundEffects ·        │
│                          VolumeControl · Clock   (interfaces)         │
│   Earcons                synthesized  → effects stream                │
│   Mixer                  content + speech + effects → PCM, ducking    │
├───────────────────────────────────────────────────────────────────────┤
│ Media  (app/media/, library "oapmedia")                               │
│   Player : MediaPlayer   FFmpeg decode → content stream               │
│   Tts    : Speech        eSpeak NG    → speech stream                 │
├───────────────────────────────────────────────────────────────────────┤
│ Platform  (app/platform/, library "oapaudio")                         │
│   AudioOutput            CoreAudio · PulseAudio · ALSA · null         │
└───────────────────────────────────────────────────────────────────────┘
```

Only `app/platform/`, `app/sim/` and `app/headless/` know which OS they run on.
The two front ends wire the same core, media and audio code together; they differ
only in where button presses come from and whether there is a window.

`App` has no audio code. It only sees the interfaces in `services.h`, and the
front end wires in the real implementations (`Tts`, `Player`, `Earcons`, `Mixer`,
`SystemClock`). The unit tests wire in fakes instead (section 12).

## 2. Source layout

| Path | Contents |
|------|----------|
| `CMakeLists.txt` | Builds `oapcore`, `oapfrontend`, `oapmedia`, `oapaudio` (backends from `OAP_AUDIO_BACKENDS`), `imgui` and `oap-sim` (`OAP_BUILD_SIM`), `oap-headless` (`OAP_BUILD_HEADLESS`, Linux), plus `oap-tests` (`OAP_BUILD_TESTS`). ImGui and GoogleTest are fetched at configure time. |
| `app/core/app.{h,cpp}` | `App`: the UI state machine and all menus. |
| `app/core/buttons.{h,cpp}` | `Button`, `Gesture`, `GestureDetector`. |
| `app/core/menu.h` | `MenuItem`, `MenuLevel` data model. |
| `app/core/services.h` | Interfaces `App` depends on: `Speech`, `MediaPlayer`, `SoundEffects`, `VolumeControl`, `Clock` (+ `SystemClock`). |
| `app/media/player.{h,cpp}` | `Player`: one audio file, decoded on its own thread (FFmpeg). |
| `app/media/tts.{h,cpp}` | `Tts`: eSpeak NG on a worker thread. |
| `app/core/earcons.{h,cpp}` | `Earcons`: UI sounds made from sine tones. |
| `app/core/mixer.{h,cpp}` | `Mixer`: the three streams mixed into the output. |
| `app/core/audio_stream.{h,cpp}` | `AudioStream`: thread-safe PCM FIFO with flush epochs. |
| `app/core/library.{h,cpp}` | `Book`, `scanLibrary()`. |
| `app/core/settings.{h,cpp}` | `Settings`: persistent key/value store. |
| `app/core/text.{h,cpp}` | Text for speech, such as `spokenDuration()`. |
| `app/platform/audio_output.{h,cpp}` | `AudioOutput` interface and `open(name)`, which picks a built-in backend at run time (`--audio`). |
| `app/platform/audio_thread.h` | Base for blocking-write backends: a thread renders 10 ms blocks and writes them. When a write fails it closes the device, keeps discarding the mix in real time (so speech and playback still finish) and reopens the device with a growing delay (250 ms up to 4 s). |
| `app/platform/audio_coreaudio.cpp` | macOS backend (AudioUnit default output). |
| `app/platform/audio_pulse.cpp` | PulseAudio simple API (also PipeWire via pipewire-pulse), about 50 ms buffer. |
| `app/platform/audio_alsa.cpp` | ALSA PCM (`default` or `alsa:DEVICE`), 50 ms latency. |
| `app/platform/audio_null.cpp` | No device: consumes audio in real time. |
| `app/frontend/presses.{h,cpp}` | `PressSequencer` (timed presses), `parseScript()`, `deepOf()`. |
| `app/frontend/terminal_keys.{h,cpp}` | `TerminalKeyDecoder`: terminal bytes → presses. Keeps an escape sequence split across reads; a lone Esc is Home only after `kEscTimeout` (200 ms) without more bytes, checked by `poll(now)`. |
| `app/frontend/held_buttons.{h,cpp}` | `HeldButtons`: counts the inputs holding each button, so App gets `keyUp` only when the last one lets go. |
| `app/frontend/gamepad.{h,cpp}` | `GamepadMapper`: game controller buttons → device buttons, with the layout in `defaultGamepadLayout()`. Used by the simulator (SDL) and headless (evdev). |
| `app/sim/main.cpp` | Simulator: window, key mapping, visualisation. |
| `app/headless/main.cpp` | Headless front end: poll loop, terminal raw mode, signals. |
| `app/headless/evdev_input.{h,cpp}` | `EvdevInput`: `/dev/input` devices → `keyDown`/`keyUp`/`setHold`, with hot-plug rescans. |
| `tests/` | GoogleTest unit tests for the core; `fakes.h` has the test doubles. |
| `tools/make_sample_library.sh` | Builds a spoken test library with espeak-ng and ffmpeg. |

## 3. Threads

| Thread | Owner | Does |
|--------|-------|------|
| **Main / UI** | front end | Input events, `App::tick()`, drawing (simulator). **All `App` calls happen here.** The headless loop `poll()`s input with a 10 ms timeout. |
| **Decoder** | `Player` | Reads, decodes and resamples into the content stream. Blocks while the stream is full. |
| **TTS worker** | `Tts` | Runs `espeak_Synth()` synchronously and resamples into the speech stream. |
| **Audio callback** | `AudioOutput` | Calls `Mixer::render()` to pull PCM for the device. CoreAudio calls back; Pulse, ALSA and null run their own writer thread. |

Rules:
- `App` is single-threaded. Front ends must not call it from other threads.
- Threads exchange data only through `AudioStream` (mutex-protected) and atomics.
- The audio callback only reads streams and atomics, and never blocks for long.
  It still takes the `AudioStream` mutex, which is acceptable for the simulator.
  A lock-free SPSC ring would be the upgrade if the device ever glitches.

## 4. Audio pipeline

```
 file ─► libavformat ─► libavcodec ─► libswresample ─┐
                         (Player thread)              ▼
                                               content stream ──┐ (ducked to 30 %
 text ─► eSpeak NG 22 kHz ─► libswresample ──► speech stream ───┤  while speech
                         (TTS thread)                           │  is buffered)
 Earcon ─► sine synthesis ────────────────────► effects stream ─┤
                         (UI thread)                            ▼
                                            Mixer::render() ─► volume² ─► clamp
                                                                │ (audio callback)
                                                                ▼
                                    AudioOutput (CoreAudio / Pulse / ALSA / null)
```

- **Format everywhere:** 48 kHz, stereo, interleaved float32 (`Mixer::kSampleRate`).
- **Streams:** content holds 2 s, speech 4 s, effects 1 s.
- **Pause** makes the mixer stop reading the content stream, so decoded audio
  stays buffered and resume is instant.
- **Flush epochs:** `AudioStream::flush()` empties the FIFO and increments an
  epoch. A producer passes the epoch it started with to every write, and a
  stale epoch makes the write fail. This makes seeking and cancelling speech
  instant and race-free: anything written after the flush belongs to the new epoch.
- **Seeking:** `Player::seek()` stores a request. The decoder thread seeks
  backwards to a keyframe, flushes the stream, and then drops decoded samples
  until it reaches the exact target time.
- **Position:** `end time of queued audio − buffered frames / 48000`, which is
  what the listener hears right now.
- **Volume:** stored as 0–100 %, applied as `(v/100)²` for a perceptual curve.

## 5. Speech (TTS)

- eSpeak NG in `AUDIO_OUTPUT_SYNCHRONOUS` mode with a 50 ms buffer, which gives
  fast time-to-first-audio.
- `say()` appends to a queue. `stop()` clears the queue and flushes the speech
  stream. The synth callback returns 1 when its epoch is stale, which aborts
  eSpeak mid-utterance.
- **`App::onButton()` calls `tts.stop()` first on every key press**, so speech is
  always interruptible.
- Rate is applied per utterance (`Settings → Speech rate`).
- Every spoken string goes to `App::speechLog()`, which the simulator displays.
  `Tts` also prints it to stdout (`say: …`).

## 6. Input: buttons and gestures

```
key / GPIO ─► App::keyDown(b) ─► GestureDetector.press   ─► Deep ──┐
          └─► App::keyUp(b)   ─► GestureDetector.release ─► Short ─┤
              App::tick()     ─► GestureDetector.poll    ─► Long ──┴► App::onButton()
```

- **Long press** fires while the button is still held, after `Long press time`
  (default 600 ms). The release that follows is ignored.
- **Deep press**: Left / Right may be two-stage keys. The second stage is a
  raw input (`BackDeep`, `ForwardDeep`). It fires a `Deep` gesture on `Back` /
  `Forward` as soon as it closes, and the first stage's release is then ignored.
- **Hold switch** (`App::setHold`) blocks every button except Power and says "Locked".
  A key that is already down when Hold goes on is ignored until it is let go:
  no long press while held, no short press on release.
- **Buttons:** `PlayPause, Back (left), Forward (right), Up, Down, Ok, Home, Talk, Power`,
  plus the raw `BackDeep, ForwardDeep`.
- `App` counts every gesture per button (`buttonUsage()`). The simulator shows
  the counts, so we can see which buttons are actually needed.

### Button behaviour

The rules behind this table, and the checklist for new menus, are in
[navigation.md](navigation.md).

| Button | Anywhere | Menu | Player screen |
|--------|----------|------|---------------|
| Play/Pause short | play / pause | | |
| Play/Pause long | "Where am I?" | menu path + item | book, chapter, time left |
| Up / Down short | | previous / next item (wraps) | volume ± 5 % |
| Up / Down long | | first / last item | volume ± 5 % |
| Left / Right short | | change the value; nothing on other items | skip back / forward |
| Left / Right long or deep | | same as short | previous / next chapter |
| OK short | | open / activate | player menu |
| OK long | | previous screen | previous screen |
| Home short | | previous screen | previous screen |
| Home long | | home menu | home menu |
| Talk | "not available yet" | | |
| Power short | sleep | | |
| Power long | power menu | | |
| *any key while sleeping* | wake and announce context | | |

In **key describer** mode (Help → Key describer), every key says what it
does and does nothing else. Holding Home leaves the mode. The same texts
(`kButtonHelp` in `app.cpp`) are read out by Help → Buttons.

## 7. App state machine

```
 start(): [Home]  or, with a last book, [Home, Player]

 screens_:  Home ─► Library ─► Player ─► Player menu ─► Chapters
            ◄────────── Home / hold OK pops one ─────────────
            hold Home: back to [Home]

 Power short / sleep timer / auto sleep ─► Sleeping ── any key ──► same screen
```

- `App` keeps a **stack of `Screen`s**. A screen is a menu (`title`, `items`,
  `index`, `build`) or a program (`Screen::Kind::Player`). OK pushes and Home
  pops. Home is always at the bottom. Popping back to a menu rebuilds it with
  `build`, keeping the selected label. There is at most one player screen:
  `enterPlayer()` returns to it if it is on the stack.
- `mode()` is derived from the stack: `Player` when the top screen is the
  player, `Menu` otherwise, and `Sleeping` while asleep. Playback continues
  in the background on any screen.
- **Sleeping** simulates suspend: playback is paused, and the position and any
  changed settings are saved. Any key wakes the device and says where you are.
- **Power off** saves state, says goodbye and sets `quitRequested()`. The front
  end exits once speech has finished.

### Menu model

```cpp
struct MenuItem {
    std::string label;                               // always spoken
    std::function<std::string()> value;              // spoken after label: "Volume, 70 percent"
    std::function<bool(int)> adjust;                 // Left / Right step the value in place
    std::function<std::vector<MenuItem>()> submenu;  // built when opened (fresh data)
    std::function<size_t()> selected;                // submenu opens on this entry
    std::function<void()> action;                    // leaf
};
```

Menus are built by `App::*Menu()` functions and created lazily, so a list such
as Library or Chapters always reflects the current state. `App::choice()`
builds a setting from a settings key and a list of `(value, spoken name)`
options. Left / Right step the value and apply it at once. The settings file
is written when the user leaves the item. OK opens the list of values on the
current one, which is spoken as "current". Choosing a value there saves it,
applies it and goes back. The item types are listed in [navigation.md](navigation.md).

The spoken form of an item is `label[, value][, i of n][. hint]`. The position
("i of n") is left out at Expert verbosity, and the hint ("Press OK to open")
is added only at Beginner.

### Current menu tree

```
Home
├── Now playing: <book or album>   (only when something is loaded)
├── Library → <books>              (OK plays/resumes)
├── Music
│   ├── Play all                   (every track, albums in order)
│   ├── <loose track>              (OK plays it)
│   └── <album> → <tracks>         (OK plays from that track to the end of the album)
├── Podcasts                       (placeholder)
├── Settings
│   ├── Speech rate      100–460 wpm
│   ├── Volume           0–100 %
│   ├── Verbosity        beginner / normal / expert
│   ├── Skip back        5–60 s
│   ├── Skip forward     5–120 s
│   ├── Rewind on resume off / 2 / 5 / 10 s   (after a pause > 10 s)
│   ├── Long press time  300–1500 ms
│   ├── Menu wrap around on / off
│   ├── Auto sleep       1–30 min / never
│   └── Sleep timer →    Off · 1 · 15 · 30 · 45 min · 1 hour · End of chapter/track (only when something is loaded; opens on the timer that is set)
├── System
│   ├── Status           battery (simulated), free space, Wi-Fi, time
│   ├── Time and date
│   ├── Wi-Fi →          Status · Scan for networks · Turn off   (simulated)
│   ├── Sync             (not configured)
│   ├── Software update
│   ├── About
│   ├── Factory reset →  Cancel · Erase all settings
│   └── Power →          Sleep · Power off · Restart · Cancel
└── Help
    ├── Getting started  spoken introduction to the buttons and menus
    ├── Buttons →        one item per button; OK describes it
    └── Key describer

Player menu (OK on player screen; music: Tracks / Album info / Go to first track)
├── Chapters → <chapters>          (opens on the current chapter)
├── Sleep timer → …
├── Book info
├── Go to start of book
├── Speed                          (placeholder)
└── Add bookmark                   (placeholder)

Power menu (hold Power): Sleep · Power off · Restart · Cancel
```

## 8. Library and books

- `scanLibrary(root)`: each **sub-folder** is one book, containing all audio
  files under it sorted by path. Each **audio file at the top level** is also a
  book (typically M4B).
- Chapters: a multi-file book has **one chapter per file**, with leading track
  numbers removed from the title ("01 - Storm" becomes "Storm"). A single-file
  book uses the **embedded chapters** (M4B, MP3 CHAP via FFmpeg). With neither,
  the whole book is one chapter.
- **Music** lives in its own folder (`App`'s `musicRoot`, simulator `--music DIR`,
  default `./music`) and is scanned the same way: a sub-folder is an album, a top-level
  file is a loose track. Music reuses the book playback code (`book_` holds the album or the
  "Play all" list, `music_` is set), but it says "track" instead of "chapter", ends with
  "End of music", and **saves no position and does not change `last_book`**. Playing music
  never makes you lose your place in a book.
- Previous chapter: if more than 3 s into the chapter, the first press goes to
  its start, like a CD player.
- At the end of a file the next file starts automatically. At the end of the
  book the device says "End of book" and goes back to the start, paused.
- A file that cannot be opened is reported ("Cannot open 02 - Calm.mp3") and
  skipped: the next file that opens plays instead. If no file from the chosen
  one on opens, the book or track is not loaded at all: whatever was loaded
  before stays loaded at the same place, and the user stays in the menu.

## 9. Persistence

`Settings` is a flat `key<TAB>value` file (simulator: `~/.config/oap-sim/settings.tsv`,
or `--config FILE`).

| Key | Meaning |
|-----|---------|
| `speech.rate`, `volume`, `verbosity`, `skip.back`, `skip.forward`, `rewind_on_resume`, `buttons.long_press_ms`, `menu.wrap`, `power.auto_sleep_min` | user settings |
| `last_book` | absolute path of the book to resume on start (never music) |
| `pos:<book path>` | `"<file index>:<seconds>"` resume position |

The position is saved on pause, sleep, power off, book change, and every 30 s
during playback. That interval is a trade-off against flash wear on the device.

## 10. Simulator (`app/sim/main.cpp`)

- **Window:** SDL2 plus Dear ImGui (v1.91.8, downloaded by CMake into `build/_deps`).
  - Left: the **menu stack as columns**. The selected item is highlighted, `>`
    marks a submenu and `<>` a value Left / Right can change. On the player screen it
    shows the player key help instead.
  - Right: clickable **device buttons** (press and hold work), the hold switch,
    **now playing** with a progress bar, a table of **button usage**, and the
    **speech log**.
- **Keyboard:** Space = Play/Pause, arrows = Back/Forward/Up/Down, Enter = OK,
  Esc/Backspace = Home, T = Talk, P = Power, H = hold switch. Holding a key gives a long press.
- **Game controller:** SDL game controllers (DualSense, Xbox, ...) go through `GamepadMapper`,
  the same map as `oap-headless`: D-pad = arrows, Square = Play/Pause, Cross = OK,
  Circle = Home, L1 + Left/Right = deep press. Pads can be plugged in while it runs.
- **Arguments:** `oap-sim [--config FILE] [--music DIR] [--audio NAME] [--script STEPS] [LIBRARY_DIR]`.
  `--audio` is `auto` (default), `coreaudio`, `pulse`, `alsa[:DEVICE]` or `null`, among those built in.
  `tools/make_sample_library.sh [LIBRARY_DIR] [MUSIC_DIR]` makes a spoken sample library and some tone "music".
- **Script mode:** `--script "ok down ok wait:3000 right:long play:long quit"`
  presses buttons automatically, for repeatable walkthroughs and future UI tests.
  Steps: `play left right up down ok home talk power`, with `:long` for a long
  press, plus `wait:<ms>` and `quit`. An unknown step or a `wait` without a number is printed and skipped.
- **Several inputs at once:** keyboard, pads, on-screen buttons and `--script` share one
  `HeldButtons`, so a button held from two of them is released only when both let go.

## 11. Headless front end (`app/headless/`) and the Linux device

- **Arguments:** `oap-headless [--config FILE] [--music DIR] [--audio NAME] [--input LIST]
  [--evdev-device PATH] [--script STEPS] [LIBRARY_DIR]`. Settings default to
  `~/.config/oap/settings.tsv`.
- **Inputs** (`--input evdev,terminal,none`; default `terminal` on a tty, else `evdev`):
  - `EvdevInput` opens every `/dev/input/event*` with a mapped key (or just `--evdev-device`),
    reads `input_event`s non-blocking, ignores autorepeat, and rescans every 3 s for hot-plugged
    devices. Shift+Left/Right on a keyboard presses the deep stage, as in the simulator.
    Game controllers (any device with `BTN_GAMEPAD`) go through `GamepadMapper`; the D-pad
    may arrive as `BTN_DPAD_*` keys or as the `ABS_HAT0X`/`ABS_HAT0Y` axes (DualSense).
    `SW_MUTE_DEVICE` is the hold switch; its state is read at startup.
  - The terminal is put in non-canonical, no-echo mode. Bytes go through `TerminalKeyDecoder`
    and the presses are played by a `PressSequencer` (80 ms short, 1600 ms long), since a
    terminal sends no releases. Alt / `l` prefix = long, Shift / `d` prefix = deep.
    `main.cpp` still calls the untimed `decode(bytes)`, which takes each read as complete;
    switching to `decode(bytes, now)` plus `poll(now)` every loop keeps sequences split over slow SSH.
  - `--script` uses a second `PressSequencer`, shared with the simulator.
- **Main loop:** `poll()` stdin and the evdev fds for up to 10 ms, feed input, `tick()`, exit on
  `quitRequested()` once speech finished (at most 10 s later, in case the audio output hangs), or on SIGINT/SIGTERM/SIGHUP. Exit always calls
  `App::shutdown()`, so the position is saved when systemd stops the service.

Still to do for the device:

| Piece | Work needed |
|-------|-------------|
| Input | Key map from `hal.toml` instead of the built-in table; MCU serial events. |
| Power | Replace simulated sleep with real suspend (`/sys/power/state` or a request to the MCU). |
| Status | Battery from the fuel gauge (sysfs `power_supply`), Wi-Fi from iwd. See the HAL plan in [hal.md](hal.md). |
| Paths | Library under `/data/library`, settings under `/data/config`. |

Nothing in `app/core/` uses macOS APIs.

## 12. Tests

```sh
ctest --test-dir build --output-on-failure    # or ./build/oap-tests
```

- `oap-tests` links only `oapcore`, `oapfrontend` and `oapaudio`, so it builds without FFmpeg, eSpeak NG,
  SDL2 or audio libraries (`cmake -DOAP_BUILD_SIM=OFF -DOAP_BUILD_HEADLESS=OFF`). Without a front end
  `oapaudio` holds only the null output; the device backends are added with the sim or headless build.
- `tests/fakes.h` provides `FakeSpeech` (records what was said), `FakePlayer`
  (position, end of file and chapters set by the test), `FakeSounds`, `FakeVolume`
  and `FakeClock` (time only moves when the test advances it).
- `test_app.cpp` drives `App` the way a user does, through `keyDown`/`keyUp`/`tick`
  on a temporary library folder, and checks what is spoken, which file plays and
  what is saved. Long presses, sleep timers and auto sleep are tested by advancing
  the fake clock.
- `test_terminal_keys.cpp` tests the headless terminal key decoding, including sequences split across reads.
- `test_presses.cpp` tests `parseScript` and `PressSequencer` timing; `test_held_buttons.cpp` tests `HeldButtons`.
- `test_gamepad.cpp` tests the gamepad map, including the order of light and deep stages.
- `test_audio_output.cpp` tests `AudioOutput::open()` with the null backend, and
  `ThreadedAudioOutput` with a fake device that is lost and comes back.
- The other files test `GestureDetector`, `AudioStream`, `Mixer`/`Earcons`,
  `Settings`, `scanLibrary` and `spokenDuration` directly.
- Every test has been checked with a sabotage: break the code it guards and
  watch it fail. See [CLAUDE.md](../CLAUDE.md).

## 13. Known gaps / next steps

- Speed control (libavfilter `atempo`, or Sonic), skip silence.
- Bookmarks, podcasts (RSS), sync (Audiobookshelf).
- STT (Talk button): push-to-talk capture plus sherpa-onnx, Vosk or whisper.cpp.
- Localisation of spoken strings; a choice of TTS voice.
- Hardware abstraction (battery, headphone jack, Wi-Fi, Bluetooth, suspend): planned in [hal.md](hal.md).
- Music: shuffle and repeat.
- The `AudioStream` mutex in the audio callback could become a lock-free ring.
