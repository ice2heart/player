# Open Audio Player for Blind Users — Design Spec (Draft v0.1)

> Status: **draft v0.2** · Last updated: 2026-10-21
> Software architecture as built: [docs/architecture.md](docs/architecture.md)
> Working name: _TBD_ (placeholder: **OAP** — Open Audio Player)

This document is the basis for implementing an open-source, screenless, button-and-voice-driven
audio player for blind and low-vision users. It covers goals, hardware, software architecture,
the audio user interface, features, and an implementation roadmap. Sections marked
**[OPEN]** need a decision.

---

## 1. Goals and non-goals

### 1.1 Goals
- **Fully usable without sight**, from unboxing onward — no sighted help needed for setup.
- **Never silent**: every action gets audible (and optionally haptic) feedback.
- **Instant-on feel**: from button press to speech in < 2 s (target), < 5 s (acceptable).
- **Long battery life** with deep sleep; resume exactly where the user left off.
- First-class **audiobooks** (incl. M4B chapters and DAISY) and **podcasts**.
- **Sync** of library and listening position with a self-hosted server.
- **On-device TTS and STT** — works offline, no cloud dependency, private by default.
- Open hardware design + open software; buildable by hobbyists.

### 1.2 Non-goals (for v1)
- Screen or visual UI (an optional status LED is fine).
- Streaming music services (Spotify etc.). A music server the user hosts
  (Subsonic, such as Navidrome) is not one of these: it is private and needs no
  account with a company. Streaming from it is built (Home → Streaming, see
  [docs/streaming.md](docs/streaming.md)). Albums and playlists can be downloaded
  and play offline, and streamed tracks are cached up to a size set in Settings.
- Phone calls, messaging, general-purpose assistant features.
- Cloud STT/TTS (may be optional plugins later, never required).

### 1.3 Design principles
1. **Predictable** — same button always does the same thing in the same context.
2. **Interruptible** — any key press stops current speech immediately.
3. **Orientation on demand** — "Where am I?" is always one press away.
4. **Forgiving** — destructive actions require confirmation; undo where possible.
5. **No time pressure** — no menus that time out while the user is thinking.
6. **Tactile first, voice second** — every function reachable with buttons alone;
   voice is a shortcut and a text-input method, not a requirement.

---

## 2. System overview

```
 ┌───────────────────────────────────────────────────────────────────┐
 │                           Device                                  │
 │                                                                   │
 │  ┌──────────────┐  I2C/UART  ┌───────────────────────────────┐    │
 │  │ Power MCU    │◄──────────►│ Linux SoC                     │    │
 │  │ (always on)  │  wake IRQ  │                               │    │
 │  │ - buttons    │───────────►│  input → UI core → audio eng. │    │
 │  │ - RTC        │  PMIC EN   │            │   ▲              │    │
 │  │ - fuel gauge │───────────►│          TTS  STT             │    │
 │  │ - haptics    │            │            │                  │    │
 │  └──────┬───────┘            │  library DB   sync (Wi-Fi)    │    │
 │         │                    └───────┬───────────────────────┘    │
 │    LiPo + charger                    │ USB host                   │
 │                              ┌───────▼────────┐                   │
 │                              │ USB Audio codec│ (UAC1/2)          │
 │                              │ mic in / HP out│                   │
 │                              └────────────────┘                   │
 └───────────────────────────────────────────────────────────────────┘
                                │ Wi-Fi
                       ┌────────▼─────────┐
                       │ Sync server      │ (e.g. Audiobookshelf)
                       └──────────────────┘
```

---

## 3. Hardware

### 3.1 SoC selection

Requirements:
- ARM Linux with mainline or well-maintained BSP kernel.
- Enough CPU for **real-time STT** (small model) + **TTS** + decoding concurrently.
- Wi-Fi (on-module or SDIO).
- USB host (for USB audio) — ideally also USB device/OTG for charging + mass storage mode.
- Supports **suspend-to-RAM** or can be fully powered down and cold-boot fast.
- ≥ 512 MB RAM (STT models), ≥ 8 GB storage (eMMC preferred over SD).

Candidates **[OPEN]**:

