# Open Audio Player for Blind Users — Design Spec (Draft v0.2)

> Status: **draft v0.2** · Last updated: 2026-10-30. Product goals and hardware targets below are proposals unless marked built.
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
- **Instant-on feel**: from a power or wake button press to the first audible response in < 2 s (target), < 5 s (acceptable) on target hardware. This has not been measured on a device.
- **Long battery life** with deep sleep; resume exactly where the user left off.
- First-class **audiobooks** (incl. M4B chapters and DAISY) and **podcasts**.
- **Sync** of library and listening position with a self-hosted server.
- **On-device TTS and STT** — works offline, no cloud dependency, private by default.
- Open hardware design + open software; buildable by hobbyists.

### 1.2 Non-goals (for v1)
- Screen or visual UI (an optional status LED is fine).
- Streaming music services (Spotify etc.). A music server the user hosts
  (Subsonic, such as Navidrome) is not one of these: it is private and needs no
  account with a company. Streaming from it is built (Home, one entry per server, see
  [docs/streaming.md](docs/streaming.md)). Albums and playlists can be downloaded
  and play offline, and streamed tracks are cached at the quality played, up to a
  size set in Settings. Download quality is independent of Streaming quality;
  optional replacement upgrades a lower-quality cached copy, with local fallback
  if the better stream cannot open (see §4.7).
- Phone calls, messaging, general-purpose assistant features.
- Cloud STT/TTS (may be optional plugins later, never required).

### 1.3 Design principles
1. **Predictable** — same button always does the same thing in the same context.
2. **Interruptible** — active controls stop current speech immediately; unused Up/Down/Talk stay silent.
3. **Orientation on demand** — "Where am I?" is always one press away.
4. **Forgiving** — destructive actions require confirmation; undo where possible.
5. **No time pressure** — no menus that time out while the user is thinking.
6. **Tactile first, voice second** — every function reachable with buttons alone;
   voice is a shortcut and a text-input method, not a requirement.

**Speech interruption acceptance target:** Measure from the electrical key event
to the last audible sample of the old utterance at the headphone output, not to
the call that cancels TTS or flushes its in-process FIFO. The current TTS cancels
synthesis and flushes the mixer speech stream, but the PulseAudio and ALSA
backends can hold about 50 ms of already submitted audio. Their actual drain
time, plus input and render scheduling, must be measured on target hardware.
Record both the latency to silence and the latency to the next utterance for
repeated key presses while content audio plays and while it is paused. The
"immediately" principle is a target, not a measured device guarantee. Use the
prototype RK3566 board if selected, the intended USB audio codec and headphones,
and a representative locally stored audiobook while eSpeak announces menu items.
Report median, 95th percentile and worst observed latency across repeated trials,
with the board, audio backend, software image and load recorded. Re-run on the
final hardware because a desktop result is not a device acceptance result.

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
| **Back**        | left arrow, two-stage      | player: previous chapter/track or restart after >3 s / menu: previous item | player: seek back / menu: first item | — |
| **Forward**     | right arrow, two-stage     | player: next chapter/track / menu: next item | player: seek forward / menu: last item     | — |
| **Up**          | top, ridged                | unused during normal use | unused | —            |
| **Down**        | bottom, ridged             | unused during normal use | unused        | —            |
| **Volume up**   | side, ridged               | content volume up, on every screen; held repeats the step | — | — |
| **Volume down** | side, ridged               | content volume down, on every screen; held repeats the step | — | — |
| **Options**     | side, distinct from OK     | actions for the selected item, or for the playing track in a player | — | — |
| **Menu / OK**   | square                     | open player menu / select | Options for selected item/current playback         | —            |
| **Esc / Home** | small, recessed            | previous screen; at Home return to loaded player without resuming | go to Home | —            |
| **Talk (Mic)**  | side, textured             | unused; voice input planned | unused            | —            |
| **Power**       | side, recessed             | sleep / wake       | power menu / off (5 s = hard off) | — |
| **Hold**        | slide switch               | locks all buttons except Power (announces "locked") | | |

