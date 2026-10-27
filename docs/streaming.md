# Streaming from a Subsonic server: plan

> Status: **phases 1 and 2 built** (stream and play, see [What phase 1 built differently](#what-phase-1-built-differently);
> robust streaming, see [Phase 2](#phase-2-robust-streaming)). Phases 2a to 5 are a plan. The code is described in [architecture.md](architecture.md), and the
> rules for lists from the server are in [navigation.md](navigation.md#lists-from-the-server).
> Last updated: 2026-10-25.
> Target server: Navidrome 0.64 (Subsonic API 1.16.1, OpenSubsonic). Test server:
> `https://navidrome.microserver`. The credentials are kept in `.env` (git ignores it) only so
> they are at hand during development; the program does not read it.

The aim is to play music (and later audiobooks) from a self-hosted Subsonic server
(Navidrome, Gonic, Airsonic, ...), first by streaming, then with a local cache,
and finally as one library together with the local files. The work comes in phases,
and each phase leaves a device that works.

Phase 1 is the minimum to test on the device: browse the server, choose an album,
and it plays. The later phases add robustness, offline copies, a merged library and
two-way sync.

SPEC §1.2 lists "streaming music services (Spotify etc.)" as a non-goal. A server
the user hosts is a different thing: it is private and needs no account with a
company. SPEC should say this when phase 1 is merged (it now does, in §1.2).

**Subsonic is the first server type, not the only one.** Others come later, such as
Audiobookshelf for books (SPEC §6.3). For that reason the core interface
(`MusicServer`) and its data structs say nothing about Subsonic, and the `server:`
section of `config.yaml` has a `type` key. A new service is a new class in `app/net/` that implements the same
interface, with no change to `App`.

---

## What the server gives us (checked against the test server)

| Need | Endpoint | Notes |
|------|----------|-------|
| Check login | `ping` | `status: ok`. A wrong password gives error 40. |
| Albums | `getAlbumList2?type=newest\|alphabeticalByName\|recent\|frequent\|random\|starred` | Paged with `size` (≤ 500) and `offset`. |
| Artists | `getArtists` → `getArtist?id=` | Indexed A–Z. The test server has 50 artists. |
| Album tracks | `getAlbum?id=` | `song[]` with `id, title, track, duration, suffix, artist`. |
| Playlists | `getPlaylists`, `getPlaylist?id=` | The test server has none yet. |
| Search | `search3?query=` | Needs text input (STT), so it comes later. |
| Audio | `stream?id=` | Without `format`, the original file is sent (FLAC on the test server). **FFmpeg opens the URL as it is**: `ffprobe` read the format and the duration. Seeking uses HTTP Range. With `format=mp3&maxBitRate=128` it is a transcode: `Accept-Ranges: none`, no length, and a Range header is ignored; `timeOffset=<s>` (extension `transcodeOffset`) starts it later. FFmpeg reports its end as a read error. |
| Original file | `download?id=` | For the cache (phase 3). |
| Played | `scrobble?id=&submission=` | For server play counts and "recent". |
| Position | `createBookmark` / `getBookmarks`, `savePlayQueue` / `getPlayQueue` | For audiobook and queue sync (phase 5). |

Extensions on the test server: `transcodeOffset`, `transcoding`, `formPost`,
`songLyrics`, `indexBasedQueue`, `playbackReport`, `topSongsByArtistId`. There is
**no `apiKeyAuthentication`**, so we use the classic token auth:
`u=<user>&t=md5(password+salt)&s=<salt>&v=1.16.1&c=oap&f=json`, with a new random salt
for each request. The password is never sent over the network, but it is kept on the device.

---

## Architecture

```
app/core/          App  ── Streaming menu, remote albums as a playable "book"
                    ├── services.h: MusicServer (new interface, async)
                    └── Settings: flat dotted keys in memory; file I/O behind an interface
app/config/ (new)  YamlStore    config.yaml ⇄ Settings (yaml-cpp)
app/net/    (new)  HttpClient   libcurl, one worker thread, timeouts
                   Subsonic     : MusicServer — builds URLs, auth, parses JSON (yaml-cpp)
app/media/         Player       opens http(s) URLs (FFmpeg already can), network options
```

The layering rules in CLAUDE.md still hold:

- **Core does no networking.** `App` talks to a new `MusicServer` interface in
  `services.h`. Tests use a `FakeMusicServer` in `tests/fakes.h`.
- **Requests are asynchronous.** `App` is single-threaded and must never block on
  the network. The interface queues a request and returns at once. `App::tick()` calls
  `server.poll()`, which runs the finished callbacks on the UI thread. The fake
  delivers a result only when the test says so, so "Loading" states can be tested.
- **A late answer is dropped by id.** Each request returns a `RequestId`. The screen
  that asked keeps it, and popping that screen calls `cancel(id)`, so its callback
  never runs. The callback holds the id and a copy of what it needs, never a pointer
  to a `Screen`, because the stack may have changed. If a result arrives for a screen
  that is still on the stack but not on top, it fills that screen silently.

  ```cpp
  struct RemoteTrack { std::string id, title, artist, album; double duration = 0; int track = 0; };
  struct RemoteAlbum { std::string id, name, artist; int songCount = 0; };
  struct RemoteArtist { std::string id, name; int albumCount = 0; };
  struct RemotePlaylist { std::string id, name; int songCount = 0; };

  class MusicServer {
  public:
      enum class Error { None, NotConfigured, Unreachable, BadLogin, Server };
      struct ServerInfo { std::string type, version; }; // "navidrome", "0.64.2"
      using RequestId = uint64_t;
      // `more` is true when the server may have another page after this one.
      template <class T> using Done = std::function<void(Error, std::vector<T>, bool more)>;
      virtual ~MusicServer() = default;
      virtual bool configured() const = 0;
      virtual RequestId ping(std::function<void(Error, ServerInfo)> done) = 0;
      virtual RequestId albums(std::string listType, int offset, int size, Done<RemoteAlbum> done) = 0;
      virtual RequestId artists(Done<RemoteArtist> done) = 0;
      virtual RequestId artistAlbums(std::string artistId, Done<RemoteAlbum> done) = 0;
      virtual RequestId albumTracks(std::string albumId, Done<RemoteTrack> done) = 0;
      virtual RequestId playlists(Done<RemotePlaylist> done) = 0;
      virtual RequestId playlistTracks(std::string id, Done<RemoteTrack> done) = 0;
      virtual void cancel(RequestId id) = 0; // its callback will not run
      virtual std::string streamUrl(const std::string& trackId) const = 0; // what Player opens
      // (since step 2.4: Stream stream(trackId, maxBitRate), the URL and, for a transcode, its offset URL)
      // (since step 2.5: void scrobble(trackId, submission), with no callback)
      virtual void poll() = 0; // UI thread: run callbacks of finished requests
  };
  ```
- **`app/net/`** is a new library `oapnet`, built with the front ends like
  `oapmedia`. Dependencies (agreed) are **libcurl** (HTTP, TLS, timeouts; packaged for arm64 on
  Debian) and **yaml-cpp**, the same library that reads the config file (see below).
  JSON is valid YAML 1.2, so yaml-cpp parses the server's `f=json` answers too.
  MD5 for the token comes from a small local function, or from OpenSSL, which curl
  already links. A new `oap-net-tests` binary tests URL building, auth and JSON parsing
  on saved responses in `tests/data/subsonic/`, with no network. The real server is
  tested by hand, with its details in a development `config.yaml`.
- **A stream URL is never spoken or logged.** It contains the user name and the auth
  token, which never expires. Anything spoken about a remote track uses its server
  title ("Cannot play Storm"), never `filename()` of the location. `Tts` prints
  everything it says to stdout, so a URL there would end up in logs.
- **Playback reuses the music path.** `Book.files` holds `std::filesystem::path`.
  A URL fits in it, but the chapter titles would be made from the URL. So `Book` gets
  an optional list of track titles (or `files` becomes a list of `{location, title}`).
  A streamed album is played like a local album (`music_ = true`): it says "track",
  ends with "End of music", saves no position and does not change `last_book`.
- **A remote track that fails to open stops playback.** For a local book,
  `App::loadFile` tries each following file until one opens. For streams that would
  block the UI for up to 10 s per track when the network is gone. So for remote
  tracks it says "Server not reachable" and stops after the first failure, and the
  previous track stays loaded and paused.

### Configuration: one YAML file

Everything a person sets lives in **one file, `config.yaml`**: the menu settings and
the server. It is easy to read and edit over SSH or USB. It replaces the setting
keys in `settings.tsv`.

```yaml
# ~/.config/oap/config.yaml   (simulator: ~/.config/oap-sim/, device: /data/config/), mode 0600
speech:
  rate: 220
volume: 70
verbosity: 1
skip:
  back: 10
  forward: 30
rewind_on_resume: 5
buttons:
  long_press_ms: 600
menu:
  wrap: 1
power:
  auto_sleep_min: 10
streaming:
  bitrate: 0                # Streaming quality: 0 = the original file, or 320 / 192 / 128 kbps MP3

server:
  type: subsonic            # first type; others (e.g. audiobookshelf) later
  name: Home server         # spoken in menus; default "Server"
  url: https://navidrome.microserver
  user: ice
  password: "..."           # quote it: an all-digit password would be read as a number
```

(Phase 1 read `server.format` and `server.max_bitrate`; since step 2.4 the Streaming
quality setting, `streaming.bitrate`, replaces them, and they are not read.)

- **The existing keys map directly.** The setting keys are already dotted
  (`speech.rate`, `skip.back`, `buttons.long_press_ms`, ...), so nested YAML is
  flattened to the same keys. `App` and `choice()` do not change, and
  `settings_.getString("server.url")` reads the server.
- **Core stays free of dependencies.** `Settings` stays in core as the in-memory map
  of dotted keys. Loading and saving go through a small interface (load into a map,
  save a map), so core never sees YAML. Tests use the in-memory version they use
  today. `app/config/` (new library `oapconfig`, with yaml-cpp) holds `YamlStore`,
  which flattens on load and nests on save.
- **State is written only on exit.** Today the position is saved on pause, sleep,
  power off, book change and every 30 s of playback. That is a lot of writes to
  flash, so it goes. Pause and book change still record the position, but only in
  the in-memory `state` map, so switching books never loses the place. The map is
  written once,
  when the program exits: Power off, Restart, and SIGTERM / SIGINT / SIGHUP (all go
  through `App::shutdown()`). Sleep writes too, because on the Pi Zero 2 W sleep may
  end in the power being cut.
  - The cost is that a crash, OOM kill, kernel panic or sudden loss of power (empty
    battery, cable pulled) loses the place since the last exit. That can be hours of
    a book. This is accepted to spare the flash. The power MCU must therefore shut the
    device down cleanly at low battery (SPEC §3.2), and systemd must stop the
    service with SIGTERM and allow enough `TimeoutStopSec` for the write.
  - State stays in its own file, `state.tsv` (the current TSV format, written by
    core). The user never needs to open it, and `config.yaml` is written only when
    the user changes a setting. `App` takes two `Settings` objects, `config` and `state`.
- **Saving from the menus drops comments.** yaml-cpp does not keep comments when it
  writes the file. To limit this, the file is written only when a setting is
  changed in the Settings menu (on leaving the item, navigation rule 6), with keys in
  a fixed order and a header comment that names the keys. Server keys are never
  changed by the device, so they are written back as read.
- **Volume from the player is not a config write.** Today `App::changeVolume` saves
  on every press. Up / Down in the player now change the volume in memory only, and
  it is written on exit together with the state. Otherwise every volume press would
  rewrite `config.yaml`.
- **Every write is atomic.** Write `file.tmp`, `fsync`, then `rename` over the old
  file, and keep mode 0600 (create the temp file with that mode, not the umask).
  Power cut mid-write then leaves the old file, not an empty one. This holds for
  `state.tsv` and `config.yaml`, and replaces the truncating `std::ofstream` write
  in `Settings::save()`.
- **Factory reset** clears the user settings and the state, but **keeps `server:`**.
  Otherwise a blind user would lose a server set up for them by someone else. Today
  the reset calls `restart()`, which saves the position first. After a reset that
  save must be skipped, or the position would go straight back into the cleared state.
- Front ends take `--config FILE` (the YAML) and `--state FILE`.
- **One library for config and JSON:** yaml-cpp. It is packaged in Debian
  (`libyaml-cpp-dev`, also arm64), and CMake can fetch it like GoogleTest, so the
  tests build without system packages. yaml-cpp is slower than a JSON-only parser.
  That is accepted, because the answers are parsed on the network thread and the
  "Loading" item covers the wait.
  - Edge cases of reading JSON as YAML: `\uXXXX` escapes including surrogate pairs
    (emoji in titles), `\/`, and very long single-line documents. There is a fixture
    test for each.
- Setting it up without a screen (SPEC §6.3): copy `config.yaml` over USB or SSH for
  now. Later options are a setup web page and dictation.

### Menu

Streaming is a new entry on Home, between Music and Podcasts:

```
Home
├── Now playing ...
├── Library
├── Music
├── Streaming                       (label is `server.name` from config.yaml)
│   ├── Recently added   → <albums>           getAlbumList2 type=newest
│   ├── Albums A to Z    → <albums>           type=alphabeticalByName (paged, see below)
│   ├── Artists          → <artist> → <albums>
│   ├── Playlists        → <playlist> → <tracks>
│   ├── Random albums    → <albums>
│   ├── Downloaded       → <list> → Play · Delete download · <tracks>   (phase 3, from the cache manifests)
│   └── Server status                         action: "Connected to Navidrome 0.64" / the error
│
│   <album> → <tracks>   OK on a track plays from it to the end of the album (like Music)
│   first item: "Play album"
├── Podcasts
...
```

Rules for lists loaded over the network. These are new and go into
[navigation.md](navigation.md):

- **A list that loads from the network opens at once with one item, "Loading".**
  When the data arrives, the list is filled in. If the user is still on it, the
  first item is spoken. Keys work meanwhile: Home leaves, and the late answer is
  dropped.
- **An error is a list too.** For example, "Server not reachable", followed by a
  "Try again" item. Without a `server:` section in `config.yaml`, the Streaming menu says "No server set
  up" and how to add one. This follows the rule that an empty list says why and
  what to do.
- **Long lists are paged by the server, not by the user.** Load 500 albums at
  once and, while `more` is true, fetch the next page (`offset` += 500) in the
  background. Random albums are one page of 50 with no paging, because each page
  is a new random draw and would repeat albums. Holding Up or Down still jumps
  to the first or last item of what has loaded.
- **The player does not change.** A streamed track has the same keys, Player menu
  and "where am I?" as a local track.

---

## Phase 1: stream and play (test on the device)

Goal: on the Pi, go Home → Streaming → Recently added → an album → OK, and hear
it. Skip, next or previous track, pause and resume all work. Pulling the Wi-Fi
must not hang the device.

| # | Step | Tests (each checked with a sabotage, as CLAUDE.md requires) |
|---|------|------|
| ✅ 1.1 | One config file: `Settings` load/save behind an interface; `app/config/` `YamlStore` (yaml-cpp) for `config.yaml`; positions and `last_book` move to `state.tsv`; `--config` / `--state` in both front ends; factory reset keeps `server:` and does not save the position again; atomic writes with mode 0600; player volume is saved on exit. The position and `last_book` are saved only on exit and sleep; the saves on pause, book change and every 30 s are removed. | YAML ⇄ dotted keys both ways, an all-digit quoted password stays a string, a missing `server.url` means not configured, factory reset keeps the server and leaves no `pos:` key, a written file keeps mode 0600, and pressing volume writes nothing until exit. Pause, a book change and minutes of playback write nothing to disk, yet switching back to a book resumes it; Power off, sleep and `shutdown()` write the position to `state.tsv` and do not touch `config.yaml`. (Existing tests that expect a save on pause change with it.) |
| ✅ 1.2 | `MusicServer` interface, data structs, `FakeMusicServer`. | – |
| ✅ 1.3 | `app/net/`: `HttpClient` (libcurl on a worker thread; 10 s connect and read timeouts) and `Subsonic` (auth token, URL building, JSON parsing with yaml-cpp for ping, getAlbumList2, getArtists, getArtist, getAlbum, getPlaylists, getPlaylist, plus error codes 40 / 50 / 70). | `oap-net-tests` on saved JSON, including non-ASCII and emoji titles. URL has `t`/`s` and not `p`. Bad login maps to `BadLogin`. Ping returns the server type and version. A cancelled request's callback does not run. |
| ✅ 1.4 | `Book` gets track titles from the server. `App` plays a list of remote tracks through `switchTo(..., music=true)`. A remote track that fails to open stops playback instead of trying the next one. | Choose a track in a fake album: the right URL is opened and the server's title is spoken. No `pos:` key is saved, and `last_book` is unchanged. A failed open says "Server not reachable", tries no other track, and nothing spoken contains the URL. |
| ✅ 1.5 | Streaming menu with Loading, error and not-configured states. Server status item. | Loading, then filled, with the first item spoken. Home while loading: a late result does not speak. A result for Artists does not fill Playlists opened after it. Albums A to Z loads a second page when `more` is true. Error, then Try again. No config gives the help text. |
| ✅ 1.6 | `Player` for URLs: `rw_timeout` / `timeout`, `reconnect` options and an interrupt callback, so `close()` never waits on a stuck socket. The open runs on the UI thread for now, so say "Loading" before it and cap it at 10 s. | Media test: an unreachable URL fails within the cap. (Use a local port with nothing listening; no internet needed.) |
| ✅ 1.7 | Build: libcurl and yaml-cpp in `CMakeLists.txt` and both Docker targets. `-DOAP_BUILD_NET=OFF` keeps the core-only build working. | `docker build --target native` and `--target pi` pass. |
| ✅ 1.8 | Docs: architecture.md (layers, threads, menu tree, persistence), navigation.md (network list rules), SPEC non-goal note. | – |

**Device check (manual):** Wi-Fi on → album plays within 2 s on a fast network,
FLAC original. Wi-Fi off during play → buffered audio runs out, then "Server not reachable"
and the device keeps answering keys. Wi-Fi off → the Streaming list says
"Server not reachable".

Known limits after phase 1: the UI pauses briefly while a stream opens, positions
are not remembered, and nothing works offline.

### What phase 1 built differently

The plan above is kept as it was written. Where the code differs, the code is right:

- **Paging.** Only Albums A to Z is paged (500 at a time, the next page fetched
  while `more` is true). Recently added is one page of 500, and Random albums one
  page of 50. Artists, an artist's albums, playlists and an album's tracks are one
  request each.
- **Server status** says "Checking the server." at once, then the server's type and
  its full version string, such as "Connected to Navidrome 0.64.2.", or the error.
- **Albums and playlists** open on "Play album" or "Play playlist", then the tracks.
- **A server type the player cannot use** (or any server in a build without
  `OAP_BUILD_NET`) gives one item, "Server not supported", instead of "No server set up".
  OK says that the player streams only from Subsonic servers and only when built with
  network support.
- **Lists from the server are not rebuilt** when the user comes back to them: they
  keep what has loaded.
- **A remote track that fails** says "Cannot play Storm. Server not reachable." What
  was current before stays current and paused. If that was a remote track too, it is
  not opened again at once (it would wait on the same server): Play opens it. When the
  next track fails at the end of one, the failed track becomes current at 0, so Play
  opens it and not the finished one again.
- **A stream that breaks off mid-track** (Wi-Fi off, server gone) is not the end of the
  track: `MediaPlayer` got `failed()` already (planned for phase 2). Once the buffered audio
  has been heard it says "Server not reachable. Press play to try again.", tries no
  other track, and Play opens the track again where it stopped. Since step 2.2 this
  comes only after `Player` has retried for 30 s (buffering earcon and "Waiting for
  network" meanwhile).
- **No migration from `settings.tsv`.** The old file is not read; settings start from
  the defaults.

---

## Phase 2: robust streaming

Goal: streaming feels like local playback. Keys answer while a stream opens, a short
dropout does not lose the track, tracks change without a gap, and the device resumes
a stream after a restart.

| # | Step | Tests (each checked with a sabotage, as CLAUDE.md requires) |
|---|------|------|
| ✅ 2.1 | **Asynchronous `Player::open`.** Open on the decoder thread. `MediaPlayer` gets `opening()` (it has `failed()` since phase 1), or an open callback. `App` speaks "Loading" and stays responsive. | Keys are answered while an open is pending. A late open result after Home or after choosing another track does nothing. A failed asynchronous open says "Server not reachable". Media test: opening an unreachable URL does not block the caller. |
| ✅ 2.2 | **Buffering feedback.** If the content stream runs dry while playing, play a "buffering" earcon (a new `Earcon`), and say "Waiting for network" after 3 s. Retry with backoff, and do not skip the track because of a short dropout. | Dry and recovered within 3 s: earcon only, same track. Dry for more than 3 s: the phrase is spoken once. Retries give up after the cap and fall back to phase 1's "Server not reachable. Press play to try again." |
| ✅ 2.3 | **Read-ahead.** Open the next track's connection about 10 s before the current one ends, so tracks change without a gap on a slow link. | The next URL is opened about 10 s before the end (fake clock). A pre-opened track that fails falls back to a normal open. Leaving the album drops the pre-open. |
| ✅ 2.4 | **Quality setting** (`choice()` in Settings → Streaming quality: Original / 320 / 192 / 128 kbps, default Original), saved in `config.yaml`. With transcoding, seek with `timeOffset` (extension `transcodeOffset`) instead of Range. | The stream URL carries `maxBitRate` / `format`, or neither for Original. A seek on a transcoded stream uses `timeOffset`, not Range. The setting persists across a restart. |
| ✅ 2.5 | **Scrobble**: `scrobble submission=false` when a track starts and `true` after half of it or 4 min, so "Recently played" on the server is right. | The fake server records both calls at the right times. A skip before half sends no `true`. A failed scrobble says nothing. |
| ✅ 2.6 | **Remember the place.** `last_stream` (server + album/playlist id + track + position) in `state.tsv`, so the device resumes after a restart. "Now playing" on Home works for streams too. | Exit and restart resume the same album, track and position. With the server down, resume says "Server not reachable". A local `last_book` still works. |
| ✅ 2.7 | Docs: architecture.md (open thread, buffering earcon, read-ahead, quality, scrobble, `last_stream`), navigation.md, SPEC where needed. | – |

Step 2.6 as built: the place is the `last_stream.*` keys of `state.tsv` (server url and
user, `album` or `playlist` and its id, title, track index and id, seconds), kept in
memory on pause and list change and written only on exit and sleep, like `last_book`.
Whichever played last resumes: opening a local book removes `last_stream.*`, and a
stream played after a book is resumed instead of it (the book keeps its place in
Library). Local music changes neither. At boot nothing is asked of the server and
nothing plays: Home opens with "Now playing, Weather" first, and OK on it (or Play)
says "Loading.", loads the list again, finds the track by its id (if it is gone, the
track now at its index from its start, else the first track) and opens it at its
place like a track chosen in the list. An error says "Server not reachable." (or the
other list errors) and Home stays. A place saved for another server url or user, or
with no server, is ignored. A resume after a restart is a new play for scrobbling.

**Device check (manual):** keys are answered at once during "Loading". A short
Wi-Fi drop during play gives the buffering earcon and the same track goes on.
Tracks change without a gap on a slow link. After a restart, "Now playing" resumes
the stream where it stopped.

Known limits after phase 2: nothing plays offline, and stars are not built (phase 2a).

## Phase 2a: stars

Moved out of phase 2: the Player menu and the Streaming lists need a new interface
first. Break it into steps once that interface is decided.

| # | Step | Tests (each checked with a sabotage, as CLAUDE.md requires) |
|---|------|------|
| 2a.1 | **Star / unstar** from the Player menu (it moves to Options with the queue, [queue.md](queue.md) Q7), and a **Starred** list in Streaming. Decide whether a track played from Starred is saved in `last_stream` (it has no album or playlist id; it needs a new `RemoteSource::Kind` that `resumeStream()` can load again). | Star and unstar call the server. Starred loads like the other lists (Loading, filled, error). An error is spoken. |

## Phase 3: local cache and downloads

Goal: albums and playlists play without the network. A downloaded album plays
offline with no change to the menus, and what was streamed is kept in a cache with a
size limit.

Decided for this phase: there is no Wi-Fi-only check for now (the device has only
Wi-Fi). Outside the device the cache folder defaults to `cache/` next to `state.tsv`
(`--cache DIR` overrides it). The automatic cache downloads a streamed track a second
time in the background, with a default limit of 1 GB. Download progress is spoken on
the album's Download item and by "where am I?".

| # | Step | Tests (each checked with a sabotage, as CLAUDE.md requires) |
|---|------|------|
| ✅ 3.1 | **Cache store in core.** `TrackCache` (plain C++, `std::filesystem`): `<cache>/<server key>/<album or playlist id>/` with an atomic `manifest.tsv` (track id, file, title, duration, server `changed` time, pinned), answering "complete cached file for this track id, or none". Front ends take `--cache DIR` (device: `/data/library/cache`; elsewhere `cache/` next to `state.tsv`). `scanLibrary()` skips the cache folder. `RemoteTrack` gets `suffix` and `size` from the server. | The manifest round-trips. A `.part` or missing file is never returned. A corrupt manifest is ignored without a crash. The cache folder never appears in Library or Music. The JSON fixture gives `suffix` and `size`. |
| ✅ 3.2 | **Play from the cache.** A track resolves to its complete cached file, else its stream URL. A fully cached album plays offline with no change to the menus; read-ahead and scrobble still work. | All tracks cached: files play, no stream URL is opened. Mixed album: each track uses the right source. A cached file deleted on disk falls back to the URL. |
| ✅ 3.3 | **Download worker.** A `Downloader` interface in `services.h`, implemented with libcurl in `app/net/`: one track with `download?id=` into `*.part`, resumed with Range, then renamed. Asynchronous, cancellable, results through `poll()`; the URL is never logged. `FakeDownloader` in `tests/fakes.h`. | Net tests: the URL is right, a partial `.part` resumes with Range, the rename happens only when complete, a failure keeps the `.part`. After cancel the callback does not run. |
| ✅ 3.4 | **Download album / playlist.** A "Download album" item right after "Play album" (and the same for playlists). Core queues the tracks, writes the manifest after each and pins them. Free space is checked first (`std::filesystem::space` against the sum of the track sizes). OK on the item again, and "where am I?", say "Downloading, 3 of 9 tracks". | Tracks are fetched in order and pinned. Not enough space: nothing starts and the reason is spoken. Progress is spoken on the item and by "where am I?". A failed track is spoken and a retry resumes. Leaving the menu does not stop the download. |
| ✅ 3.5 | **Streaming → Downloaded** lists the albums and playlists in the manifests; they play like the server lists. **Delete download** sits in a list that opens on Cancel. | The list matches the manifests. Playing from it opens files. Delete opens on Cancel, removes the folder, and the list updates. An empty list says why. |
| 3.6 | **Offline.** When the server cannot be reached, Streaming still offers Downloaded, and the error list gets a "Downloaded" item. `last_stream` resumes from the cache with the server down. | Server unreachable: Downloaded works and plays. Resume after a restart with the server down plays the cached album. |
| 3.7 | **Automatic cache with an LRU limit.** Setting (0 / 1 / 4 / 16 GB, default 1 GB) in Settings, saved in `config.yaml`. A streamed track is queued to the downloader unpinned; above the limit the least recently played unpinned tracks are evicted. | A streamed track ends up cached. Over the limit the least recently played unpinned track goes first; pinned tracks never go. With 0 nothing is cached. The setting persists. |
| 3.8 | Docs: architecture.md, navigation.md, SPEC where needed, and what phase 3 built differently. | – |

**Device check (manual):** download an album, pull the Wi-Fi, and it still plays
from Streaming → Downloaded and from "Now playing" after a restart. "Where am I?"
during a download says the progress. A streamed album plays offline the second time.

## Phase 4: one library (local files and server)

- **Library index** instead of separate `scanLibrary()` lists:
  `Item { id, source: local|server|cached, kind: book|music|podcast, title, artist, album, tracks }`.
  It is kept on disk (SQLite, as in SPEC §4.7) so that start-up does not rescan and
  browsing works offline.
- **Incremental sync** of the server catalogue: `getIndexes ifModifiedSince` and
  album `changed` times. It runs on Wi-Fi connect and from a "Refresh" item.
- **Duplicates:** when a local album and a server album are the same (MusicBrainz id,
  or artist + album + track count), show one entry and prefer the local file.
- **Browse across sources:** Music → Artists / Albums / Genres / Recently added /
  Recently played. Each item says where it plays from only when asked ("Info").
- **Audiobooks from the server:** a server music folder (`getMusicFolders`), picked in
  `config.yaml` (`server.audiobook_folder: Audiobooks`), is treated as books. Books save
  positions and set `last_book`, and they are listed under Library.
- **Streaming** stays a separate Home entry for now (decided). It may later become a
  filter ("Server only") inside Library and Music, once the merged views work well.

## Phase 5: sync and playlists

- **Playlists, both ways:** create, rename, add the current track ("Add to playlist"
  in the Player menu, which opens on the last playlist used), remove a track, and
  delete (behind Cancel), all through `createPlaylist` / `updatePlaylist`. Local-only
  playlists stay as `.m3u` files in `/data/library/playlists/`.
- **Offline playlists:** "Keep offline" on a playlist pins its tracks in the cache
  and follows changes made on the server.
- **Position sync for books:** server `createBookmark` / `getBookmarks` per track,
  with the latest timestamp winning. If the server is further on, ask: "Server
  position is further, jump there?" (SPEC §6.3).
- **Play queue sync:** `savePlayQueue` / `getPlayQueue` (`indexBasedQueue`), so a
  queue started on a phone continues on the device.
- **Offline outbox:** scrobbles, stars, bookmarks and playlist edits made offline go
  into a queue file and are sent in order when the server can be reached again.
- Local files are **not uploaded**: the Subsonic API has no upload. Sending local
  books to the server is a job for Syncthing or WebDAV, outside this plan.

---

## Decisions

- New libraries libcurl and yaml-cpp: **yes**. yaml-cpp reads both the config and the JSON answers.
- **One config file**, `config.yaml`, for menu settings and the server. Positions stay in `state.tsv`.
- **State is written only on exit** (and sleep), to spare the flash. A crash or power loss loses the place since the last exit.
- `.env` only stores the test credentials for development; the program does not read it.
- yaml-cpp is slow at parsing JSON, and that is **acceptable**.
- Subsonic is the **first** server type. Other services, such as Audiobookshelf for
  books, are added later behind the same interface.
- Streaming is a **separate Home entry** for now and may be merged into Library and
  Music later.
- Streams default to the **original file**. Streaming quality in Settings can pick a
  transcode (320 / 192 / 128 kbps) for slow links.

## Open questions

1. Should more than one server be supported (`servers:` as a YAML list)? With a second
   service type such as Audiobookshelf this becomes likely. The plan assumes one
   for now, but the config and menu should not rule out more.
2. Should the password stay in plain text in `config.yaml` (mode 0600), or should
   the device keep only `t`/`s` (a token that never expires, so it is just as
   sensitive)? Navidrome API keys would be better once the server offers
   `apiKeyAuthentication`.
3. TLS on the device: check that the Pi image trusts the server's certificate. A
   self-signed home CA would need `server.ca_file` in `config.yaml`.