| SoC / Board                     | CPU                | RAM       | Wi-Fi     | Suspend-to-RAM | Notes |
|---------------------------------|--------------------|-----------|-----------|----------------|-------|
| Rockchip RK3566 (Radxa Zero 3W) | 4× A55 1.8 GHz     | 1–8 GB    | yes       | yes            | Good STT headroom, eMMC, NPU. **Front-runner.** |
| Allwinner H618 / H616           | 4× A53             | 1–4 GB    | module    | partial        | Cheap; mainline support good, suspend weaker. |
| Raspberry Pi Zero 2 W (RP3A0)   | 4× A53 1 GHz       | 512 MB    | yes       | **no**         | Huge community; no suspend → must rely on power-off + fast boot. RAM tight for STT. |
| Rockchip RV1106 (Luckfox Pico)  | 1× A7 + NPU        | 64–256 MB | no        | yes            | Very low power, fast boot, but too weak for good STT. |
| i.MX 8M Nano / Mini             | 1–4× A53           | 512 MB+   | module    | yes, excellent | Best power mgmt + long-term support; more expensive. |

Recommendation for prototyping: **RK3566 (Radxa Zero 3W)** — enough CPU for whisper-class
STT, working suspend, small form factor. Keep the software SoC-agnostic (no vendor-only APIs).

### 3.2 Power management MCU (recommended)

A small always-on MCU (e.g. **RP2040**, STM32L0/L4, or nRF52) handles:
- Scanning the buttons (so a press can wake the SoC from deep sleep or power-off).
- Hold switch, power key long-press → hard power off.
- RTC (sleep timer, podcast auto-download schedule wakeups).
- Battery fuel gauge (e.g. MAX17048) and charger status.
- Haptic motor driver (instant tactile "click" while Linux is still booting).
- Enabling/disabling SoC power rails.

Link to SoC: I²C (register map) or UART (simple framed protocol), plus one wake GPIO.
This decouples low-power behaviour from Linux and gives **instant feedback** even during boot.

> Alternative for v0 prototype: buttons directly on SoC GPIOs via the `gpio-keys` DT
> driver, no MCU. Fine for development, worse for deep sleep.

### 3.3 Buttons

Physical, tactile, **distinct shapes** so they can be identified by touch.

Proposed layout (v1):

| Button          | Shape / position           | Short press        | Long press (≥ 600 ms)     | Double press |
|-----------------|----------------------------|--------------------|---------------------------|--------------|
| **Play/Pause**  | large round, center        | play / pause       | "Where am I?" (announce)  | —            |
| **Back**        | left arrow, two-stage      | player: skip back by the step unit / menu: previous letter, or 10 items back | previous chapter or track (also deep press) | — |
| **Forward**     | right arrow, two-stage     | player: skip fwd by the step unit / menu: next letter, or 10 items on | next chapter or track (also deep press)     | — |
| **Up**          | top, ridged                | menu ↑ / player: previous step unit | first item (in menus) | —            |
| **Down**        | bottom, ridged             | menu ↓ / player: next step unit | last item (in menus)        | —            |
| **Volume up**   | side, ridged               | content volume up, on every screen; held repeats the step | — | — |
| **Volume down** | side, ridged               | content volume down, on every screen; held repeats the step | — | — |
| **Options**     | side, distinct from OK     | actions for the selected item, or for the playing track in a player | — | — |
| **Menu / OK**   | square                     | open menu / select | go back one level         | —            |
| **Home / Back** | small, recessed            | back one level     | go to Home (library root) | —            |
| **Talk (Mic)**  | side, textured             | push-to-talk       | dictation mode            | —            |
| **Power**       | side, recessed             | sleep / wake       | power menu / off (5 s = hard off) | — |
| **Hold**        | slide switch               | locks all buttons except Power (announces "locked") | | |

Notes:
- **Two-stage Left/Right** (like a camera shutter button): a light press skips, and pressing deeper jumps
  a chapter at once, without waiting for a long press. The deep stage is a second contact, so a second
  GPIO per key. **[OPEN]** Pick a two-step tact switch and check that the stages can be felt through
  the case. On one-stage hardware the long press stays the fallback.
