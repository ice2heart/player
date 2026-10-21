# Software Architecture

> Status: describes the code as of v0.1 (macOS simulator). Last updated: 2026-10-21.
> For product goals, hardware and the overall plan see [../SPEC.md](../SPEC.md).

## 1. Overview

The software is one C++20 program with three layers:

```
┌───────────────────────────────────────────────────────────────────────┐
│ Front end (per target)                                                │
│   app/sim/       desktop simulator: SDL2 window, Dear ImGui, keyboard │
│   (later)        device: evdev buttons / power-MCU events, no screen  │
├───────────────────────────────────────────────────────────────────────┤
│ Core  (app/core/, platform independent, library "oapcore")            │
│   App ── menus, player screen, power states, settings                 │
│    ├── GestureDetector   press/release → short / long press           │
│    ├── Player            FFmpeg decode → content stream               │
│    ├── Tts               eSpeak NG    → speech stream                 │
│    ├── Earcons           synthesized  → effects stream                │
│    ├── Mixer             content + speech + effects → PCM, ducking    │
│    ├── Library           scans folders into books                     │
│    └── Settings          key/value file, also stores positions        │
├───────────────────────────────────────────────────────────────────────┤
│ Platform  (app/platform/, library "oapaudio")                         │
│   AudioOutput            CoreAudio (macOS) · PulseAudio (Linux, TODO) │
└───────────────────────────────────────────────────────────────────────┘
```

Only `app/platform/` and `app/sim/` know which OS they run on. Porting to the
device means writing a new `AudioOutput` backend and a new front end that feeds
button presses into `App`. The core stays the same.

## 2. Source layout

| Path | Contents |
|------|----------|
| `CMakeLists.txt` | Builds `oapcore`, `oapaudio`, `imgui` (fetched at configure time) and `oap-sim`. |
| `app/core/app.{h,cpp}` | `App`: the UI state machine and all menus. |
| `app/core/buttons.{h,cpp}` | `Button`, `Gesture`, `GestureDetector`. |
| `app/core/menu.h` | `MenuItem`, `MenuLevel` data model. |
| `app/core/player.{h,cpp}` | `Player`: one audio file, decoded on its own thread. |
| `app/core/tts.{h,cpp}` | `Tts`: eSpeak NG on a worker thread. |
| `app/core/earcons.{h,cpp}` | `Earcons`: UI sounds made from sine tones. |
| `app/core/mixer.{h,cpp}` | `Mixer`: the three streams mixed into the output. |
| `app/core/audio_stream.{h,cpp}` | `AudioStream`: thread-safe PCM FIFO with flush epochs. |
| `app/core/library.{h,cpp}` | `Book`, `scanLibrary()`. |
| `app/core/settings.{h,cpp}` | `Settings`: persistent key/value store. |
| `app/core/text.{h,cpp}` | Text for speech, such as `spokenDuration()`. |
| `app/platform/audio_output.h` | `AudioOutput` interface plus a `create()` factory. |
| `app/platform/audio_coreaudio.cpp` | macOS backend (AudioUnit default output). |
| `app/sim/main.cpp` | Simulator: window, key mapping, visualisation, script runner. |
| `tools/make_sample_library.sh` | Builds a spoken test library with espeak-ng and ffmpeg. |

## 3. Threads

| Thread | Owner | Does |
|--------|-------|------|
| **Main / UI** | front end | Input events, `App::tick()`, drawing (simulator). **All `App` calls happen here.** |
| **Decoder** | `Player` | Reads, decodes and resamples into the content stream. Blocks while the stream is full. |
| **TTS worker** | `Tts` | Runs `espeak_Synth()` synchronously and resamples into the speech stream. |
| **Audio callback** | `AudioOutput` | Calls `Mixer::render()` to pull PCM for the device. |

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
                                               AudioOutput (CoreAudio / Pulse)
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
- Every spoken string also goes to stdout (`say: …`) and to `App::speechLog()`,
  which the simulator displays.

## 6. Input: buttons and gestures

```
key / GPIO ─► App::keyDown(b) ─► GestureDetector.press
          └─► App::keyUp(b)   ─► GestureDetector.release ─► Short ─┐
              App::tick()     ─► GestureDetector.poll    ─► Long ──┴► App::onButton()
```

- **Long press** fires while the button is still held, after `Long press time`
  (default 600 ms). The release that follows is ignored.
- **Hold switch** (`App::setHold`) blocks every button except Power and says "Locked".
- **Buttons:** `PlayPause, Back (left), Forward (right), Up, Down, Ok, Home, Talk, Power`.
- `App` counts every gesture per button (`buttonUsage()`). The simulator shows
  the counts, so we can see which buttons are actually needed.

### Button behaviour

| Button | Anywhere | Menu | Player screen |
|--------|----------|------|---------------|
| Play/Pause short | play / pause | | |
| Play/Pause long | "Where am I?" | menu path + item | book, chapter, time left |
| Up / Down short | | previous / next item (wraps) | volume ± 5 % |
| Up / Down long | | first / last item | volume ± 5 % |
| Left / Right short | | adjust value, or Left = back, Right = open | skip back / forward |
| Left / Right long | | same as short | previous / next chapter |
| OK short | | open / activate | player menu |
| OK long | | back one level | player menu |
| Home short | | back one level (from top → player) | home menu |
| Home long | | home menu | home menu |
| Talk | "not available yet" | | |
| Power short | sleep | | |
| Power long | power menu | | |
| *any key while sleeping* | wake and announce context | | |

In **key describer** mode (Settings → Key describer), every key says what it
does and does nothing else. Holding Home leaves the mode.

## 7. App state machine