Notes:
- **Two-stage Left/Right**: deep moves ten menu items immediately, then a continued
  hold reaches the first/last item. In a player, light hold seeks 5 seconds every
  500 ms and deep seeks 30 seconds every 500 ms immediately. Easing to light
  changes the rate without an extra step or timer reset; release stops and announces
  the position. Seek stays within the file, preserves playing/paused state and
  stops on screen change, Hold, sleep or shutdown. Ten/end menu jumps clamp;
  ordinary steps obey Menu wrap. Defaults are approved but still need device trials.
  The deep stage needs a second contact/GPIO. **[OPEN]** Pick a two-step tact switch
  and check that both stages can be felt through the case. One-stage keys still
  support light holds.
- Debounce in MCU (or `gpio-keys` `debounce-interval`).
- Linux side sees standard **evdev** key events (`KEY_PLAYPAUSE`, `KEY_NEXTSONG`, …) so the
  app can be developed on a desktop with a keyboard. The volume keys are `KEY_VOLUMEUP` /
  `KEY_VOLUMEDOWN` and Options is `KEY_MENU`. On a gamepad R1 / R2 are volume up / down and
  Triangle is Options; in the simulator `+` / `-` and `O`; in a terminal `+` / `-` and `o`.
- Up/Down/Talk are silent during normal use. Step units and configurable skip
  intervals are retired; old saved values are tolerated. The separate Options
  button aliases held OK. Settings change only in their list. Full contract:
  [docs/navigation.md](docs/navigation.md).
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

**Boot time budget (unmeasured target, cold boot):** Cold boot starts with SoC power off and ends at the first audible UI word. Measure from the electrical button event, including MCU wake, bootloader, kernel, init, audio device readiness and application startup. Record the board, storage, audio device, software image and repeated-trial distribution when hardware exists.

| Stage                 | Target   |
|-----------------------|----------|
| MCU haptic click      | < 50 ms  |
| Bootloader            | < 300 ms |
| Kernel                | < 800 ms |
| Init + audio up       | < 400 ms |
| UI says first words   | < 500 ms |
| **Total**             | **< 2 s** |

Resume from suspend-to-RAM: **< 300 ms to audible response** is a separate, unmeasured warm-resume target, measured from the electrical wake event. A screen or log event does not count as audible output.

### 4.2 Processes / components

The current simulator and headless front end each run as one C++20 process with
worker threads; modules use in-process interfaces and queues. There is no local
IPC bus. A future device daemon could retain this arrangement. If processes are
split later, the bus and protocol need a separate design decision.

| Component        | Responsibility |
|------------------|----------------|
| **input**        | Built: evdev and simulator keys, gestures and Hold. MCU input is planned. |
| **ui-core**      | Built: state machine for menus and players; decides what to say and do. |
| **audio-engine** | Built: decoding, playback, seeking and mixing/ducking of TTS over content. Speed control is planned. |
| **tts**          | Built: eSpeak NG text to PCM, queued utterances and cancellation. Speech priorities are planned. |
| **stt**          | Planned: push-to-talk capture to commands or dictation. |
| **library**      | Scans storage and parses metadata; the SQLite index holds items, tracks and playlists; history recovery needs phase 7 of fixes.md. Book positions live in `state.tsv`; bookmarks are planned. |
| **sync**         | Talks to Subsonic and Audiobookshelf servers for built browsing, downloads and supported progress sync; podcast feeds are planned. |
| **power**        | Built: software sleep and sleep timer, simulated power and storage snapshots, and battery-event warnings with save before supported critical power-off. Planned: MCU, real battery status and hardware suspend/resume. |
| **settings**     | Persistent config (speech rate, voice, volume, language…) and the music server, in `config.yaml`; positions in `state.tsv`. |
| **net**          | Built: server access and status. Wi-Fi management and connection announcements are planned. |