- Debounce in MCU (or `gpio-keys` `debounce-interval`).
- Linux side sees standard **evdev** key events (`KEY_PLAYPAUSE`, `KEY_NEXTSONG`, …) so the
  app can be developed on a desktop with a keyboard. The volume keys are `KEY_VOLUMEUP` /
  `KEY_VOLUMEDOWN` and Options is `KEY_MENU`. On a gamepad R1 / R2 are volume up / down and
  Triangle is Options; in the simulator `+` / `-` and `O`; in a terminal `+` / `-` and `o`.
- **Step unit:** Up / Down in a player choose what Left / Right move by (music: Skip, Track,
  Album; book: Skip, Chapter, 10 minutes), like the navigation levels of a Victor Reader Stream.
  Settings change only in their list. See [docs/navigation.md](docs/navigation.md) for the full contract.
- **Key describer mode** (like screen readers): press a key → it says what it does, no action.

### 3.4 Audio: USB Audio Class codec

- Use a **USB Audio Class (UAC1/UAC2)** codec for both **mic input** and **headphone/speaker
  output** — driver-free on Linux (`snd-usb-audio`), easy to swap, avoids board-specific
  I²S codec work.
- Candidates: CM108/CM119 (cheap, mono mic + stereo out), CM6206, or a custom board using
  a UAC2 chip. Must expose hardware volume or we do software volume.
- **Headphone jack** with detect (for privacy) + small **speaker** with amp (for use without
  headphones). Auto-route on jack insert/remove; pause on headphone unplug.
- **Microphone**: MEMS mic, close to the Talk button. Optional second mic for noise reduction.
- Sample rates: 48 kHz playback; 16 kHz capture for STT (resampled in software if needed).

**[OPEN]** Should the device also act as a **USB audio gadget** (UAC2 gadget via ConfigFS)
so it can be used as a talking sound card for a PC? Nice-to-have, not v1.

### 3.5 Other hardware
- LiPo 1500–3000 mAh, USB-C charging (charging announced by voice + tone).
- Optional **vibration motor** (haptic confirmation).
- Optional single **LED** (for sighted helpers / debugging).
- Lanyard hole, rugged enclosure, raised tactile markings.

---

## 4. Software architecture

### 4.1 OS and boot

- **Buildroot** (preferred for fast boot and small image) or Yocto. **[OPEN]**
- Bootloader: U-Boot with minimal delay (`bootdelay=0`), consider **falcon mode** (SPL → kernel).
- Kernel: trimmed config, drivers built-in (no module loading at boot for critical path),
  `quiet`, LZ4-compressed kernel.
- Init: **not systemd** for v1 — use BusyBox init or a tiny custom init that starts the
  audio stack first. (Revisit if dependency management becomes painful.)
- Root filesystem: **read-only SquashFS** (A/B partitions for OTA updates) + **ext4/f2fs data
  partition** for library, DB, settings.
- Early audio: play a boot earcon as soon as the audio device is ready, before the full UI.

**Boot time budget (target, cold boot):**

| Stage                 | Target   |
|-----------------------|----------|
| MCU haptic click      | < 50 ms  |
| Bootloader            | < 300 ms |
| Kernel                | < 800 ms |
| Init + audio up       | < 400 ms |
| UI says first words   | < 500 ms |
| **Total**             | **< 2 s** |

Resume from suspend-to-RAM: **< 300 ms** to audio.

### 4.2 Processes / components

All components talk over a single local IPC bus. **[OPEN]** D-Bus vs. a lightweight
Unix-socket JSON/msgpack bus. Proposal: one main process (`oapd`) with internal modules for
v1 to minimize latency and boot time; split later if needed.