```
            start()
              │
              ▼   last book found?
      ┌──── yes ───────────────┐ no
      ▼                        ▼
 ┌─────────┐  Home / OK  ┌──────────┐
 │ Player  │────────────►│  Menu    │
 │ screen  │◄────────────│  (stack) │
 └────┬────┘ back at top └────┬─────┘
      │  Power short / sleep timer / auto sleep
      ▼                       ▼
         ┌─────────────────┐
         │    Sleeping     │── any key ──► previous mode
         └─────────────────┘
```

- `Mode::Menu` has a **stack of `MenuLevel`s** (`title`, `items`, `index`).
  Opening an item pushes a level and going back pops one. Popping the last
  level returns to the player when a book is loaded, otherwise Home.
- `Mode::Player` is the "now playing" screen. Playback continues in the
  background while menus are open.
- `Mode::Sleeping` simulates suspend: playback is paused and the position is
  saved. Any key wakes the device and says where you are.
- **Power off** saves state, says goodbye and sets `quitRequested()`. The front
  end exits once speech has finished.

### Menu model

```cpp
struct MenuItem {
    std::string label;                               // always spoken
    std::function<std::string()> value;              // spoken after label: "Volume, 70 percent"
    std::function<void(int)> adjust;                 // Left/Right change the value
    std::function<std::vector<MenuItem>()> submenu;  // built when opened (fresh data)
    std::function<void()> action;                    // leaf
};
```

Menus are built by `App::*Menu()` functions and created lazily, so a list such
as Library or Chapters always reflects the current state. `App::choice()`
builds an adjustable item from a settings key and a list of `(value, spoken
name)` options.

The spoken form of an item is `label[, value][, i of n][. hint]`. The position
("i of n") is left out at Expert verbosity, and the hint ("Press OK to open")
is added only at Beginner.

### Current menu tree

```
Home
├── Now playing: <book>            (only when a book is loaded)
├── Library → <books>              (OK plays/resumes)
├── Podcasts                       (placeholder)
├── Sleep timer → Off · 1 · 15 · 30 · 45 min · 1 hour · End of chapter
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
│   └── Key describer
└── System
    ├── Status           battery (simulated), free space, Wi-Fi, time
    ├── Time and date
    ├── Wi-Fi →          Status · Scan for networks · Turn off   (simulated)
    ├── Sync             (not configured)
    ├── Software update
    ├── About
    ├── Factory reset →  Cancel · Erase all settings
    └── Power →          Sleep · Power off · Restart · Cancel

Player menu (OK on player screen)
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
- Previous chapter: if more than 3 s into the chapter, the first press goes to
  its start, like a CD player.
- At the end of a file the next file starts automatically. At the end of the
  book the device says "End of book" and goes back to the start, paused.

## 9. Persistence

`Settings` is a flat `key<TAB>value` file (simulator: `~/.config/oap-sim/settings.tsv`,
or `--config FILE`).

| Key | Meaning |
|-----|---------|
| `speech.rate`, `volume`, `verbosity`, `skip.back`, `skip.forward`, `rewind_on_resume`, `buttons.long_press_ms`, `menu.wrap`, `power.auto_sleep_min` | user settings |
| `last_book` | absolute path of the book to resume on start |
| `pos:<book path>` | `"<file index>:<seconds>"` resume position |

The position is saved on pause, sleep, power off, book change, and every 30 s
during playback. That interval is a trade-off against flash wear on the device.

## 10. Simulator (`app/sim/main.cpp`)

- **Window:** SDL2 plus Dear ImGui (v1.91.8, downloaded by CMake into `build/_deps`).
  - Left: the **menu stack as columns**. The selected item is highlighted, `>`
    marks a submenu and `<>` an adjustable value. On the player screen it
    shows the player key help instead.
  - Right: clickable **device buttons** (press and hold work), the hold switch,
    **now playing** with a progress bar, a table of **button usage**, and the
    **speech log**.
- **Keyboard:** Space = Play/Pause, arrows = Back/Forward/Up/Down, Enter = OK,
  Esc/Backspace = Home, T = Talk, P = Power, H = hold switch. Holding a key gives a long press.
- **Script mode:** `--script "ok down ok wait:3000 right:long play:long quit"`
  presses buttons automatically, for repeatable walkthroughs and future UI tests.
  Steps: `play left right up down ok home talk power`, with `:long` for a long
  press, plus `wait:<ms>` and `quit`.

## 11. Porting to the Linux device

| Piece | Work needed |
|-------|-------------|
| `AudioOutput` | Add `audio_pulse.cpp` (libpulse; also works on PipeWire through pipewire-pulse). Possibly later an ALSA backend for the fastest boot. Choose the backend in CMake. |
| Input | A device front end reading **evdev** (`gpio-keys`) or power-MCU events and calling `keyDown`/`keyUp`/`setHold`. No ImGui. |
| Main loop | Same shape as the simulator: poll input, call `tick()` about every 10 ms, exit on `quitRequested()`. |
| Power | Replace simulated sleep with real suspend (`/sys/power/state` or a request to the MCU). |
| Status | Battery from the fuel gauge (sysfs `power_supply`), Wi-Fi from wpa_supplicant/iwd. |
| Paths | Library under `/data/library`, settings under `/data/config`. |

Nothing in `app/core/` uses macOS APIs.

## 12. Known gaps / next steps

- Speed control (libavfilter `atempo`, or Sonic), skip silence.
- Bookmarks, podcasts (RSS), sync (Audiobookshelf).
- STT (Talk button): push-to-talk capture plus sherpa-onnx, Vosk or whisper.cpp.
- Unit tests for `App` driven by scripted input (the script runner is the first step).
- Localisation of spoken strings; a choice of TTS voice.
- The `AudioStream` mutex in the audio callback could become a lock-free ring.