### 4.3 Implementation language — **decided: C++20 + CMake**
- Direct use of the C libraries we depend on (FFmpeg, eSpeak NG, sherpa-onnx/whisper.cpp).
- Platform-independent core library (`app/core`), thin platform layer (`app/platform`),
  separate front ends (desktop simulator now, device later).
- Avoid Python in the critical path (slow startup on small SoCs); OK for tooling.

### 4.4 Audio stack
- Output behind an `AudioOutput` interface:
  - **macOS (simulator): CoreAudio** — implemented.
  - **Linux (simulator and headless front end): PulseAudio simple API** — implemented;
    PipeWire can serve it through `pipewire-pulse`.
  - **Linux: direct ALSA PCM** — implemented, including `alsa:DEVICE` selection.
  - **Null output** — implemented for headless runs without a sound card.
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
- UI speech time-to-first-audible-audio < 100 ms is an unmeasured warm-system target:
  measure from UI dispatch to the first headphone output sample with the engine
  already initialized, both idle and while decoding and playing a local book.
- Audible cancellation within 20 ms of a key press remains an unmeasured target.
  Measure software cancellation (UI dispatch to TTS stop and mixer flush) separately;
  it does not establish the audible target. See section 1.3 for the measurement boundary.
- Adjustable rate, pitch, volume, voice; per-language voices.
- Names in other scripts (Cyrillic, Greek, Chinese, Japanese, Korean, Hebrew, Arabic) are spoken
  with that script's voice when it is installed, else transliterated into Latin letters
  (Cyrillic, Greek, kana, Hangul), else named once ("Chinese text", "Japanese text", ...),
  never spelled letter by letter. Kanji are read by the Chinese voice when it is installed, else named. Details in
  [docs/architecture.md](docs/architecture.md#5-speech-tts).
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
- **Built: download and cache quality** ([fixes.md phase 5](docs/fixes.md#phase-5-download-quality-and-the-cache)).
  Subsonic Download album / playlist uses Download quality: original (default),
  320, 192 or 128 kbps, independently of Streaming quality. Original-only Books
  providers keep originals. A complete copy at least as good is kept and pinned;
  a lower copy stays usable until the replacement is complete and saved.
  Automatic caching fetches a streamed music track again at its opened quality
  after 10 seconds heard, subject to Cache size and free space. Replace cached
  copies is off by default; on, a lower-quality copy can stream at Streaming
  quality while its server is not marked down, then be replaced silently.
  Failed stream opening falls back to the copy at the requested position and
  marks the server down; a server already marked down uses the copy directly.
  Replacement preserves pinning. Real-server listening checks remain pending.
- SQLite DB: the currently built `library.db` is primarily a rebuildable index of items,
  tracks, references and playlists. It does not own positions or bookmarks.
  The library index (local files, downloads and server albums, with where each was seen) is
  `library.db` next to `state.tsv`. The app core holds the index in memory and loads and saves
  it through a `LibraryStore` interface; only the SQLite implementation knows the database
  ([docs/streaming.md](docs/streaming.md) phase 4). The scan of the files runs in the background and
  reads tags only of new or changed files, so start-up shows the saved index at once; the
  database is a cache that a scan and a sync rebuild, so a damaged file is moved aside and replaced.
  However, first-seen and last-played history is not fully rebuildable today.
  [fixes.md phase 7](docs/fixes.md#phase-7-listening-reliability-and-durable-data)
  plans explicit ownership and recovery for authoritative data.
  Book positions and last-player state are in `state.tsv`; the music queue and its
  place are in `queue.tsv`. Bookmarks and podcast state need a separate design.
- Folder layout on data partition:
  ```
  /data/library/audiobooks/<Author>/<Title>/...
  /data/library/podcasts/<Feed>/<episode>.mp3
  /data/library/music/   (optional)
  ```
- **USB mass-storage mode** (or MTP) when connected to a PC to copy files directly.
- Position saving: local places are kept in memory on pause and book change,
  and written to flash on sleep, power-off, exit and restart. The music queue's
  place is saved with `queue.tsv` at those boundaries. This policy prioritizes
  flash longevity over reducing the loss window after abrupt power failure.
  A chapter-boundary checkpoint is a proposal only, pending a concrete design
  and device measurements of write frequency, wear and useful recovery. There is
  no periodic local position write requirement. A crash or
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
- Short **Back/Forward** moves one item; deep moves ten; held reaches first/last.
  Lists retain sorting, but keys do not jump by letter. Only single steps wrap.
- Short **OK** enters/activates; held OK opens Options. Short **Esc** goes back,
  or returns from Home to a loaded player without resuming it; held Esc goes Home.
- Settings change only in their list, opened with OK. Paged remote lists keep
  Load more; jumping to a loaded boundary never fetches more automatically and
  announces "More items are available. Load more to continue." when appropriate.
  Full rules: [docs/navigation.md](docs/navigation.md).
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
│   ├── Playlists              (the server's, from the index; downloaded ones too; works offline)
│   ├── Genres
│   ├── Recently added
│   ├── Recently played
│   ├── Refresh library        ("Updating the library.", then the result or the error)
│   └── <album> → tracks (Album info, where it plays from, is in its Options)
├── <server name>              (one entry per server in config.yaml, in config order; "Server" when there is one and it has no name, else "Server 1", ...; its menu is Streaming, docs/streaming.md. The music queue, local and server tracks mixed, resumes after a restart; docs/queue.md)
├── Bookmarks
├── Settings
│   ├── Speech (voice, rate, pitch, verbosity)
│   ├── Playback (speed, skip silence, ducking; planned controls)
│   ├── Buttons (key describer, long-press time, hold behaviour)
│   ├── Wi-Fi
│   ├── Sync account
│   ├── Power (auto-sleep time, deep sleep)
│   ├── Streaming quality (original file, or 320 / 192 / 128 kbps for a slow link)
│   ├── Download quality (original file by default, or 320 / 192 / 128 kbps; independent of Streaming quality)
│   ├── Cache size (off / 1 / 4 / 16 GB for tracks cached while streaming; off caches nothing new but keeps what is cached)
│   ├── Replace cached copies (off by default; on prefers a better stream when the server is not marked down)
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
| Back / Forward short  | previous/next chapter or track; Back restarts after more than three seconds |
| Back / Forward held | repeated 5-second seeks every 500 ms; deep: 30 seconds every 500 ms, immediately; release stops and speaks position |
| Up / Down / Talk      | silent and inactive during normal use |
| Volume up / down      | content volume, held repeats; earcon pitch indicates level, edge sound at the limits |
| Options               | Music: actions for the playing track (Go to album, Album info, Star / Unstar for a supported server-linked track, including local playback). Book: when a newer, distant server place is offered, the first action accepts it; otherwise it says "No options". On a list item: Play now, Play next, Add to queue, and for a server album or playlist Download |
| Play long             | "Book player. Dune, by Frank Herbert. Chapter 7 of 22, 'The Storm'. 14 minutes left in chapter, 6 hours 3 minutes left in book." |
| OK                    | player menu. Music: queue (list, move, remove, save as server playlist, clear), shuffle, repeat (off, all, album, track), go to first track, sleep timer, add current track to playlist. Book: chapters, sleep timer, info, speed, bookmark add |
| OK held               | Options for current playback |

### 5.5 Onboarding (first boot)
- Device speaks immediately on first power-on; explains Back/Forward, OK, Esc and volume (voice tutorial planned).
- Interactive **button tutorial** (press each button, device names it).
- Language selection first (by buttons; voice later).
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
- Per-book resume position is built. Speed control and bookmarks (with optional voice note label) are planned; Add bookmark currently speaks a placeholder.
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
- Sync **listening progress** both ways. Playback starts at the local place while the server
  is asked. A newer server place more than 30 seconds away moves a book with no local place;
  with an existing local place it is offered without interrupting playback: "The server has a
  later place, chapter 5. It is in Options." The first Options action is "Go to the server's place, chapter
  5, 12 minutes"; accepting it moves there and preserves pause. Older or nearby places and
  failed background requests stay silent.
- Sync bookmarks.
- Works offline; keep failed book-progress saves pending and retry on the next startup
  with a reachable server.

Audiobookshelf is a second **server type** next to Subsonic. `config.yaml` holds a list of
servers (`servers:`), each with its own `type`, and each server has its own Home entry. An
Audiobookshelf server logs in with an API key. Built: its books are listed in Library, play in
the book player with the server's chapters and keep their place, can be downloaded and play
offline, and its Home entry has a Books menu (Continue listening, Recently added, Books A to Z,
Authors, Downloaded, Server status). Progress is sent on pause, changing books, sleep, exit,
and every 60 seconds of playback when it moved at least 5 seconds; end of book sends completion.
Continue listening opens at the server’s place even when a local place exists. Progress sync
is implemented; real-server and device checks are pending. Bookmarks sync and podcasts from
Audiobookshelf are not built. See [docs/audiobookshelf.md](docs/audiobookshelf.md).

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

- **Desktop simulator** (`oap-sim`, implemented for macOS and Linux): the core runs on a normal PC;
  keyboard keys map to device buttons, laptop speakers used for audio. An SDL2 + Dear ImGui
  window visualises the menu stack, playback state, speech log and **per-button usage
  counts** (to decide how many buttons the hardware really needs). A `--script` mode
  replays button sequences. The window draws names in any script (`--font TTF` picks the font; by default a system font with Cyrillic and CJK glyphs). Most UI work happens here; validate device behavior with the Linux headless front end.
- Hardware-abstraction foundations are built: an event queue, selectable simulated power
  and storage snapshots, and battery-event handling. Simulator controls for changing
  snapshots and real hardware backends remain planned. Input, Wi-Fi, Bluetooth and
  audio routing are future HAL work: [docs/hal.md](docs/hal.md).
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

Milestone names describe product scope, not completion gates. Current software
status is summarized here; device hardware, power, boot and listening targets
remain to be measured on the chosen board. The present Linux headless front end
is a development front end, not a completed hardware product.

| Milestone | Scope |
|-----------|-------|
| **M0 — Simulator** | Built on macOS and Linux: menu engine, eSpeak NG TTS, local playback, keyboard input and SDL window. |
| **M0.5 — Linux port** | Built: PulseAudio and ALSA backends, evdev and terminal input, headless front end and Linux simulator. Device listening checks remain. |
| **M1 — Player core** | Built: local and server books, chapters where available, resume, sleep timer, music queue and server playback. Planned: bookmarks and speed control. |
| **M2 — Hardware bring-up** | Dev board + USB audio + GPIO buttons; Buildroot image; boot time measurement. |
| **M3 — STT** | Push-to-talk commands + dictation + spelling mode. |
| **M4 — Network** | Built in software: Subsonic streaming, playlist editing, offline playlists, book progress and queue sync; Audiobookshelf books and progress sync. Deferred features and pending sync states are in [docs/deferred.md](docs/deferred.md). Planned: Wi-Fi setup and podcasts (RSS). Real-server and device checks for Audiobookshelf progress remain. |
| **M5 — Power** | MCU firmware, suspend/resume, deep sleep, battery reporting. |
| **M6 — Custom PCB + enclosure** | First integrated prototype; user testing round. |
| **M7 — v1.0** | OTA updates, localization, DAISY (if not earlier), docs for builders. |

---

Draft implementation plans: [input methods](docs/input-methods.md),
[shared runtime and App decomposition](docs/refactoring.md), and
[listening reliability](docs/fixes.md#phase-7-listening-reliability-and-durable-data).

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