| Component        | Responsibility |
|------------------|----------------|
| **input**        | Reads evdev (and MCU events), turns raw keys into gestures (short/long/double), handles Hold. |
| **ui-core**      | State machine for menus/player; decides what to say and do. The "brain". |
| **audio-engine** | Decoding, playback, seeking, speed control, mixing/ducking of TTS over content. |
| **tts**          | Text → PCM. Queue with priorities and instant cancel. |
| **stt**          | Push-to-talk capture → text (commands or free dictation). |
| **library**      | Scans storage, parses metadata/chapters, SQLite DB, bookmarks, positions. |
| **sync**         | Talks to sync server: download, progress upload/download, podcast feeds. |
| **power**        | Talks to MCU, battery status, idle timers, suspend/resume, sleep timer. |
| **settings**     | Persistent config (speech rate, voice, skip intervals, language…) and the music server, in `config.yaml`; positions in `state.tsv`. |
| **net**          | Wi-Fi management (wpa_supplicant / iwd), connection status announcements. |

### 4.3 Implementation language — **decided: C++20 + CMake**
- Direct use of the C libraries we depend on (FFmpeg, eSpeak NG, sherpa-onnx/whisper.cpp).
- Platform-independent core library (`app/core`), thin platform layer (`app/platform`),
  separate front ends (desktop simulator now, device later).
- Avoid Python in the critical path (slow startup on small SoCs); OK for tooling.

### 4.4 Audio stack
- Output behind an `AudioOutput` interface:
  - **macOS (simulator): CoreAudio** — implemented.
  - **Linux (device): PulseAudio API (libpulse)** — planned. Also works on PipeWire
    systems through `pipewire-pulse`, so one backend covers both.
  - Optional later: direct **ALSA** backend if the sound server costs too much boot time /
    RAM on the device (ALSA remains the kernel layer underneath either way).
- Internal mixer: content stream + speech stream + earcons, all 48 kHz stereo float.
- **Ducking**: when TTS speaks during playback, lower content (e.g. −15 dB) or pause it
  (user setting).
- Decoding: **FFmpeg libraries (libavformat / libavcodec / libswresample)** — decided.
  Speed control via libavfilter `atempo` (or Sonic) — TODO.
- **Playback speed** 0.5×–3.0× with pitch preservation (scaletempo2 / Sonic / SoundTouch).
- Skip silence (optional, podcasts).
- Volume: software volume with sane max limit (hearing protection setting).

### 4.5 TTS (text-to-speech)
Two tiers:
- **UI speech** — must be instant and fast at high rates: **eSpeak NG** (tiny, very low
  latency, intelligible at 400+ wpm; what many screen-reader users prefer).
- **Natural voice** — optional, for reading longer texts / descriptions: **Piper** (VITS/ONNX)
  or **RHVoice**.

Requirements:
- Time-to-first-audio < 100 ms for UI speech.
- Cancel within 20 ms on any key press.
- Adjustable rate, pitch, volume, voice; per-language voices.
- Pre-render frequent phrases ("Paused", "Playing", menu names) to a cache at first boot.
- Speak numbers/times naturally ("3 hours 12 minutes left").

### 4.6 STT (speech-to-text), on device
Use cases:
1. **Commands**: "next chapter", "sleep timer 30 minutes", "go to podcasts", "what time is it".
2. **Search**: "play Dune", "find podcast about history".
3. **Text entry**: Wi-Fi SSID/password, server URL, search queries.

Engines **[OPEN]**:
- **sherpa-onnx** (streaming Zipformer / Paraformer; also does Whisper, TTS, VAD, KWS) —
  strong candidate for one unified on-device speech library.
- **whisper.cpp** (tiny/base, quantized) — great accuracy for dictation, not streaming.
- **Vosk** — grammar-constrained recognition = very accurate/fast for command sets.

Design:
- **Push-to-talk** (hold Talk button) — no always-listening wake word in v1 (battery + privacy).
- Two modes:
  - **Command mode**: constrained grammar / intent matching → high accuracy, low latency.
  - **Dictation mode**: free text; result is **read back** and confirmed before use.
- **Spelling mode** for passwords: letters, digits, NATO alphabet ("alpha", "bravo"),
  "capital", "symbol at", "delete last", "read back".
- VAD to trim silence; earcon on start/stop of listening.

