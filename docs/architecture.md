# Software Architecture

> Status: describes the code as of v0.1 (simulator on macOS and Linux, headless Linux front end,
> streaming from a Subsonic server, with a local cache and downloads: phases 1 to 3 of [streaming.md](streaming.md)). Last updated: 2026-10-27.
> For product goals, hardware and the overall plan see [../SPEC.md](../SPEC.md).

## 1. Overview

The software is one C++20 program in layers. A front end wires the others together;
core depends on none of them:

```
┌───────────────────────────────────────────────────────────────────────┐
│ Front end (per target)                                                │
│   app/sim/       desktop simulator: SDL2 window, Dear ImGui, keyboard │
│   app/headless/  device: evdev buttons, terminal keys, no screen      │
│   app/frontend/  shared: scripted presses, terminal keys, gamepad map │
│                  makeMusicServer(): Subsonic, else NoMusicServer      │
├───────────────────────────────────────────────────────────────────────┤
│ Core  (app/core/, plain C++, no dependencies, library "oapcore")      │
│   App ── menus, player screen, power states, settings                 │
│    ├── GestureDetector   press/release → short / long press           │
│    ├── Library           scans folders into books                     │
│    ├── Settings          dotted keys in memory, SettingsStore for I/O │
│    └── services.h        Speech · MediaPlayer · SoundEffects ·        │
│                          VolumeControl · Clock · MusicServer ·        │
│                          Downloader · TextFiles (interfaces)          │
│   TrackCache             downloaded and cached tracks, manifests      │
│   Earcons                synthesized  → effects stream                │
│   Mixer                  content + speech + effects → PCM, ducking    │
├───────────────────────────────────────────────────────────────────────┤
│ Config  (app/config/, library "oapconfig", yaml-cpp)                  │
│   YamlStore : SettingsStore   config.yaml (settings, server)          │
│   TsvStore  : SettingsStore   state.tsv (positions, what to resume)   │
│   DiskTextFiles : TextFiles   cache manifests, written atomically     │
├───────────────────────────────────────────────────────────────────────┤
│ Net  (app/net/, library "oapnet", libcurl + yaml-cpp)                 │
│   Subsonic : MusicServer   URLs with token auth, JSON answers         │
│   HttpClient               libcurl on one worker thread, timeouts     │
│   HttpDownloader : Downloader   track files to *.part, own thread     │
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
The two front ends wire the same core, config, net, media and audio code together;
they differ only in where button presses come from and whether there is a window.
Config and net read YAML and JSON with yaml-cpp, and net uses libcurl; core sees
neither, only `SettingsStore`, `TextFiles`, `MusicServer` and `Downloader`.

`App` has no audio code. It only sees the interfaces in `services.h`, and the
front end wires in the real implementations (`Tts`, `Player`, `Earcons`, `Mixer`,
`SystemClock`, `DiskTextFiles`, and from `makeMusicServer()` / `makeDownloader()` a `Subsonic` and an
`HttpDownloader` for the server in `config.yaml` when built with `OAP_BUILD_NET`, else a `NoMusicServer`
and a `NoDownloader`).
The unit tests wire in fakes instead (section 12).

## 2. Source layout

| Path | Contents |
|------|----------|
| `CMakeLists.txt` | Builds `oapcore`, `oapconfig`, `oapfrontend`, `oapnet` (`OAP_BUILD_NET`, libcurl via pkg-config; on by default when a front end is built), `oapmusicserver` (`makeMusicServer()`, linking `oapnet` when it is built), `oapmedia`, `oapaudio` (backends from `OAP_AUDIO_BACKENDS`), `imgui` and `oap-sim` (`OAP_BUILD_SIM`), `oap-headless` (`OAP_BUILD_HEADLESS`, Linux), plus `oap-tests`, `oap-net-tests` (with `OAP_BUILD_NET`) and `oap-media-tests` (with a front end) (`OAP_BUILD_TESTS`). ImGui and GoogleTest are fetched at configure time, and yaml-cpp too when the system has none. |
| `app/core/app.{h,cpp}` | `App`: the UI state machine and all menus. |
| `app/core/buttons.{h,cpp}` | `Button`, `Gesture`, `GestureDetector`. |
| `app/core/menu.h` | `MenuItem`, `MenuLevel` data model. |
| `app/core/services.h` | Interfaces `App` depends on: `Speech`, `MediaPlayer`, `SoundEffects`, `VolumeControl`, `Clock` (+ `SystemClock`), `TextFiles` (whole text files for the cache manifests: `read()` and `writeAtomically()`, which leaves the old or the new contents after a crash; core does no file I/O of its own), and `Downloader` (queues a track download into `<dest>.part`, renamed when complete; the same `RequestId` / `poll()` / `cancel()` contract as `MusicServer`, results `Ok` / `NotConfigured` / `Unreachable` / `Server` / `Disk`; `App` takes one for Download album / playlist), and `MusicServer` with its `RemoteTrack` (with the original's `suffix` and `size`) / `RemoteAlbum` / `RemoteArtist` / `RemotePlaylist` structs (asynchronous: a request returns a `RequestId`, its callback runs from `poll()` on the UI thread, and `cancel(id)` drops it). `App` takes a `MusicServer&`: the Streaming menu asks it for lists, `tick()` calls its `poll()`, and opening a remote track asks it for `stream(id, maxBitRate)`: the URL, and for a transcode a `Transcoded` (offset URL, and the length `App` fills in from the listing) that `MediaPlayer::open()` takes. |
| `app/net/http_client.{h,cpp}` | `Http`: asynchronous GET whose handler runs on the client's thread (tests use a fake). `HttpClient`: libcurl multi on one worker thread; 10 s connect timeout, 10 s without data or 60 s in all ends a request, answers over 16 MB fail; cancellable; the destructor aborts transfers and never waits on a socket. |
| `app/net/subsonic.{h,cpp}` | `Subsonic : MusicServer`: API 1.16.1, token auth (`u`, `t` = md5(password + salt), a fresh random `s` per request, never `p`), percent-encoded URLs, `stream()`: the original file with no `format` / `maxBitRate`, or an MP3 transcode (`format=mp3&maxBitRate=<kbps>`) whose offset URL adds `&timeOffset=` once `getOpenSubsonicExtensions` (asked with the first transcode) lists `transcodeOffset`. `scrobble()` sends `scrobble?id=&submission=` and only logs a failure to stderr (by its kind, never the URL). `downloadUrls(Config)` gives the `download?id=` URL of a track for `HttpDownloader`. `configFrom(Settings)` reads `server.*`. Answers are parsed on the HTTP thread; callbacks run from `poll()`. |
| `app/net/subsonic_json.{h,cpp}` | Pure parsers for ping, getAlbumList2, getArtists, getArtist, getAlbum, getPlaylists, getPlaylist and getOpenSubsonicExtensions answers, and `parseStatus()` for an answer with only a status (scrobble) (yaml-cpp). Error 40 is `BadLogin`, other Subsonic errors and non-Subsonic bodies `Server`. `jsonForYaml()` rewrites `\uD83C\uDFB5` surrogate pairs, which yaml-cpp 0.8 rejects. |
| `app/net/http_downloader.{h,cpp}` | `HttpDownloader : Downloader`: libcurl on its own worker thread (apart from `HttpClient`, so a long download never delays a list request), one download at a time in queue order. The URL comes from `Subsonic::downloadUrls()` (`download?id=`, same token auth as `stream()`); nothing here logs or reports it. Writes `<dest>.part` (flushed per chunk) and renames it to `dest` only when curl reports the transfer complete; folders are created. A `.part` is continued with `Range: bytes=N-`; a 200 answer (no ranges) or 416 starts the file over. An HTTP error, or a text / JSON / XML body with status 200 (a Subsonic error), is `Server` and leaves the `.part` as it was; a broken connection or 10 s without data is `Unreachable` and keeps it; a write or folder failure is `Disk`. Progress (latest only) and the result are delivered from `poll()`; `cancel(id)` stops the transfer and its callbacks never run. The destructor wakes the transfer and returns at once. |
| `app/net/md5.{h,cpp}` | `md5Hex()` for the auth token. |
| `app/media/player.{h,cpp}` | `Player`: one audio file or stream URL, decoded on its own thread (FFmpeg). A local file opens in `open()`; a URL opens on the decoder thread (`opening()` until it is done), with a 10 s read timeout and HTTP reconnects, and the whole open gives up after 10 s (`kStreamOpenTimeout`). `preload()` opens the next track's stream the same way in the background, and `open()` of that URL takes it over. A stream whose connection is lost while it plays is retried with backoff at the same place for 30 s (`kStreamRetryGiveUp`); `buffering()` says when it has run dry meanwhile. A transcode (`open()` with `Transcoded`) has no byte ranges: a seek, a retry and an open at a place open it again (see Streams in section 4), and its length is the server's. An interrupt callback lets `close()` abort any network wait at once, and `close()` or a newer `open()` drops an open still running. Its error messages print a local path but never a URL (`://`), which would hold the server login, and FFmpeg's own messages pass through `redactUrls()`. |
| `app/media/tts.{h,cpp}` | `Tts`: eSpeak NG on a worker thread. |
| `app/core/earcons.{h,cpp}` | `Earcons`: UI sounds made from sine tones. |
| `app/core/mixer.{h,cpp}` | `Mixer`: the three streams mixed into the output. |
| `app/core/audio_stream.{h,cpp}` | `AudioStream`: thread-safe PCM FIFO with flush epochs. |
| `app/core/library.{h,cpp}` | `Book` (also a remote list: the server's track ids in `files`, server titles in `titles` and lengths in `durations`, `remote` set), `scanLibrary(root, skip)`, which leaves out the cache folder `skip`. |
| `app/core/track_cache.{h,cpp}` | `TrackCache`: downloaded tracks of one server in `<cache>/<server key>/<kind>%3A<list id>/`, where a list is an album or a playlist (the kind in the folder name keeps an album and a playlist with one id apart). Each list folder has the audio files and a `manifest.tsv` (header `oap-cache 2`, then a line `list<TAB>album|playlist<TAB>id<TAB>name<TAB>artist`, then per track tab-separated: id, file, title, duration, a reserved `changed` column (always empty: a download is never compared with the server), pinned `0`/`1`, last-played wall time; `\\`, `\t`, `\n`, `\r` escaped), read and written through the `TextFiles` interface of `services.h` (core does no file I/O of its own; `DiskTextFiles` in `app/config/` writes with `writeFileAtomically()`: temp file, `fsync`, rename, mode 0600; tests use the same on a temp folder). If `save()` finds a manifest that is there but not valid, it deletes the files in the folder that the new manifest does not name (never a `.part`, never another folder), so a damaged manifest leaves no orphans; a manifest that cannot be read at all (an I/O error) is not replaced (`save()` fails). A missing or corrupt manifest reads as empty, and so does a manifest of format 1 (it had no list line; no device ever held one) or one whose list line does not match its folder (a copied folder). `lists()` gives the lists with at least one complete track, sorted by name (`lists(true)`: only those with a complete pinned track, for Streaming > Downloaded); `trim(limit, played, keep, info)` writes play times into `lastPlayed` (the latest play replaces the old time; a time before `kEarliestPlayTime`, 2020-01-01, is a clock that was never set and is ignored) and evicts unpinned tracks and `.part` files (a `.part` first, then least recently played first), above the limit; `info` gives the evicted ids and the bytes left (section 5, "Automatic cache"); `remove()` deletes one list folder, only ever a direct child of the server's folder. `find(trackId)` returns the complete file of a track from any list (`index()` does it for every track in one pass, `completeFile()` for one manifest entry), never a `.part` file, a missing or empty file or a file named outside the folder. The server key is the host plus a hash of the URL (no login, no trailing slash) and the user name; list ids and the key are made filename-safe (`safeName()`), never holding a password or token. `App` takes the cache root, leaves it out of Library and Music, and plays from it (section 5, "Play from the cache"). |
| `app/core/library_index.{h,cpp}` | `LibraryIndex` (not used by `App` yet): the library in memory, `IndexItem` (id, source local/server/cached, kind book/music/podcast, title, artist, album, genre, year, MusicBrainz id, first-seen and last-played wall times, tracks, `seenOn`) with `IndexTrack` (location, title, artist, number, duration, size, file time, `seenOn`). `seenOn` holds server references `<server key>:<album\|playlist\|track>:<id>` (`seenRef()`; `refOnServer()` finds the one for a server key), each at most once. Lookups by id, by track location, and by reference. `upsert()` merges (keeps first-seen, later last-played, and the references already there), `replaceSource()` is a scan, `replaceServer()` a sync of one server key. It loads and saves through `LibraryStore` in `services.h` (`load()`; `save(changed, removed)`), and `save()` hands over only the items that changed and the ones removed, nothing when none did. The SQLite store comes in step 4.3. |
| `app/core/settings.{h,cpp}` | `Settings`: dotted keys in memory, loaded and saved through a `SettingsStore`; the `state.tsv` line format; `serverConfigured()`. |
| `app/config/stores.{h,cpp}` | `YamlStore` (`config.yaml`, nested YAML ⇄ dotted keys) and `TsvStore` (`state.tsv`). |
| `app/config/atomic_file.{h,cpp}` | `writeFileAtomically()`: temp file with mode 0600, `fsync`, `rename`. |
| `app/config/disk_text_files.h` | `DiskTextFiles`: the `TextFiles` of the real file system, for the cache manifests (`App` takes it with the cache root; without one the cache is off). |
| `app/config/settings_files.{h,cpp}` | `SettingsFiles`: what a front end hands to `App` (both files, loaded). |
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
| `app/frontend/no_music_server.{h,cpp}` | `NoMusicServer`: the `MusicServer` the front ends give `App` when `config.yaml` sets up no server they can use (no `server.url`, a `server.type` other than `subsonic`, or a build without `OAP_BUILD_NET`); every request finishes with `NotConfigured` on the next `poll()`. |
| `app/frontend/no_downloader.{h,cpp}` | `NoDownloader`: the `Downloader` for the same cases; every download finishes with `NotConfigured` on the next `poll()` and writes nothing. |
| `app/frontend/make_music_server.{h,cpp}` | `makeDownloader()` (an `HttpDownloader` for a Subsonic server, else `NoDownloader`) and `makeMusicServer()`: the `MusicServer` both front ends give `App`: `Subsonic::fromConfig()` when built with `OAP_BUILD_NET` (`OAP_HAVE_NET`), else `NoMusicServer`, with a line on stderr when `config.yaml` sets up a server it cannot use. Its own library, `oapmusicserver`, so `oapfrontend` keeps no dependencies. |
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
| **Decoder** | `Player` | Opens a stream URL, then reads, decodes and resamples into the content stream. Blocks while the stream is full, or while a stream waits for data or retries after a dropout; `close()` aborts either at once. A thread still opening when it is dropped is left to end by itself (see Streams in section 4). |
| **Preload** | `Player` | Opens the next track's stream during the last 10 s of a remote track (read-ahead), next to the decoder thread. An `open()` of that URL takes it over; if it is still opening, it carries on as the decoder thread. Dropped like an open (Preload in section 4). |
| **TTS worker** | `Tts` | Runs `espeak_Synth()` synchronously and resamples into the speech stream. |
| **Audio callback** | `AudioOutput` | Calls `Mixer::render()` to pull PCM for the device. CoreAudio calls back; Pulse, ALSA and null run their own writer thread. |
| **HTTP worker** | `HttpClient` | Runs every libcurl transfer and parses the answers (`Subsonic`). Finished results go into a mutex-protected inbox that `Subsonic::poll()` empties on the UI thread. |
| **Download worker** | `HttpDownloader` | Fetches one track file at a time into its `.part`. Progress and results go into a mutex-protected list that `HttpDownloader::poll()` empties on the UI thread. |

Rules:
- `App` is single-threaded. Front ends must not call it from other threads.
- `App` never waits on the network. A `MusicServer` request returns a
  `RequestId` at once; `App::tick()` calls `server.poll()`, which runs the
  callbacks of finished requests on the UI thread. Leaving a screen calls
  `cancel(id)` on its request, so its callback never runs (section 7).
  Opening a stream is asynchronous too: `Player::open()` returns at once,
  and `App::tick()` sees the result when `opening()` turns false (section 8).
  So is the read-ahead: `preload()` returns at once. Scrobbles go out like
  any other request, with no callback.
- Threads exchange data only through `AudioStream` (mutex-protected), atomics,
  and mutex-protected queues: the `Tts` text queue, the `HttpClient` job queue,
  the `Subsonic` inbox of finished requests and the `HttpDownloader` list of progress and results.
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
  until it reaches the exact target time. If FFmpeg cannot seek, playback goes
  on and the position is that of the audio actually queued. A start position
  past the end is clamped to the duration.
- **Format changes:** a file may change sample rate, sample format or channels
  mid-stream (concatenated MP3s). Each frame is checked against the format the
  resampler was built for, and the resampler is rebuilt when it differs.
- **Read errors:** only `AVERROR_EOF` ends a file. Other `av_read_frame` errors
  are retried every 10 ms; after 50 in a row the file is treated as ended.
  (FFmpeg's Ogg demuxer rejects chained streams that change format this way.)
- **Streams:** any URL (`://`) is opened on the decoder thread: `Player::open()`
  returns at once with `opening()` true, and the open gives up after
  `kStreamOpenTimeout` (10 s) for `avformat_open_input` and
  `avformat_find_stream_info` together. The thread opens into its own `Input` and
  hands it to the `Player` (fields first, then `state_` = Open, which the UI thread
  reads) only if the open was not dropped meanwhile; each open has an `OpenJob`
  shared with its thread. FFmpeg's interrupt callback checks the job's deadline and
  `stop`, so `close()` and the destructor never wait on a socket; the decoder thread
  notices within FFmpeg's 100 ms poll. `close()` (also run by a newer `open()`)
  marks a job still opening as dropped and detaches its thread instead of joining
  it, so even a wait no interrupt reaches cannot hold up the UI; that thread then
  frees what it opened and never touches the `Player` again. A local file is still
  opened in `open()` on the calling thread. Options: `rw_timeout` and `timeout` 10 s (a read or connect with no
  data fails), `reconnect` with `reconnect_delay_max` 0 (FFmpeg reconnects a
  dropped connection once, at once, at the same byte). Not `reconnect_on_network_error`, so an unreachable
  server fails the open at once, and not `reconnect_streamed`, which would restart a
  stream without a length from the beginning. DNS lookups (`getaddrinfo`) are
  not interruptible and are not covered by the cap.
- **Preload (read-ahead):** `preload(url)` opens a stream on a thread of its own
  with an `OpenJob` that has no owner yet, next to what plays. Done, the thread
  leaves the opened `Input` in the job (or nothing, if it failed) and ends. An
  `open()` of that URL takes the job over: a finished preload starts decoding at
  once (`isOpen()` on return, as for a local file); one still running gets the
  `Player` as its owner and its thread carries on as the decoder thread, as if
  `open()` had started it. A preload that failed, before or after the takeover, is
  opened once more as a normal open. One preload is kept; `open()` of another
  path and `close()` leave it, and `cancelPreload()`, a preload of another path
  and the destructor drop it like an open (its thread is detached and frees what
  it opened; an `Input` left in the job is freed with it).
- **Dropouts:** a read error of a stream that is a network error (FFmpeg's
  reconnect failed) does not end it. The decoder thread keeps the queued audio
  playing and retries: it waits 1 s, then twice the last wait up to 8 s
  (`kStreamRetryFirstDelay`, `kStreamRetryMaxDelay`), in 10 ms steps so that
  `close()` and a seek do not wait, and then seeks to where the queued audio ends
  (`endPts_`; `skipUntil_` drops what the seek gives before it), which makes FFmpeg
  ask the server for the rest with a Range request. Every read error counts as
  part of the dropout until bytes from beyond the place it was lost
  (`AVIOContext::pos`) have come, since FFmpeg's read-ahead still holds older
  data. Once `kStreamRetryGiveUp` (30 s, a constructor argument for tests) of
  playing time has passed since the loss (time paused does not count, and a
  seek that works starts it again), no retry starts and the stream has failed:
  once what came has been heard, `failed()` is true and `finished()` stays false.
  The read timeout that notices a stalled connection and a connect that hangs
  come on top, so "Server not reachable" can take longer than 30 s. A local
  file never fails this way. `buffering()` is true while a stream plays, has not
  ended or failed, and nothing is queued (also while a seek waits): it has run dry.
- **Transcodes** (Streaming quality, step 2.4): the server makes the MP3 as it
  sends it, so it has no byte ranges and no length of its own. `open(url, at,
  Transcoded{offsetUrl, duration})` makes `Player` go to a place by opening the
  stream again (`reopenInput()` on the decoder thread, which `seekInput()` uses for
  a user seek and a dropout retry alike): `offsetUrl` plus whole seconds
  (`timeOffset`), with the audio up to the exact place dropped (`skipUntil_`) and
  timestamps moved by where the input starts (`inputStart_`); with no offset URL
  (a server without `transcodeOffset`) the stream from its start, decoding and
  dropping everything before the place. An open at a place opens the offset URL
  at once, so a preload (from the start) is taken over only by an open from the
  start. `duration()` is the length the server lists. FFmpeg reports the end of a
  transcode as a read error, like a lost connection, so only an error more than
  `kTranscodeEndSlack` (2 s) before that length is retried; without a length the
  error is the end. A reopen that the server answers with no audio (Navidrome
  past the end of a track listed too long), or one that opens and then brings
  nothing, means the track has ended: a seek there ends it too.
- **FFmpeg log:** `Player` installs an `av_log` callback that prints errors only
  and cuts every URL to its scheme and host, and every other `?query`, since some
  FFmpeg messages print URLs or request lines.
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
          a stream played last: [Home], "Now playing" first, nothing loaded

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
- **Sleeping** simulates suspend: playback is paused, a stream open or resume
  still loading is dropped, and the position (or the stream's place) and any
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
├── Now playing: <book or album>   (when something is loaded, or a stream to resume: `last_stream.*`)
├── Library → <books>              (OK plays/resumes)
├── Music
│   ├── Play all                   (every track, albums in order)
│   ├── <loose track>              (OK plays it)
│   └── <album> → <tracks>         (OK plays from that track to the end of the album)
├── <server.name, default Server>  (Streaming: lists from the server, see below)
│   ├── Recently added → <albums>  (one page of 500)
│   ├── Albums A to Z → <albums>   (pages of 500, the next one fetched while the server has more)
│   ├── Artists → <artist> → <albums>
│   ├── Playlists → <playlist> → Play playlist · Download playlist · <tracks>
│   ├── Random albums → <albums>   (one page of 50)
│   ├── Downloaded                 (only with a cache folder; built from the manifests, not the server: <name> → Play album|Play playlist · Delete download → [Cancel, Yes, delete] · <tracks>)
│   └── Server status              ("Checking the server.", then "Connected to Navidrome 0.64.2." or the error)
│       <album> → Play album · Download album · <tracks>   (Download only with a cache folder and a downloader; OK plays from that track to the end, via playRemote())
│       No server: one item, "No server set up" (OK says how to add one);
│       a server this build cannot use (another type, or no OAP_BUILD_NET): "Server not supported"
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
│   ├── Streaming quality original file / 320 / 192 / 128 kbps (an MP3 transcode for a slow link; from the next track)
│   ├── Cache size       off / 1 / 4 / 16 GB (automatic cache of streamed tracks; default 1 GB; a lower size is applied on the next exit; off keeps what is cached and caches nothing new)
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

**Lists from the server** (`App::openRemoteList`) open at once with one item,
"Loading", and the request goes out. Each screen has an `id`; the request's
callback holds that id, never a `Screen` pointer, and finds its screen with
`findScreen()`. Every way off the stack goes through `closeScreens()`, which
cancels the screen's request, so a late answer never runs. When the answer
comes (from `server.poll()` in `tick()`), the items are filled in; if the user
is still on "Loading" and the screen is on top, the first item is spoken,
else the list is filled silently. An error becomes the error and "Try again";
an empty answer says why. Such a screen has no `build`: returning to it keeps
what has loaded. With a page size (Albums A to Z), `onPage()` asks for the next
page after what came while the server says `more`. Server status is not a list:
`checkServer()` keeps one ping, cancelled when another screen is pushed or its
screen closes. The user-visible rules are in [navigation.md](navigation.md#lists-from-the-server).

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
- **Remote tracks** (see [streaming.md](streaming.md)): `App::playRemote(title, tracks, index)`
  turns a list of `RemoteTrack`s into a `Book` with `remote` set, `files` = the track ids,
  `titles` = the server titles and `durations` = their lengths. Each track's stream comes from
  `server.stream(id, streaming.bitrate)` when it is opened (`remoteStream()`), so a change of
  Streaming quality takes effect with the next track; a transcode is opened with its
  `Transcoded`, its length taken from the listing. The list plays like music (`playMusic` → `switchTo(..., music=true)`),
  so it saves no `pos:` key and leaves `last_book` alone. Every spoken part name comes from
  `partTitle()`: the server title, or "Track 3" if it has none, never the URL, which holds the login.
- A remote track that cannot be opened says "Cannot play Rain. Server not reachable." and **no
  other track is tried** (each try could block for the network timeout). What was current
  before stays current and paused at the same place. If that is a remote track it is **not
  opened again at once** (it would wait on the same dead server): `reopenAt_` holds its
  position and Play opens it. When the next track fails at the end of one, the track that
  failed becomes current at 0 instead, so Play does not reopen the one just heard. At the end
  of a remote list the first track is likewise opened only on Play. A local book that cannot
  be opened leaves a remote track that was playing open and playing.
- A stream that **runs dry** while it plays (`MediaPlayer::buffering()`: a slow
  network, or a dropout that `Player` retries) gets the `Buffering` earcon once it
  has been dry for `kDryBeforeEarcon` (0.5 s, so the moment after a seek while the
  new part loads is quiet), and "Waiting for network." once after
  `kDryBeforeWaitingForNetwork` (3 s), said only when no other speech is playing
  (`App::checkBuffering()`, from `tick()`, timed with `clock_`). The track is not
  changed; when audio comes again it goes on. The dropout is over once audio has
  played for `kPlayingBeforeRecovered` (2 s) without a gap, so a stream that
  stutters on a slow link gets one earcon and then the phrase; the next dropout,
  and Play after a pause, get them again.
- A stream that **breaks off** mid-track (`MediaPlayer::failed()`: Wi-Fi off or the
  server gone, and `Player`'s retries gave up) says
  "Server not reachable. Press play to try again." once the buffered audio has been heard
  (`App::onStreamFailed()`). The track stays current, closed, with `reopenAt_` at the place
  it got to; the next track is not tried. While a remote track is closed, "where am I" and
  Album info leave out the time left, which the closed player does not know.
- **A remote track opens in the background** (`MediaPlayer::opening()`), and `App`
  stays responsive meanwhile. `openFile()` returns `Pending` and keeps the open in
  `pendingOpen_`: the track is already `book_` / `fileIndex_`, and the open holds
  what to do when it has opened (`opened`, such as entering the player) and how to
  go back to what was loaded before (`undo`). `tick()` finishes it once `opening()`
  is false: it plays, or says "Cannot play Rain. Server not reachable." and runs
  `undo`, exactly as a failed open did in phase 1. A local file opens at once and
  takes the same paths straight away.
- `openFile()` says "Loading." before every remote open the user asked for
  (`Origin::Menu`, `Key` or `Resume`, Play opening a closed track again). A track chosen in a menu keeps the user there (a list
  of tracks, the Player menu) until it has opened; then the player opens if they
  are still on that screen. Play/Pause meanwhile decides whether it plays once
  open ("Paused." / "Loading."), and "where am I" says "Loading" instead of "Paused".
- **A pending open is dropped** (the player is closed and `undo` runs, silently)
  when the user leaves the menu it was chosen in (Home, Back), chooses another
  track, the device goes to sleep or powers off, so a late result never plays or
  speaks. A newer open of another track of the same book (`loadFile()`: a skip,
  the next track, a chapter chosen) drops the pending one without undoing it, and
  keeps its `undo` to run after its own if it fails too: the pending open may be a
  `switchTo()` whose undo loads the book before, which must not happen first. An
  `undo` that leaves nothing loaded (a resumed list that failed) also leaves the
  player screen, if the user had entered it. A skip or Play in the player
  (`Key`), the next track at the end of one (`Next`, quiet) and reopening what
  played before a failed choice (`Restore`, quiet even when it fails) belong to
  no screen. If the player has not finished
  after `kStreamOpenGiveUp` (15 s, which covers a DNS lookup that FFmpeg cannot
  interrupt), `App` closes it and reports the track as not reachable. Automatic
  sleep and the end-of-track sleep timer wait while a track opens.
- **Play from the cache:** `openFile()` opens a remote track from its complete
  cached file when `App::cachedFile()` finds one (the `TrackCache` that `trackCache()`
  makes once, from the cache root and the `server.url` / `server.user` in the config: the
  server cannot change while the player runs and a factory reset keeps `server:`, so
  another server's downloads are never used), else from `remoteStream()` as above.
  The file is a local path: it opens at once (no "Loading."), seeks by itself and
  has no transcode offset, whatever the Streaming quality. A file that is gone is
  not found, and one that fails to open falls back to the stream. Read-ahead
  pre-opens nothing for a cached next track (it is a file). A cached track is
  scrobbled like a streamed one (it is a play on the server's account; offline
  the call is lost, as for any scrobble). The menus do not change.
- **Cache index:** `cachedFile()` is called once per tick near the end of a
  track, so it never reads the disk by itself. It keeps `TrackCache::index()` (track
  id to file, for the server in the config) in memory, built at the first call and
  updated when a download saves a track (and when one drops a track). The open of a track passes `verify`, which
  also checks that the file is still there. A file added to the cache by anything
  but `App`'s own downloads is not seen until the index is rebuilt (Delete download
  resets it; a finished download and a cached track add to it, an eviction or a dropped track
  erase their entries).
- **Downloaded (3.5):** Streaming > Downloaded (`downloadedMenu()`, a menu with a
  `build`, so it is rebuilt when the user comes back) lists `TrackCache::lists(true)`:
  the lists of the server in the config that have a complete pinned track (what the user
  downloaded; lists of only automatically cached tracks are not shown). Each item opens
  `downloadedListMenu()`, built from the manifest and the files on disk: "Play
  album/playlist" first, "Delete download", then the complete tracks (the items of
  `trackItems()`, with Delete in place of Download); they play with
  `playRemote()` and the list's `RemoteSource`, so a cached file opens (section
  "Play from the cache") and `last_stream` is saved like for a server list. Delete
  download opens a list of "Cancel" (selected) and "Yes, delete". `deleteDownload()`
  refuses with a spoken reason when the list is the book loaded in the player; it
  cancels the download when that list is downloading; then `TrackCache::remove()`,
  `cacheIndex_.reset()`, two `popScreen()` (the rebuilt Downloaded list shows what is
  left) and "Deleted Weather.". The folder name keeps an album and a playlist apart;
  the manifest keeps the name and artist.
- **Offline (3.6):** `showStatus()` adds a "Downloaded" item after "Try again" in an error list when `hasDownloads()` (a cache folder and `TrackCache::lists(true)` not empty); it opens `downloadedMenu()`, which never asks the server. "Server status" is not changed: it sits in the Streaming menu, which already has Downloaded. `completeTracks()` builds the playable tracks of a manifest and is shared by `downloadedListMenu()` and `resumeStream()`. Scrobbles of a cached track are still sent; they are fire and forget and silent, so a failure offline says nothing and blocks nothing. A cached next track is never read ahead (`checkReadAhead()`), so offline play of a cached album touches no network.
- **Automatic cache (3.7):** `checkAutoCache()` (in `tick()`, after `checkScrobble()`)
  queues the streamed track that plays once it has been heard for `kAutoCacheAfter`
  (10 s of listening, the same count as scrobbling: paused or dry time does not count,
  so a skipped track costs no bandwidth), once per play, unless `cache.limit_gb` is 0.
  The job holds the list's `RemoteSource`, name and artist (`Book::artist`) and the
  track's title, length, `suffix` and `size` (`Book::suffixes`, `sizes`, set by
  `playRemote()`). `pumpAutoCache()` starts one transfer at a time into the list's
  folder, only while no explicit `download_` runs, the downloader works and the device
  is awake, and drops jobs for tracks that are on disk by then. A track bigger than the
  limit is never queued (it would be evicted at once and fetched at every play), and a
  track that would leave less than `kAutoCacheReserve` (256 MiB: state, config and
  manifest writes on a small card, with margin for an unlisted size) free is skipped
  silently (`setFreeSpace()` as for explicit downloads). `sleep()` calls `pauseAutoCache()` too (nothing is fetched
  for the cache while the device sleeps; the job continues from its `.part` after the wake). An
  explicit download is not paused by sleep and ends silently. `startDownload()` calls `pauseAutoCache()`:
  the running transfer is cancelled (its `.part` stays and continues later), its job
  goes back to the head of the queue, and the queue waits until `download_` is gone.
  Download progress ("Downloading, 3 of 9 tracks") counts only the download's own list
  and is never spoken for the automatic cache. The cache is silent: no speech, no
  earcon, a failed transfer is forgotten (not retried during that play). `onAutoCached()`
  adds the track to the list's manifest **unpinned** with
  `lastPlayed` = the time the play began (an existing list keeps its name, a pinned
  entry stays pinned), updates `cacheIndex_` and calls `trimCache(false)` (evict only); the merge is
  `addToManifest()`, shared with `onTrackDownloaded()`, and the destination is
  `trackDestination()`, shared with `fetchNextTrack()`. Deleting a list
  drops its queued and running jobs.
  **LRU:** `trimCache()` calls `TrackCache::trim()` with `cache.limit_gb` times
  `cacheGigabyte_` (1 GiB; a test sets a small one). It evicts the unpinned complete
  files, least recently played first (never played counts as oldest), until they add up
  to at most the limit; **pinned tracks never go and are not counted**. `.part` files
  count and go first, except those of the running and waiting transfers (`partsInUse()`:
  the explicit download's rest, the auto job and queue), so a `.part` left by a failed or
  dropped transfer is cleaned when the cache is over the limit. The file the player has
  open is kept. A list left with no tracks loses its folder, unless a kept `.part` is in
  it. **Off (0) evicts nothing**: what is cached stays and only nothing new is cached
  (plays are still written to the manifests). It runs after a track was cached and in
  `saveOnExit()` (exit and sleep), so a lowered size takes effect then, not while the
  user steps through the values in Settings. It scans the manifests once, never from a
  key or a tick, and only when there is a reason: plays to write, `trimDirty_` (a
  transfer was abandoned), or `cacheBytes_` (the bytes after the last scan plus the
  tracks cached since) unknown or over the limit. Evicted ids are erased from
  `cacheIndex_`, which is not rebuilt. The second `saveOnExit()` at power off (the front
  end's `shutdown()`) finds nothing to do.
  **lastPlayed:** `startTrackPlay()` records the wall time (`clock_.wallTime()`) of a
  cached track that starts in the in-memory `played_` map; a newly cached track gets the
  time its play began. `trim()` writes `played_` into the manifests, and only where it
  changed, so the flash is written when a track was cached (its own list) and on exit or
  sleep, never at a play. The latest play replaces `lastPlayed` (it is not a maximum, so one play with a
  clock that was ahead cannot pin a track). A wall time before 2020-01-01 is a clock
  that was not set: not recorded (a track cached then has `lastPlayed` 0, "never
  played", and goes first).
- **Downloads (3.4):** `trackItems()` adds "Download album" / "Download playlist"
  after the Play item when `canDownload()`. `startDownload()` reads the list's
  manifest, skips (and pins) the tracks whose files are whole, checks the free
  space of the cache root against the sizes of the rest (`App::setFreeSpace()`,
  default `std::filesystem::space()`; an unknown size counts as 0, a failed query
  as room enough) and holds one `Download` in `download_`. It asks the
  `Downloader` for one track at a time into `<list folder>/<safe id>.<suffix>`; each
  `Ok` (whole by the downloader's own check, see `Downloader` in `services.h`; `App`
  compares no sizes) is added to `manifest.tsv` pinned, and updates the cache index. The
  manifest then follows the server's order, and pinned tracks the server no longer lists
  are dropped with their files (`followServerOrder()`); a track listed twice counts once. A failure is spoken once and drops the `Download`; OK on the item starts again
  and skips what is whole, the `.part` continuing. The download belongs to no screen,
  so leaving menus never cancels it; `tick()` calls `downloader_.poll()`, and
  `shutdown()` cancels the transfer (the `.part` stays). `whereAmI()` adds
  `downloadProgress()`. The end is spoken, or an earcon while something plays.
- **Read-ahead:** while a remote track plays its last `kReadAhead` (10 s, by the
  player's position and length), `App::checkReadAhead()` (last in `tick()`) asks
  the player to `preload()` the next track of the list, once; opening that track
  (at the end of this one, or a skip) then takes the pre-open over and starts
  without "Loading" or a wait. If the pre-open failed, the open is a normal one
  (pending, as above). A pause (sleep and power off pause too), a seek back out
  of the last 10 s, another book or list, or a track of unknown length (a
  transcode the server lists no length for) means no pre-open: `cancelPreload()`
  drops one that was made. The last track and local books pre-open nothing.
  `Player` takes a pre-open over only for the same URL, and each URL from the
  server has a fresh salt, so `readAhead_` keeps the stream it pre-opened (with
  the track id and the quality), and opening that track at that quality uses it.
  A change of Streaming quality during the pre-open pre-opens the new one.
- **Scrobbling:** each play of a remote track (`scrobble_`) tells the server
  `scrobble(id, false)` ("now playing") once it plays, not when it is chosen and
  not for an open that fails, and `scrobble(id, true)` once it has been heard for
  half its length (the player's, else the server's) or `kScrobbleAtMost` (4 min),
  whichever comes first (`App::checkScrobble()`, from `tick()`). Heard time is
  counted with `clock_` while the player plays and is not dry, not read from the
  position, so a pause, a dropout or a seek forward does not count. Opening another
  track, the next one at the end, or going back to the start of the track (Previous,
  or choosing it) starts a new play; Play opening the same track again after it broke
  off (`Origin::Resume`) or after a failed choice (`Restore`) goes on with the same
  play. The end of a track ends its play, and a local book ends it too: local files
  are never scrobbled. `scrobble()` returns nothing, so a failed scrobble is never
  heard; `Subsonic` logs it to stderr by its kind, never with the URL.

## 9. Persistence

`App` takes two `Settings`: `config` and `state`. Each is a flat map of dotted
keys in memory. Core never sees a file: `load()` and `save()` go through a
`SettingsStore`, which the front end gives it (`SettingsFiles`), and the tests use
`FakeStore` or no store at all.

| File | Store | Keys |
|------|-------|------|
| `config.yaml` (`--config`; simulator `~/.config/oap-sim/`, headless `~/.config/oap/` or `$XDG_CONFIG_HOME/oap/`) | `YamlStore` | `speech.rate`, `volume`, `verbosity`, `skip.back`, `skip.forward`, `rewind_on_resume`, `buttons.long_press_ms`, `menu.wrap`, `power.auto_sleep_min`, `streaming.bitrate` (kbps, 0 = the original file), `cache.limit_gb` (0, 1, 4 or 16; default 1), and `server.*` (see [streaming.md](streaming.md)) |
| `state.tsv` (`--state`, default next to `config.yaml`) | `TsvStore` | `last_book`: the book to resume on start (never music); `pos:<book path>`: `"<file index>:<seconds>"`; `last_stream.server` / `.user` (the `server.url` without a trailing slash and `server.user`, never the password or a stream URL), `.kind` (`album` / `playlist`), `.id`, `.title`, `.track` (index), `.track_id`, `.pos` (seconds): the remote list that played last |

- `config.yaml` is nested YAML (`speech: {rate: 220}` is `speech.rate`). It is
  written only when a setting changes in the Settings menu (on leaving the item,
  or at once when chosen from the list), and on exit if the player volume changed.
  Writing drops comments; the file gets a header naming the keys, and keys in a
  fixed order. Numbers are written plain and everything else quoted, the password
  always. A file that does not parse, or is there but cannot be read, is reported
  on stderr, the defaults are used, and it is never overwritten. A `config.yaml` or
  `state.tsv` that is a symbolic link stays one: the file it points to is replaced.
- **State is written only on exit and on sleep**, to spare the flash: Power off,
  Restart, Sleep and `App::shutdown()` (the front ends call it on SIGTERM / SIGINT /
  SIGHUP and when the window closes). Pause and a book change only update the map
  in memory, so switching back to a book still resumes it. The file is written
  only when the map differs from what was last loaded or written (a write that
  failed counts as not written, so the next exit tries again). A crash or
  power loss loses the place since the last exit.
- **Which place start() resumes:** the one that played last. `rememberPosition()`
  keeps the place of a remote list from the Streaming menus (`Book::source`, the
  album or playlist id) in `last_stream.*`; `openBook()` removes those keys, since a
  local book is then the last played. Local music changes neither. If
  `savedStream()` finds a place for the server set up now (same url and user, and
  `MusicServer::configured()`), `start()` opens Home with "Now playing" first and
  loads nothing; otherwise `last_book` resumes as before. OK on it, or Play with
  nothing loaded, runs `resumeStream()`: "Loading.", `albumTracks` /
  `playlistTracks` (`resume_`; a new screen, sleep, Power off and `shutdown()` cancel it), then
  `playRemote(..., source, position)` with the track found by id (`playResumed()`).
  The answer's error is spoken with `errorText()`.
  **Resume from the cache (3.6):** `resumeStream()` first reads the list's manifest.
  A download that is whole (every manifest track pinned and complete on disk, the
  saved track among them) plays at once with `playResumed()`: nothing is asked of
  the server and "Loading." is not said, because offline the question would wait for
  the connect timeout (10 s). Otherwise the server is asked first (a partly cached
  album, e.g. from the automatic cache, would lose its other tracks; automatically cached
  tracks are unpinned, so they never make a "whole download"); on an error,
  if any track of the list is on disk, those play (the saved track and position if it
  is among them, else the first from its start) and nothing is spoken but the usual
  "Now playing". With none on disk the error is spoken as in step 2.6. A whole
  download is not compared with the server, so a list changed there is not seen until
  it is played from a server list.
- Up / Down in the player change the volume in memory; it is written on exit.
- **Factory reset** clears both, but keeps the `server.*` keys, and restarts
  without saving the position first, so it does not come back.
- Every write is atomic (`writeFileAtomically`): `FILE.tmp` created with mode 0600,
  `fsync`, `rename`, then `fsync` of the folder. A power cut leaves the old file.
- The old single `settings.tsv` is not read any more; settings start from the defaults.

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
- **Arguments:** `oap-sim [--config FILE] [--state FILE] [--music DIR] [--cache DIR] [--audio NAME] [--script STEPS] [LIBRARY_DIR]`.
  `--audio` is `auto` (default), `coreaudio`, `pulse`, `alsa[:DEVICE]` or `null`, among those built in.
  `tools/make_sample_library.sh [LIBRARY_DIR] [MUSIC_DIR]` makes a spoken sample library and some tone "music".
- **Script mode:** `--script "ok down ok wait:3000 right:long play:long quit"`
  presses buttons automatically, for repeatable walkthroughs and future UI tests.
  Steps: `play left right up down ok home talk power`, with `:long` for a long
  press, plus `wait:<ms>` and `quit`. An unknown step or a `wait` without a number is printed and skipped.
- **Several inputs at once:** keyboard, pads, on-screen buttons and `--script` share one
  `HeldButtons`, so a button held from two of them is released only when both let go.

## 11. Headless front end (`app/headless/`) and the Linux device

- **Arguments:** `oap-headless [--config FILE] [--state FILE] [--music DIR] [--cache DIR] [--audio NAME] [--input LIST]
  [--evdev-device PATH] [--script STEPS] [LIBRARY_DIR]`. The config defaults to
  `~/.config/oap/config.yaml`, the state to `state.tsv` next to it (section 9), the cache of downloaded tracks to `cache/` next to the state.
- **Inputs** (`--input evdev,terminal,none`; default `evdev` without a tty; on a tty the CMake
  setting `OAP_HEADLESS_TTY_INPUT`: `terminal`, or `evdev,terminal` in the Pi toolchains, since
  a desktop would see each key twice):
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
| Paths | Library under `/data/library`, `config.yaml` and `state.tsv` under `/data/config` (`--config /data/config/config.yaml`), the download cache in `/data/library/cache` (`--cache /data/library/cache`; without `--cache` it is `cache/` next to `state.tsv`). `scanLibrary()` skips it. |

Nothing in `app/core/` uses macOS APIs.

## 12. Tests

```sh
ctest --test-dir build --output-on-failure    # or ./build/oap-tests
```

- `oap-tests` links only `oapcore`, `oapconfig`, `oapfrontend`, `oapmusicserver` and `oapaudio`, so it builds without FFmpeg, eSpeak NG,
  SDL2, audio libraries or libcurl (`cmake -DOAP_BUILD_SIM=OFF -DOAP_BUILD_HEADLESS=OFF`, where `OAP_BUILD_NET` defaults to `OFF`);
  yaml-cpp is downloaded if missing. With `OAP_BUILD_NET` it links `oapnet` and libcurl through `oapmusicserver`. Without a front end
  `oapaudio` holds only the null output; the device backends are added with the sim or headless build.
- `tests/fakes.h` provides `FakeSpeech` (records what was said), `FakePlayer`
  (position, end of file and chapters set by the test), `FakeSounds`, `FakeVolume`,
  `FakeClock` (time only moves when the test advances it), `FakeStore` (what was
  last saved to a settings file, and how often) and `FakeMusicServer` (records each
  request; answers only when the test calls `reply` / `replyPing` / `fail`, and then
  only on the next `poll()`; honours `cancel`; its stream URLs show the quality,
  `fake://stream/<id>?maxBitRate=<kbps>`, and with `saltUrls` differ on each call, as a
  real server's do; `scrobble()` calls are recorded in `scrobbles`). `FakeDownloader` records each `download()` and finishes only when the test calls `finish(id, result)` (which writes `dest` for `Ok`, or a `.part` for a failure, if asked) or `progress()`, then on the next `poll()`; honours `cancel`. `FakePlayer` records the `Transcoded` of the last open.
  `test_fake_music_server.cpp` checks that contract.
- `test_config.cpp` tests YAML ⇄ dotted keys, `YamlStore` / `TsvStore` on real files
  (mode 0600, atomic replace, a broken file left alone) and `SettingsFiles`.
- `test_app.cpp` drives `App` the way a user does, through `keyDown`/`keyUp`/`tick`
  on a temporary library folder, and checks what is spoken, which file plays and
  what is saved. Long presses, sleep timers and auto sleep are tested by advancing
  the fake clock.
- `test_terminal_keys.cpp` tests the headless terminal key decoding, including sequences split across reads.
- `test_presses.cpp` tests `parseScript` and `PressSequencer` timing; `test_held_buttons.cpp` tests `HeldButtons`.
- `test_gamepad.cpp` tests the gamepad map, including the order of light and deep stages.
- `test_no_music_server.cpp` tests `NoMusicServer` (answers only from `poll()`, `cancel`, also from a callback).
- `test_make_music_server.cpp` tests `makeMusicServer()`: no server, another server type, and a Subsonic server,
  which streams only in a build with `OAP_BUILD_NET` (CMake tells the test which build it is in).
- `oap-net-tests` (built with `OAP_BUILD_NET`) tests `Subsonic` URL building, auth
  and error mapping over a fake `Http`, the parsers on saved answers in
  `tests/data/subsonic/` (non-ASCII and emoji titles, `\uXXXX` surrogate pairs, `\/`,
  an 80 kB single-line page), and `HttpClient` against sockets on 127.0.0.1 (refused,
  stalled, trickling, too large, cancelled, destroyed while stuck). No internet.
- `oap-media-tests` (built with the simulator or headless front end, as it needs FFmpeg) tests
  `Player` on WAV files it writes itself; the format-change tests make MP3s with the `ffmpeg`
  command and are skipped without it. Two tests capture stderr to check that a URL that fails
  to open is not printed.
- `test_audio_output.cpp` tests `AudioOutput::open()` with the null backend, and
  `ThreadedAudioOutput` with a fake device that is lost and comes back.
- The other files test `GestureDetector`, `AudioStream`, `Mixer`/`Earcons`,
  `Settings`, `scanLibrary` and `spokenDuration` directly.
- Every test has been checked with a sabotage: break the code it guards and
  watch it fail. See [CLAUDE.md](../CLAUDE.md).

## 13. Known gaps / next steps

- Speed control (libavfilter `atempo`, or Sonic), skip silence.
- Bookmarks, podcasts (RSS), sync (Audiobookshelf).
- Streaming phases 2a, 4 and 5 in [streaming.md](streaming.md): star / unstar and a Starred
  list, one library with the server, and sync.
- STT (Talk button): push-to-talk capture plus sherpa-onnx, Vosk or whisper.cpp.
- Localisation of spoken strings; a choice of TTS voice.
- Hardware abstraction (battery, headphone jack, Wi-Fi, Bluetooth, suspend): planned in [hal.md](hal.md).
- Music: shuffle and repeat.
- The `AudioStream` mutex in the audio callback could become a lock-free ring.