### 4.7 Storage and library
- SQLite DB: items, chapters, positions, bookmarks, play state, podcast feeds/episodes.
  The library index (local files, downloads and server albums, with where each was seen) is
  `library.db` next to `state.tsv`. The app core holds the index in memory and loads and saves
  it through a `LibraryStore` interface; only the SQLite implementation knows the database
  ([docs/streaming.md](docs/streaming.md) phase 4). The scan of the files runs in the background and
  reads tags only of new or changed files, so start-up shows the saved index at once; the
  database is a cache that a scan and a sync rebuild, so a damaged file is moved aside and replaced.
- Folder layout on data partition:
  ```
  /data/library/audiobooks/<Author>/<Title>/...
  /data/library/podcasts/<Feed>/<episode>.mp3
  /data/library/music/   (optional)
  ```
- **USB mass-storage mode** (or MTP) when connected to a PC to copy files directly.
- Position saving: kept in memory on pause and book change, and written to
  flash only on suspend, power-off and restart, to spare the flash. The same holds
  for the place in the album or playlist streamed last. A crash or
  sudden power loss loses the place since then, so the power MCU must shut down
  cleanly at low battery (see [docs/streaming.md](docs/streaming.md#configuration-one-yaml-file)).

---

## 5. Audio user interface

### 5.1 Concepts
- **Speech** for content (names, numbers, status).
- **Earcons** (short sounds) for structure: menu open/close, wrap-around, end of list,
  error, listening start/stop, boot, low battery, waiting for the network (a stream that
  ran dry). Keep them short (< 150 ms) and distinct.
- **Haptics** (optional) mirror earcons for noisy environments.
- **Verbosity levels**: Beginner (full hints) / Normal / Expert (minimal speech).

### 5.2 Menu model
Hierarchical list navigation, like a screen reader:
- **Up/Down** move through items, each item is spoken: _"Podcasts, 3 of 6"_.
- **OK** enters / activates. **Home short** goes up a level. **Home long** goes to root.
- **OK goes in, Home goes out** of a stack of screens (menus and the player). **Left/Right jump in a long list**
  (to the next or previous letter in a list sorted by name, else 10 items). A setting is changed
  in its list, opened with OK. Full rules: [docs/navigation.md](docs/navigation.md).
- Position info (`n of m`) is configurable.
- Wrap-around at end of list with a distinct earcon (setting: wrap or stop).
- **First-letter navigation** via voice: "go to D".
- Re-announce current item on Play long press ("Where am I?").

### 5.3 Menu tree (v1)

> The tree currently implemented in the simulator (including the **System** menu: status,
> time, Wi-Fi, sync, update, about, factory reset, power) is listed in
> [docs/architecture.md §7](docs/architecture.md#current-menu-tree).


```
Home
├── Continue listening          (last item, one press to resume)
├── Audiobooks                 (built so far as "Library", a flat list of books: those on the server are listed after the local ones, value "on the server")
│   ├── Recently played
│   ├── By author
│   ├── By title
│   └── <book> → Play / Chapters / Bookmarks / Info / Mark finished / Delete
├── Podcasts
│   ├── New episodes
│   ├── Subscriptions → <feed> → <episode>
│   ├── Add podcast (search / URL by voice)
│   └── Refresh now
├── Music                      (albums grouped by their tags; one library of local and server albums, an album on both once)
│   ├── Play all               (what is on the device, never the whole server)
│   ├── Artists
│   ├── Albums A to Z
│   ├── Genres
│   ├── Recently added
│   ├── Recently played
│   ├── Refresh library        ("Updating the library.", then the result or the error)
│   └── <album> → tracks (Album info, where it plays from, is in its Options)
├── Streaming                  (own music server; named after it; the last album or playlist resumes after a restart; docs/streaming.md)
├── Bookmarks
├── Settings
│   ├── Speech (voice, rate, pitch, verbosity)
│   ├── Playback (skip intervals, speed, skip silence, ducking)
│   ├── Buttons (key describer, long-press time, hold behaviour)
│   ├── Wi-Fi
│   ├── Sync account
│   ├── Power (auto-sleep time, deep sleep)
│   ├── Streaming quality (original file, or a transcode for a slow link)
│   ├── Cache size (off / 1 / 4 / 16 GB for tracks cached while streaming; off caches nothing new but keeps what is cached)
│   ├── Sleep timer
│   ├── Language
│   └── About / Update / Reset
├── Status (battery, time, Wi-Fi, storage)
└── Help (getting started, buttons, key describer)
```

### 5.4 Player screen ("Now playing" context)
| Action                | Behaviour |
|-----------------------|-----------|
| Play/Pause            | toggles; says "Paused" / earcon on resume (configurable) |
| Back / Forward short  | move by the step unit: skip 10 s / 30 s (configurable), track or chapter, album or 10 minutes |
| Back / Forward long or deep | previous / next chapter or track; announces chapter name |
| Up / Down             | choose the step unit for Back / Forward; speaks it ("Track") |
| Volume up / down      | content volume, held repeats; earcon pitch indicates level, edge sound at the limits |
| Options               | actions for the playing track (Go to album, Album info; Star later); the book player says "No options" |
| Play long             | "Chapter 7 of 22, 'The Storm'. 14 minutes left in chapter, 6 hours 3 minutes left in book." |
| OK                    | player menu: speed, bookmark add, sleep timer, chapters, info |
| Talk                  | voice command |

### 5.5 Onboarding (first boot)
- Device speaks immediately on first power-on; explains the Talk button and Up/Down/OK.
- Interactive **button tutorial** (press each button, device names it).
- Language selection first (by voice or Up/Down).
- Optional Wi-Fi and sync setup; skippable and revisitable.

### 5.6 Localization
- All UI strings in translation catalogs (gettext / Fluent).
- TTS voice and STT model chosen per language; ship English first, others as packs.

---

## 6. Features

### 6.1 Audiobooks
- Formats: MP3, M4A/**M4B** (chapters), AAC, Opus/OGG, FLAC, WAV.
- **DAISY 2.02 / DAISY 3** talking books (heading-level navigation) — important for the
  blind community; libraries (e.g. national talking-book libraries) distribute in DAISY.
  **[OPEN]** v1 or v2?
- Multi-file books (one folder = one book), ordered by track number / filename.
- Chapters from: M4B atoms, ID3 CHAP, CUE files, DAISY NCC/NCX, or one-file-per-chapter.
- Per-book resume position, speed, and bookmarks (with optional voice note label).
- Mark finished; "rewind a bit on resume" (e.g. back 5 s after long pause).

### 6.2 Podcasts
- Subscribe by RSS URL (dictated), OPML import, or search (via sync server or podcast index).
- Auto-download new episodes over Wi-Fi (scheduled via MCU RTC wake), keep last N.
- Played/unplayed state; auto-delete played episodes (setting).
- Speed control, skip silence, chapter support (Podcasting 2.0 chapters).

### 6.3 Web sync
Primary target: **Audiobookshelf** (open-source, self-hosted, has REST API for library,
downloads, and progress sync; has podcast support).
- Download books/episodes from server to device.
- Sync **listening progress** both ways (latest-timestamp-wins, with conflict announcement:
  "Server position is further, jump there?").
- Sync bookmarks.
- Works offline; queue updates and push when online.

Secondary / alternatives **[OPEN]**:
- **gPodder sync API** (gpodder.net / Nextcloud gPodder Sync) for podcast subscriptions.
- **WebDAV / Nextcloud** folder sync for plain files.
- **Syncthing** for file sync.

Setup without a screen:
- Dictate server URL + credentials using spelling mode, **or**
- Device opens a temporary Wi-Fi AP + local web page (setup from phone with screen reader), **or**
- Drop a `config.yaml` onto the device in USB mass-storage mode.

### 6.4 Power and deep sleep
States:

| State            | SoC                 | Wake by                | Typical |
|------------------|---------------------|------------------------|---------|
| Playing          | on                  | —                      |         |
| Idle             | on, CPU low-freq    | —                      | after pause |
| **Suspend (RAM)**| suspend-to-RAM      | any button (via MCU), RTC | after 2 min idle |
| **Off**          | power cut by MCU    | Power/Play button, RTC | after N hours suspended |

- Always save state before suspend/off.
- Resume returns to exactly the previous context and announces it briefly
  ("Dune, paused, chapter 7").
- **Sleep timer**: duration or "end of chapter"; fades out over last 30 s; shake/press to extend
  (optional accelerometer).
- Battery announcements: on demand, at 20 %, 10 %, 5 %; charging start/full.
- Wi-Fi off when not syncing.

### 6.5 Updates
- A/B rootfs, OTA via Wi-Fi (SWUpdate or RAUC) **or** update file copied via USB.
- Spoken progress, automatic rollback on failed boot.

---

## 7. Development workflow

- **Desktop simulator** (`oap-sim`, implemented for macOS): the core runs on a normal PC;
  keyboard keys map to device buttons, laptop speakers used for audio. An SDL2 + Dear ImGui
  window visualises the menu stack, playback state, speech log and **per-button usage
  counts** (to decide how many buttons the hardware really needs). A `--script` mode
  replays button sequences. Most work happens here; then port to Linux.
- Hardware-abstraction layer for: input source, power/MCU, battery, Wi-Fi, Bluetooth,
  audio routing. Plan: [docs/hal.md](docs/hal.md).
- CI: unit tests for ui-core state machine (scripted key sequences → expected speech output
  as text), cross-compile for target, build Buildroot image.
- **Testing with blind users** from the earliest prototype — mandatory, not optional.

### Repository layout (proposal)
```
/docs              specs, UI guidelines, hardware notes
/firmware/mcu      power MCU firmware
/hardware          schematics, PCB, enclosure (KiCad, FreeCAD/OpenSCAD)
/os                Buildroot external tree, board configs, kernel config
/app               main daemon (oapd) + modules
/app/sim           desktop simulator
/assets            earcons, prebuilt prompts
/i18n              translations
```

---

## 8. Roadmap

| Milestone | Scope |
|-----------|-------|
| **M0 — Simulator** | Desktop app: menu engine, eSpeak NG TTS, local file playback, keyboard input. **In progress — macOS version working.** |
| **M0.5 — Linux port** | PulseAudio backend, evdev input front end, run the simulator on Linux. |
| **M1 — Player core** | Audiobooks with chapters, resume, bookmarks, speed, sleep timer. |
| **M2 — Hardware bring-up** | Dev board + USB audio + GPIO buttons; Buildroot image; boot time measurement. |
| **M3 — STT** | Push-to-talk commands + dictation + spelling mode. |
| **M4 — Network** | Wi-Fi setup, podcasts (RSS), Audiobookshelf sync. |
| **M5 — Power** | MCU firmware, suspend/resume, deep sleep, battery reporting. |
| **M6 — Custom PCB + enclosure** | First integrated prototype; user testing round. |
| **M7 — v1.0** | OTA updates, localization, DAISY (if not earlier), docs for builders. |

---

## 9. Open questions

1. Final SoC choice (RK3566 vs. i.MX 8M vs. Pi Zero 2 W) — needs power + STT benchmarks.
2. ~~Single vs. multi-process~~ — single process with threads for now (see architecture doc).
3. ~~Decoder backend~~ — FFmpeg libraries.
4. STT engine: sherpa-onnx vs. whisper.cpp vs. Vosk (benchmark accuracy/latency/RAM).
5. Buildroot vs. Yocto.
6. DAISY support in v1?
7. Speaker built-in or headphones-only?
8. Sync: Audiobookshelf only, or also gPodder/WebDAV in v1?
9. Licence: GPLv3 (software) + CERN-OHL-S (hardware)? (eSpeak NG is GPLv3 — check compatibility.)
10. Project name.

---

## 10. References
- eSpeak NG — https://github.com/espeak-ng/espeak-ng
- Piper TTS — https://github.com/rhasspy/piper
- RHVoice — https://github.com/RHVoice/RHVoice
- sherpa-onnx — https://github.com/k2-fsa/sherpa-onnx
- whisper.cpp — https://github.com/ggerganov/whisper.cpp
- Vosk — https://alphacephei.com/vosk/
- Audiobookshelf — https://www.audiobookshelf.org/
- DAISY Consortium — https://daisy.org/
- Buildroot — https://buildroot.org/
- Prior art: Victor Reader Stream (HumanWare), BookSense (HIMS) — study their button
  layouts and UX.
