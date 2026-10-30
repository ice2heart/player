# Streaming from a Subsonic server: plan

> Status: **phases 1 to 4 built** (stream and play, see [What phase 1 built differently](#what-phase-1-built-differently);
> robust streaming, see [Phase 2](#phase-2-robust-streaming); local cache and downloads, see
> [Phase 3 as built](#phase-3-as-built); one library of files and server, see
> [Phase 4 as built](#phase-4-as-built)). Phase 2a stars are built (manual listening checks pending); phase 5 is a plan. The code is described in [architecture.md](architecture.md), and the
> rules for lists from the server are in [navigation.md](navigation.md#lists-from-the-server).
> The music queue ([queue.md](queue.md) phase 2) changed some of it: tracks from any source play in one
> queue, `queue.tsv` replaced `last_stream`, and a track that cannot play is skipped.
> Download quality and cache replacement are built in [fixes.md phase 5](fixes.md#phase-5-download-quality-and-the-cache); real-server listening checks remain pending.
> Last updated: 2026-10-30.
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
| Save a queue | `createPlaylist?name=&songId=&songId=...` | One `songId` per track, in play order (queue.md step 2.9). A GET, so at most 150 tracks; `App` says how many it left out. |
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
      // (since step 2.5: void scrobble(trackId, submission), with no callback; since fixes.md 3.2:
      //  scrobble(trackId, submission, timeMs, done), with the time it was heard and the error)
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
  ends with "End of queue", saves no `pos:` and does not change `last_book` (its place is in `queue.tsv`, queue.md step 2.8).
- **A remote track that fails to open stops playback.** (Only for a book now. The music queue skips
  an entry that cannot play and marks a server that fails as down, queue.md steps 2.2 and 2.3.) For a local book,
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
downloads:
  bitrate: 0                # Download quality: independent of Streaming quality; 0 / 320 / 192 / 128
cache:
  upgrade: 0                # Replace cached copies: 0 = off (default), 1 = prefer a better stream online
  limit_gb: 1               # Cache size: gigabytes of automatically cached tracks (0, 1, 4 or 16); 0 = off: caches nothing new, evicts nothing

server:
  type: subsonic            # first type; others (e.g. audiobookshelf) later
  name: Home server         # spoken in menus; default "Server"
  url: https://navidrome.microserver
  user: ice
  password: "..."           # quote it: an all-digit password would be read as a number
  audiobook_folder: Books   # optional (phase 4): the server's music folder whose albums are audiobooks, listed in Library; you set it, the player never writes it
```

This is the first form. Several servers go in a `servers:` list with the same keys in each
entry; `server:` is read as a list of one (see [audiobookshelf.md](audiobookshelf.md#configuration-a-list-of-servers)).

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
- **Volume from the player is not a config write.** `App::changeVolume` used to save
  on every press. The volume keys (Volume up / down, on every screen) now change the
  volume in memory only, and it is written on exit and sleep together with the state. Otherwise every volume press would
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

### Button-remap update (2026-10-30)

The historical phase-1 paging notes below are superseded by
[button-remap.md](button-remap.md) phase 1: Albums/Books A to Z load one page,
then offer Load more. Back/Forward short moves one item, deep ten and hold to the
loaded boundary. Jumps never fetch another page. A partial boundary announces
"More items are available. Load more to continue."; a fully loaded end does not.
Loading/error rows keep their own announcements. Recently added, Random albums
and other one-request lists retain their limits.

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
  OK says that the player streams from Subsonic servers and from Audiobookshelf, and only
  when built with network support.
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

Step 2.6 as built (replaced by the queue in `queue.tsv`, [queue.md](queue.md) step 2.8: `last_stream.*` is only read from a
state.tsv of an older version, turned into a queue when it plays, and removed): the place is the `last_stream.*` keys of `state.tsv` (server url and
user, `album` or `playlist` and its id, title, track index and id, seconds), kept in
memory on pause and list change and written only on exit and sleep, like `last_book`.
Whichever player played last resumes (queue.md step 1.5 changed this: opening a book no
longer removes `last_stream.*`; `last_player` says which was last, and the other is
"Continue" on Home). Local music changed neither then; `queue.tsv` resumes it too. At boot nothing is asked of the server and
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

Goal: star or unstar the playing server-linked music track from Options, including
local playback when the library knows its exact server track id, and browse and play
starred tracks from Streaming. The queue interface built in queue.md supplies playback
and restart persistence.

| # | Step | Tests (each checked with a sabotage, as CLAUDE.md requires) |
|---|------|------|
| ✅ 2a.1 | **Star / unstar** in `App::playerOptionsMenu()` (the Options of the music player, "Options, <track>", next to Go to album and Album info; queue.md step 2.5 built that menu and left Star out; the Player menu never gets it), for a remote track of the server set up now or a local track with an exact library track reference to a configured star-capable server, and a **Starred** list in Streaming. A track played from Starred is a queue entry like any other (it needs its album id only for Go to album), so it is saved in `queue.tsv` and nothing else is needed to resume it. | Star and unstar call the server. Starred loads like the other lists (Loading, filled, error). An error is spoken. Options in the player lists Star or Unstar for remote and linked local tracks, including indexed cached files, and neither for an unlinked local one. Linked local operations use the exact track id and keep playback and the saved queue local; invalid/non-track references never qualify, and configured servers are selected in configuration order, skipping unsupported or absent servers. |

**As built:** Subsonic uses [getSong](https://opensubsonic.netlify.app/docs/endpoints/getsong/)
for the current star state, [getStarred2](https://opensubsonic.netlify.app/docs/endpoints/getstarred2/)
for songs, and [star](https://opensubsonic.netlify.app/docs/endpoints/star/) / unstar to change it.
Options keeps Go to album and Album info usable while the state loads. Success says
"Starred." / "Unstarred." and returns to the player; errors stay for retry. Requests are
cancelled when Options closes, the track changes, or the device sleeps or exits. Starred
plays through the normal queue and keeps each track's album id; no new persisted state.
Stars are per configured music server. Local files, including indexed cached/downloaded
files, offer stars when the playing track has an exact library track reference. The first
configured star-capable server in configuration order with a valid track reference wins;
non-track, empty, malformed and removed-server references are skipped. Playback and the
saved queue remain local. Book providers and unlinked local tracks do not offer stars.

**Device check (manual):** play a server track, open Options and star it. Streaming → Starred
must include it and play it. Unstar it from the player's Options and reopen Starred: it is
gone. With the server unreachable, an error is spoken and Home still responds. Restart
while a track from Starred is saved: Now playing resumes its track and place. Also play a
local or downloaded file with a known library track reference: Star / Unstar must update
that exact song on the server while the local file continues playing. A local file with
no valid server track reference offers neither action.

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
| ✅ 3.6 | **Offline.** When the server cannot be reached, Streaming still offers Downloaded, and the error list gets a "Downloaded" item. `last_stream` resumes from the cache with the server down. | Server unreachable: Downloaded works and plays. Resume after a restart with the server down plays the cached album. |
| ✅ 3.7 | **Automatic cache with an LRU limit.** Setting (0 / 1 / 4 / 16 GB, default 1 GB) in Settings, saved in `config.yaml`. A streamed track is queued to the downloader unpinned; above the limit the least recently played unpinned tracks are evicted. | A streamed track ends up cached. Over the limit the least recently played unpinned track goes first; pinned tracks never go. With 0 nothing is cached. The setting persists. |
| ✅ 3.8 | Docs: architecture.md, navigation.md, SPEC where needed, and what phase 3 built differently. | – |

### Phase 3 as built

The plan above is kept as it was written. Where the code differs, the code is right:

- **Manifest.** `manifest.tsv` has the header `oap-cache 2`, a list line (kind, id, name,
  artist) and one line per track: id, file, title, duration, server `changed` time,
  pinned, last-played wall time, bitrate (0 for original, otherwise transcode kbps).
  Older seven-column track rows read as original and gain the column on their next
  save; merely reading them does not rewrite them. The header stays at version 2.
  The folder is `<cache>/<server key>/<kind>%3A<id>/`, so an
  album and a playlist with one id do not meet. Format 1 (no list line) reads as empty;
  no device ever held one. Core does no file I/O: manifests go through `TextFiles`
  (`services.h`), which `DiskTextFiles` (`app/config/`) implements with an atomic
  write. `App` takes it after the cache root; with no cache root the cache is off.
- **A damaged manifest.** One that is there but not valid is replaced on the next save,
  and the files in that folder the new manifest does not list are deleted (never a
  `.part`, never another folder). One that cannot be read at all (an I/O error) is not
  replaced: the save fails and nothing is deleted.
- **Complete means the downloader says so.** The server's `Content-Length`, or the total of
  `Content-Range` for a continued `.part`, is authoritative; a short body is
  `Unreachable` and keeps the `.part`. Only when the server sends no length is the body
  compared with the size from the listing for an original (0 accepts anything):
  shorter fails the same way, longer is taken as a stale size. A transcode ignores
  that original size; its own response length, when present, still applies. `App`
  checks no sizes itself, except the free space before a download.
- **Download quality (fixes.md 5.3–5.5).** Settings → Download quality selects
  original (default), 320, 192 or 128 kbps independently of Streaming quality.
  Subsonic originals use `download`; transcodes use `stream` with `format=mp3` and
  `maxBitRate`. Original-only Books providers still download originals. A job keeps
  the quality chosen at its start. A complete copy at least as good is kept and
  pinned (original is best, otherwise higher kbps); a lower copy stays playable
  until its replacement and manifest are saved. Transfer or manifest-save failure
  preserves the old copy. Originals use `<safe id>.<suffix>` and transcodes
  `<safe id>~<bitrate>.mp3`; a transcode restarts its `.part` without Range, while
  an original can resume. Free-space estimates for a transcode use duration ×
  bitrate × 1000 / 8, rounded up; unknown duration uses the original size, and an
  unknown size counts as zero.
- **Replace cached copies (fixes.md 5.6).** Settings → Replace cached copies is off
  by default (`cache.upgrade: 0`): a complete copy plays regardless of quality.
  On, a cached copy below Streaming quality opens the better stream if its server
  is not marked down; read-ahead follows that selection too. An immediate or
  asynchronous open failure, or open timeout, marks the server down and plays the
  cached copy at the requested position, preserving Play/Pause intent and without
  an unreachable error. Cancelling the open stays silent and plays no late copy.
  A server already marked down uses the copy directly.
- **Delete waits for cancel.** `HttpDownloader::cancel()` waits for the file operation in
  flight (one write at most), and the job then creates no folder or file, so Delete
  download can remove the folder at once. Deleting the list that downloads cancels the
  download; deleting the list loaded in the player is refused.
- **Order, dropped tracks, duplicates.** Downloading a list again makes its manifest
  follow the server's order. Pinned tracks the server no longer lists are dropped with
  their files; unpinned (automatically cached) ones stay after the others until the LRU
  limit evicts them. A track listed twice is fetched and counted once. The `.part` of the
  explicit download that failed last is kept in this session (OK on Download goes on from
  it for an original, or restarts it for a transcode); after a restart it is an
  abandoned `.part` like the others.
- **Resume after a restart.** A download that is whole (every manifest track pinned and
  complete on disk, its download over (no `partial` mark), the saved track among them) plays at once, with no question to the
  server and no "Loading.". Anything else asks the server first, so a partly cached album
  loses no tracks. If that fails, the tracks on disk play (the saved track and place if
  it is among them, else the first), and nothing is spoken but the usual "Now playing";
  with none on disk the error is spoken as in step 2.6. A whole download is not compared
  with the server.
- **Downloaded** lists only what the user downloaded (a list with at least one pinned
  track). The automatic cache's tracks play from their files wherever they are met
  (a server list, a resume), but make no entry there. An error list gets a "Downloaded"
  item when there is one. A pre-opened stream is cancelled when the next track turns out
  to be cached at sufficient quality (or replacement is off or the server is down).
- **Automatic cache.** A streamed track is queued once it has been heard for 10 s (a
  skipped track costs no bandwidth), unpinned, into the folder of the list it plays from.
  It captures the quality of the stream when it opened and fetches it a second
  time at that quality; later setting changes leave the job unchanged. With
  replacement enabled, an upgrade stream queues replacement of the lower copy
  under the same size and free-space rules, retaining its pinning. Playback from
  a fallback file never queues an upgrade, even if the server recovers.
  It is silent (no speech, no earcon, failures forgotten) and counts nothing in the
  download progress. An explicit download goes first: it cancels the running automatic
  transfer, which waits and resumes an original `.part` or restarts a transcode
  afterwards. Sleep pauses the automatic cache the same way (an explicit download goes on). It never fills the card:
  a track that would leave less than 256 MiB free is skipped. A track bigger than the
  limit is not cached at all; transcodes use the same duration-based estimate as
  explicit downloads, falling back to the original size.
- **LRU.** Only unpinned complete files count toward the limit (`cache.limit_gb`, 0 / 1 /
  4 / 16, default 1); pinned tracks never go and are not counted. A `.part` left by an
  abandoned transfer counts and is evicted first, then the least recently played (never
  played counts as oldest); the `.part` of a running or waiting transfer is never
  touched, and neither is the file the player has open. Trimming runs after a cached track
  and on exit and sleep, so a lowered size takes effect then. It scans the manifests only
  when there is a reason (plays to write, an abandoned transfer, bytes over the limit).
  Off keeps what is cached: nothing new is cached and nothing is evicted.
- **`lastPlayed`.** The latest play replaces the old time (it is not a maximum, so one play
  with a clock that was ahead cannot pin a track). It is kept in memory and written
  with the manifests, not at every play. A wall time before 2020-01-01 (a clock that was
  never set) is ignored.

**Device check (manual):** download an album, pull the Wi-Fi, and it still plays
from Streaming → Downloaded and from "Now playing" after a restart. "Where am I?"
during a download says the progress. A streamed album plays offline the second time.

Known limits after phase 3: there is no Wi-Fi-only check (decided: not now, the device
has only Wi-Fi). A server that sends no `Content-Length` and a stale listed size can
leave a file of the wrong size. Re-downloading a list deletes the file of a dropped
track even if the player has it open (harmless on POSIX, the open file stays readable).
Scrobbles made offline wait in the scrobble outbox, `scrobbles.tsv` (fixes.md 3.2, built: sent with `time=` when the server answers again; the front ends give the server up to 3 s to finish at exit, fixes.md 3.1). The manifest's `changed` column holds no changed time
(the track data has none), so a track changed on the server is not fetched again; since
fixes.md 2.6 it is empty or `partial`, the mark of a download that is not over.

## Phase 4: one library (local files and server)

Goal: Music lists local albums and server albums together, by artist, album and genre,
with one entry when an album is both on the device and on the server. Start-up does not
rescan or wait for the server. Browsing works offline. Audiobooks on the server are
listed in Library, keep their place and resume like local books.

Decided for this phase:

- **Library index** in place of the `scanLibrary()` calls in the menus:
  `Item { id, source: local|server|cached, kind: book|music|podcast, title, artist, album, tracks, seenOn }`.
  It is kept on disk, so start-up does not rescan and browsing works offline. It is
  stored in **SQLite** (`library.db` next to `state.tsv`, as SPEC §4.7 says). Core holds the
  index in memory and never sees SQLite: it loads and saves through a `LibraryStore`
  interface in `services.h`. `SqliteLibraryStore` lives in a new library `app/library/`
  (`oaplibrary`). Writes follow the flash rule: only what changed, in one transaction,
  and play times only on exit and sleep.
- **Where an item was seen.** Each item and each track keeps `seenOn`, a list of server
  references `<server key>:<kind>:<id>`. The server key is `TrackCache::serverKey(url,
  user)`, the key the cache folders already use. The kind is `album`, `playlist` or `track`, and the
  id is the server's id (for example `ab12cd:album:3a1f9c`). A download gets the
  reference of the server it came from, and a local album that matches a server album
  (4.8) gets that album's reference. A direct check asks "is this on server X, and with
  which id?". It needs no name matching, and the server copy of a local or downloaded
  album can be opened, merged again after a sync, or dropped when it leaves the server.
  A local item with no match has none. No reference holds a URL or a token.
- **Books stay folders.** A local book is still one folder (or one file) under the
  library root, titled by its folder. Only music is grouped by its tags. `podcast` is
  reserved for later and nothing sets it in this phase.
- **Scans and syncs run in the background.** The menus use the saved index at once, and
  a new scan or sync replaces it when it is done. It is saved only when it changed, so
  start-up writes nothing when nothing changed.
- **Server sync** runs once at start-up, silently, and from a "Refresh library" item.
  A sync that starts when Wi-Fi connects needs a network state from the platform
  ([hal.md](hal.md), not built), so it comes later. The cheap check is `lastModified` from
  `getArtists`. Only when that changed is the album list (`getAlbumList2
  alphabeticalByName`, 500 per page) fetched again. The tracks of a server album are not
  stored in the index: opening the album loads them, as Streaming does today.
- **Duplicates:** when a local album and a server album are the same (MusicBrainz album
  id, or else artist + album + track count, with case and spaces ignored), one entry is
  shown and the local files are preferred, then a download (`cached`), then the server.
- **Where it plays from** is spoken only when asked: an "Album info" action in the
  Options of an album (queue.md step 2.5).
- **Play all** in Music plays what is on the device (local and downloaded), never the
  whole server.
- **Streaming** stays a separate Home entry for now. It may later become a filter
  ("Server only") inside Library and Music, once the merged views work well.

| # | Step | Tests (each checked with a sabotage, as CLAUDE.md requires) |
|---|------|------|
| ✅ 4.1 | **Navigation contract first** (docs only): navigation.md and SPEC §5 get the new Music tree (Play all · Artists · Albums A to Z · Genres · Recently added · Recently played · Refresh library), "Album info", server books in Library, and what Refresh says. SPEC §4.7 names `library.db`. | – |
| ✅ 4.2 | **`LibraryIndex` in core** (`app/core/library_index.{h,cpp}`): items and tracks as above, plus per track the location, title, artist, number, duration, size and file time, and per item the genre, year, MusicBrainz album id, the wall time it was first seen and the time it was last played. Items and tracks carry `seenOn` references (see above), with lookups both ways: from a reference to the item or track, and from an item to its reference on a given server key. Load and save go through a `LibraryStore` interface in `services.h` (load everything; save the items that changed and remove the ones that went). `FakeLibraryStore` in `tests/fakes.h` keeps it in memory and counts the saves. No menu change yet. | `tests/test_library_index.cpp`: an unchanged index saves nothing; one changed item saves only that item; a removed item is removed from the store; lookups by id and by path; an item with references to two servers is found by each and round-trips both; a reference is found for its own server key only; adding the same reference twice keeps one. |
| ✅ 4.3 | **`SqliteLibraryStore`** in `app/library/` (`oaplibrary`, links SQLite): a schema with a version table (items, tracks, references), a save in one transaction, WAL off, and `synchronous=FULL`, so a power cut leaves the old data. A database of an unknown version or a damaged one is moved aside and the store starts empty (the next scan and sync fill it again). Front ends take `--library-db FILE` (device: `/data/config/library.db`; elsewhere next to `state.tsv`). Build: SQLite from the system (`libsqlite3-dev`, also arm64) or else fetched as the amalgamation like yaml-cpp, so the core-only build and its tests need no system package; both Docker targets get the package. | `tests/test_sqlite_library_store.cpp` on a temp dir: round-trip of items, tracks and references with non-ASCII, quotes and newlines in titles; saving one item leaves the others unchanged; removing an item removes its tracks and references; a damaged file and a newer schema version open as empty without a crash and are moved aside; the file is mode 0600. `docker build --target native` and `--target pi` pass, and so does the core-only configure. |
| ✅ 4.4 | **Tags of local files.** A `TagReader` interface in `services.h` (title, artist, album artist, album, track and disc number, genre, year, duration, MusicBrainz album id; none for a file that cannot be read). `FfmpegTagReader` in `app/media/` reads the container metadata (ID3, Vorbis comments, MP4 atoms; keys compared without case). `FakeTagReader` in `tests/fakes.h`. | Media tests on small files the test writes with tags (MP3, FLAC, M4A), including Cyrillic and emoji. A file with no tags gives the duration and empty tags. A file that is not audio gives none. |
| ✅ 4.5 | **Background scan.** A `LibraryScanner` interface (asynchronous, cancellable, results through `poll()`, like `MusicServer`), and `ThreadedScanner` in `app/frontend/` that walks the library and music roots on its own thread, skips the cache folder, keeps the entries whose path, size and file time did not change, and reads tags (through `TagReader`) only for new or changed files. Both front ends wire it. `FakeLibraryScanner` in `tests/fakes.h`. | On a temp folder with `FakeTagReader`: unchanged files are not read again; a changed file is; a removed file leaves the index; the cache folder is never entered. After cancel the callback does not run. A file whose tags cannot be read is still listed, with its folder and file names. |
| ✅ 4.6 | **Library and Music read the index.** `App` loads the saved index at start-up, starts a scan and no longer calls `scanLibrary()` from the menus. A finished scan replaces the local items and saves the index if it changed. Music albums are grouped by album artist + album tag; files without tags are grouped by folder as today. The Music menu keeps its current shape until 4.9. | With a saved index and no scan delivered yet, Library and Music list it. A book added on disk appears after the scan is delivered. Two folders tagged as one album give one album; a folder of untagged files is one album with its folder name. A scan that changes nothing writes no index. The existing Library and Music tests pass. |
| ✅ 4.7 | **Server catalogue in the index.** `RemoteAlbum` gets `genre`, `year`, `created` and `musicBrainzId`. `MusicServer` gets `catalogueVersion(done)` (`getArtists` `lastModified`). At start-up and from Music → Refresh library, `App` checks it, fetches all albums when it changed, and replaces the server items. Each server item gets its `seenOn` reference (`<server key>:album:<id>`). Items from another server key are dropped, and so are references to that key on local items. The start-up sync is silent. Refresh says "Updating the library." and then the result or the error. Refresh also rescans the local files. | Net tests: `lastModified` and the new album fields are parsed from the fixtures, and missing fields are empty. App tests: an unchanged `lastModified` fetches no album list; a changed one replaces the items, and an album the server dropped is gone; a sync error at start-up says nothing and the saved items stay; Refresh speaks the result and the error; items saved for another url or user are not listed. |
| ✅ 4.8 | **Duplicates and downloads.** Merge local, server and downloaded (pinned in `TrackCache`) albums by the rule above into one entry with the preferred source. A download takes its reference from its manifest (server key, kind, list id, track ids). A merged entry keeps the server's reference, and the next sync matches it by that reference first and uses the name rule only for items with no reference. When an album leaves the server, its reference is removed and the local or downloaded entry stays. "Album info" at the end of an album's list says where it plays from ("On this device", "Downloaded", or the server's name), and "Also on the server" for a merged entry. | Same album local and on the server: one entry, and playing it opens local files and no stream URL. The same name with another track count gives two entries. A matching MusicBrainz id merges different spellings. A downloaded server album is `cached`, plays its files, and carries the reference of the server it came from. After a merge, a sync where the server renames the album keeps one entry, matched by reference. An album removed from the server leaves the local entry with no reference. Album info names each source. |
| ✅ 4.9 | **Music → Artists and Albums A to Z across sources.** Music becomes: Play all, Artists → albums, Albums A to Z, then the later items. A server album opens with "Loading" and its tracks, as in Streaming (same errors, same "Play album" first item). Offline, a server-only album says "Server not reachable", and its tracks are not listed. Artists are sorted by name, ignoring the server's `ignoredArticles` ("The"), and include Cyrillic and other non-Latin names. | Artists list local and server artists once each, sorted. An artist's albums include both sources. A local album plays at once; a server album shows Loading, then its tracks. Server unreachable: a server album gives the error item, a local album still plays. Play all plays only device files. The Music tests from 4.6 are rewritten for the new tree. |
| ✅ 4.10 | **Genres, Recently added, Recently played.** Genres from tags and from the server's album genre. Recently added is newest first: the first-seen time for local albums, `created` for server albums. Recently played is the last-played time an album gets when it plays, kept in memory and written with the index on exit and sleep, like the state. | A genre lists albums from both sources. Recently added orders a new local album and a new server album by time. Playing an album puts it first in Recently played after a restart. Playing writes nothing to disk until exit or sleep. A wall time before 2020-01-01 is ignored, as in the cache. |
| ✅ 4.11 | **Audiobooks from the server.** `MusicServer::musicFolders(done)` (`getMusicFolders`) and a `musicFolderId` on the album list. `server.audiobook_folder` in `config.yaml` names a folder, and sync gives its albums `kind: book`. They are listed in Library after the local books (with "on the server" as the value), not in Music. They open as books: chapters are the tracks, the place is kept per server key + album id in `state`, and it is resumed when the book is opened again. Fetched with "Loading", as in 4.9. | Net tests: the folders are parsed, and the album list URL carries `musicFolderId`. With the folder set, its albums are in Library and not in Music; without it nothing changes. Opening a server book says "Chapter", and pause, another book, then this book again resume the place. No `pos:` key is ever made from a URL, and nothing spoken or saved contains one. |
| ✅ 4.12 | **Resume a server book.** `last_book` can name a server book, so "Now playing" on Home and a restart resume it like a local book. With the server down, a downloaded book plays from its files; otherwise "Server not reachable." is said and Home stays. Opening a server book removed `last_stream.*` (queue.md step 1.5 stopped that: the players keep their places apart). | Exit and restart resume the server book at its chapter and place. Server down: a downloaded book plays offline, and one that is not downloaded gives the error. A local `last_book` and a `last_stream` still resume as before. |
| ✅ 4.13 | Docs: architecture.md (index, scanner, tag reader, sync, merged views, server books), navigation.md where the build differs from 4.1, SPEC, and what phase 4 built differently. | – |

**Device check (manual):** with the test server set up, the first start lists Music
from local files, and the server albums appear within a minute without anything being
said. A restart lists everything at once, with no scan wait. An album that is both local
and on the server is listed once and plays without network traffic. With Wi-Fi off,
Music still lists everything, a local album plays, and a server album says "Server not
reachable". A book in the server's audiobook folder is in Library and resumes after a
restart. Refresh library after an album is added on the server lists it.

### Phase 4 as built

The plan above is kept as it was written. Where the code differs, the code is right:

- **The index.** `LibraryIndex` (`app/core/`) is the whole library in memory; `App` loads it
  through `LibraryStore` when it is built and saves only the items that changed, in one
  transaction. `library.db` has schema version 2 (4.8 added the track count and `created`); a file that
  is damaged, not a database or of another version is renamed to `library.db.bad` and the store
  starts empty, so the next scan and sync fill it. `--library-db` defaults to `library.db` next
  to `state.tsv` (so `/data/config/library.db` on the device). A scan or sync that changes nothing writes nothing.
- **Scan.** `ThreadedScanner` (`app/frontend/`) lists the roots with `scanLibrary()`, reuses
  an entry whose path, size and whole-second file time are unchanged, and reads tags through
  `FfmpegTagReader` (which lowers FFmpeg's log level to errors) only for the rest. Items:
  `local:book:<path>` (a book per folder or loose file under the library root, titled by the
  folder), `local:album:<album artist or artist>\x1f<album>` for tagged music (an album in
  several folders is one item; with no artist at all the id ends in `\x1f<folder>`, so such an
  album is one per folder). Files of one album whose ids differ are then one item when the
  album is the same with case and spaces folded and: the artist is the same and not empty, or
  they share a folder and one artist is the other with more words ("Band feat. Guest"), or they
  share a folder and one has no artist while the others there are one album. Two album artists
  in one folder stay two albums. The joined item keeps the id of its part with the most tracks
  that the index had (else the first id), the artist with the most tracks, the server
  references of every part and their latest last-played time) and `local:folder:<path>` for music without an album tag
  (grouped by its folder under the music root). Lists are built when opened and are not
  refreshed live; until the first scan is done an empty Library or Music says "Scanning".
  `last_book` of a book the index does not know yet is looked up with `scanLibrary()`.
- **Sync.** Start-up and Music, Refresh library run the same sync; Refresh also rescans and
  joins a scan or sync that is already running. The albums are fetched again only when
  `lastModified` of `getArtists` differs from `catalogue.version` in `state.tsv` (kept with
  `catalogue.server`, and `catalogue.folder` for the audiobook folder), or the index holds no
  server item of that key. The version is stored only after a save that worked. Items from
  another server key are dropped. The start-up sync is silent, also on an error.
- **Merged views.** Nothing merged is stored. A local or downloaded album gets the server
  item's reference in its `seenOn` (by reference first, by the name rule only for items with no
  reference), and `mergeAlbums()` builds the merged entries whenever a menu is built. A
  download is a `cached:<ref>` item made from the manifests of `TrackCache` each time Music
  is built, and it plays as local files.
- **Texts.** Album info says "On the device.", "Downloaded." or "On the server.", then "Also on
  the server.", the artist and the track count (not "On this device" or the server's name).
  Refresh says "Updating the library.", "Library updated, N albums." (an album on both counts once),
  "Still updating." or the error.
- **Music.** Artists sort without a leading "The" only: the server's `ignoredArticles` is
  not read. Cyrillic and other scripts come after Latin names, and an album with no artist is
  under "Unknown artist", last. Genres are merged without case or spacing; albums with no
  genre are left out. Recently added and Recently played list 50 albums at most; times
  before 2020-01-01 are ignored. A play time is set in memory when an album starts from Music
  (not Play all, Streaming or Downloaded) and written only on exit and sleep. A server-only
  album opens like one in Streaming (Loading, Play album, Download album, tracks) with Album
  info last.
- **Server books.** `server.audiobook_folder` is the exact name of a music folder. The sync
  asks `getMusicFolders`, fetches that folder's album ids, then the whole catalogue, and the
  albums in the folder are `Kind::Book`; a name the server does not have makes no books and
  the sync is not remembered. They are listed in Library after the local books with the value
  "on the server", never in Music and never merged with a local or downloaded copy (a book
  that is on both is listed twice). The place is `pos:<server key>:album:<id>`, with no URL.
- **Resume a server book.** `last_book` is the book's item id `<server key>:album:<id>` and is
  set only once the book plays (a failed open changes nothing). The music player keeps its own place
  (first `last_stream.*`, now the queue in `queue.tsv`, queue.md step 2.8), and `last_player` says which player was last. A book that is whole on disk plays at once, with no "Loading." and
  also offline. Any other says "Loading." and asks the server; with it down "Server not
  reachable." is said and Home stays. Unlike a stream, a partly downloaded book does not fall
  back to the files on disk, since its chapter numbers would shift.

Known limits after phase 4: Streaming is still a separate Home entry. There is no sync
when Wi-Fi connects (the platform gives no network state, [hal.md](hal.md)), only at start-up and from
Refresh. A server album's tracks are not in the index, so offline only a downloaded or device
album can be opened. A catalogue stops at 200000 albums (an error). The albums are fetched
from `getAlbumList2`, so only an album-level genre is known. A tag edit that keeps a file's size
and whole-second time is not noticed by the scan; the disc number is not stored, so a
multi-disc album is ordered by folder, then track number. A play time sits on one item: if
the item is removed and comes back under another id (a moved folder, a deleted and renewed
download) the album is not in Recently played any more. An album first seen while the clock
was not set has no first-seen time and is left out of Recently added until a scan with a set clock
fills it in. A downloaded album with no server item (the server is not reachable, or dropped
the album) plays from Music as a local album: no scrobble, no read-ahead, and it is resumed as local music
from `queue.tsv`, not from the server. Refresh fetches the albums again only when the server's `getArtists`
`lastModified` changed, so a server that does not bump it for an edit is not noticed until it
does. A book that is both on the device and on the server is listed twice in Library. The destructor of the scanner joins its thread, so
a read stuck on a hung mount can delay exit (the state and the play times are saved first). A download of a book that left the index is listed in
Music as an album, and "Now playing" for it goes. Local music played after a server book
leaves `last_book` alone, so a restart resumes the book. The manifest's `changed` column is
still empty.


## Phase 5: sync and playlists

Goal: edit playlists on the device, keep selected playlists offline, and sync book
positions and the music queue with the server. Failed book progress saves retain the
existing retry behaviour; other offline edits are deferred.
Local files are **not uploaded**; moving them to a server is outside this plan.

The design below records the phase 5 choices. Book progress follows the existing
Audiobookshelf behaviour in navigation.md and SPEC §6.3.

| # | Step | Tests (each checked with a sabotage, as REPO.md requires) |
|---|------|------|
| ✅ 5.1 | **Navigation and design.** Resolve the phase 5 design questions, then document menus, keys, spoken feedback, conflict handling, persistence and server capabilities in this plan and navigation.md. Keep existing playback usable throughout the following steps. | Review the design against SPEC, existing navigation and the implemented server interfaces. |
| ✅ 5.2 | **Playlist server operations.** Add asynchronous server interfaces and Subsonic requests for adding tracks, removing tracks and deleting playlists; retain the existing create operation. Handle unsupported providers and errors. | Net tests check request parameters, ordering and response errors; fake-server tests cover success, failure and cancellation. |
| ✅ 5.3 | **Local playlists.** Read and write local-only `.m3u` playlists under `/data/library/playlists/`, with an injectable root for tests. Provide create, add, remove and delete operations through the agreed interface. | Temp-directory tests cover round trips, track order, duplicate entries, removal, deletion and failed writes; local files remain local. |
| ✅ 5.4 | **Playlist editing menus.** Expose create, remove and delete for supported playlists and add the current track from the Player menu, reopening the last playlist used. Delete requires a Cancel-first confirmation. | App tests exercise each action and its feedback, last-used selection, confirmation cancellation, server errors and local versus server track eligibility. |
| ✅ 5.5 | **Keep playlists offline.** Persist the keep-offline choice, pin playlist tracks in the cache and reconcile them with later server changes without removing pins needed by other downloads. | Tests cover enabling and disabling, restart, added and removed tracks, overlapping pins, failed refresh and insufficient space. |
| ✅ 5.6 | **Subsonic book progress.** Implement the existing `MusicServer` progress interface with Subsonic bookmarks per track, applying the agreed timestamp and chapter mapping. Reuse the existing book-player offer and progress persistence. | Net tests cover bookmark requests, timestamps, missing and malformed data and errors; App tests cover local and newer server positions, chapters, pause, stale callbacks and offline retry. |
| 5.7 | **Play queue sync.** Add asynchronous `savePlayQueue` and `getPlayQueue` support with `indexBasedQueue`, and integrate it with the music queue using the agreed conflict and mixed-source rules. | Net tests cover request and response mapping including duplicate tracks and position; App tests cover a phone queue, a device queue, restart, offline and errors, and unsupported or local-only tracks. |
| 5.8 | **Docs for phase 5 as built.** Update architecture.md, navigation.md, SPEC and this plan to describe the implementation and its limits. | – |

**Device check (manual):** With a configured test server and a second client, create
and edit a playlist on the device and verify its contents on the server; change a
kept-offline playlist on the server, refresh it and play it with Wi-Fi off. Change a
book's position and a music queue on the second client and check the device's sync
behaviour. Make book progress offline, restart and reconnect, and verify it reaches the
server. Check the corresponding spoken feedback and Cancel on delete.

### Phase 5 design

- A playlist has a generated name; editing names waits for [input-methods.md](input-methods.md).
  Save the queue as a playlist using the existing timestamp name. Creating an empty
  playlist uses "Playlist, 27 October 14:05" with the same wall-clock rules as Save
  as playlist. Playlist names are labels, not identifiers: local files use unique,
  safe generated filenames and server playlists use server key and id.
- Music → Playlists lists local `.m3u` files alongside server and downloaded
  playlists. Distinct playlists stay distinct, even if their names match. Local files
  store ordered local paths; duplicate entries are allowed. Adding a server track to
  a local playlist is unavailable. A server playlist accepts only tracks with an id
  for that server; local-only tracks and tracks from another server are skipped,
  with the count spoken. No file is uploaded.
- Music → Playlists offers Create playlist and existing playlists. Create opens a
  destination list with "On the device" first and each configured playlist-capable
  server in configuration order; choosing one creates the empty playlist. A playlist opens
  with Play playlist, followed by its tracks and actions to delete it and, for a
  server playlist, Keep offline / Stop keeping offline. Options on a track offers
  Remove from playlist; for a server playlist it first fetches the current ordered tracks
  and the user selects the occurrence to remove, including duplicates. A fetch error uses
  the existing server error feedback. Deletion opens a confirmation with Cancel first. The music
  Player menu adds Add to playlist, which opens on the last used eligible playlist;
  with no last selection it opens on the first. A successful add or removal says
  "Added to playlist." or "Removed from playlist."; deletion says "Playlist deleted."
  and cancellation says "Cancelled." A server operation says "Saving." while it
  waits; failures use the existing server error messages and leave the list unchanged.
  Unsupported actions say "Not available." Local write failures say "Could not save
  playlist." Creating a local playlist is available offline; creating or editing a
  server playlist requires a reachable server in this phase.
- A kept playlist owns a pin for each referenced server track. Each download and
  each kept playlist has its own ownership; removing a track, stopping offline
  keeping or deleting a playlist releases only that owner's pin. A track remains
  pinned while another owner needs it. Persist owners across restart. A successful
  refresh reconciles tracks in server order, schedules new tracks at Download quality
  and releases removed tracks; a confirmed server deletion releases that playlist's
  pins. An error leaves the previous contents and pins intact.
  Existing download space and progress feedback applies; a failed or incomplete
  transfer remains eligible for the next refresh. Cached audio is not removed merely
  because its last pin was released: normal cache eviction may remove it later.
- Subsonic books are separate from playlists. Reuse Audiobookshelf's book timeline,
  pull/offer, save scheduling, dirty markers and startup retry, as documented in
  [audiobookshelf.md](audiobookshelf.md#phase-3-as-built). Subsonic bookmarks are per
  track: map track positions to the book timeline using `book()` track order and
  durations, and select the newest valid bookmark for a track of that book. Save
  the current track and its offset with `createBookmark`; `getBookmarks` provides
  timestamps. Ignore malformed, unknown-track or unmappable bookmarks. Playback and
  pending saves must not wait for a network request on the UI thread.
- Sync queues per configured server, using only entries with a track id for that
  server and preserving order and duplicate entries. Compare the server's `changed`
  timestamp with the local queue's last change or place timestamp; the newer wins,
  equal timestamps leave the local queue alone. Fetch at startup and before sending
  a pending local queue, so an older offline queue cannot overwrite a newer server
  queue. A failed fetch keeps the local queue and defers sending. A newer server
  queue replaces the saved music queue paused when music is not playing; it must not
  interrupt an active book. While music plays, retain a newer server snapshot pending
  until the player is left, and discard it if the local queue changes in the meantime.
  Discard stale responses after any local change. Use wall time through
  `clock_`; an unset clock cannot establish that a local edit is newer. Persist
  pending sync with the queue at exit and sleep. Server-only queues are synced only
  with their own server; mixed queues send each server's eligible entries and skip
  all others, reporting omissions when a sync is user initiated. Queue syncing uses
  [indexBasedQueue](https://opensubsonic.netlify.app/docs/extensions/indexbasedqueue/)
  `getPlayQueueByIndex` and `savePlayQueueByIndex` when supported; without the
  extension use ordinary queue calls only when the current track is unambiguous.
- General offline replay for stars and playlist edits is deferred. Their existing
  online actions keep their existing failure feedback; they do not claim success
  or save an offline edit. The existing scrobble outbox and book progress retry
  remain separate.

---

## Decisions

- New libraries libcurl and yaml-cpp: **yes**. yaml-cpp reads both the config and the JSON answers.
- The library index (phase 4) is stored in **SQLite**, behind a `LibraryStore` interface, so core stays free of it.
- **One config file**, `config.yaml`, for menu settings and the server. Positions stay in `state.tsv`.
- **State is written only on exit** (and sleep), to spare the flash. A crash or power loss loses the place since the last exit.
- `.env` only stores the test credentials for development; the program does not read it.
- yaml-cpp is slow at parsing JSON, and that is **acceptable**.
- Subsonic is the **first** server type. Audiobookshelf for books is the second, behind the
  same interface ([audiobookshelf.md](audiobookshelf.md)).
- Streaming is a **separate Home entry** for now and may be merged into Library and
  Music later.
- Streams default to the **original file**. Streaming quality in Settings can pick a
  transcode (320 / 192 / 128 kbps) for slow links.

## Open questions

1. Should more than one server be supported (`servers:` as a YAML list)? **Answered: yes.**
   `servers:` is a list, each entry has its own `type` and its own Home entry, and the old
   `server:` section is read as a list of one. See [audiobookshelf.md](audiobookshelf.md)
   (phases 1 and 2, built). This plan still describes one server.
2. Should the password stay in plain text in `config.yaml` (mode 0600), or should
   the device keep only `t`/`s` (a token that never expires, so it is just as
   sensitive)? Navidrome API keys would be better once the server offers
   `apiKeyAuthentication`.
3. TLS on the device: check that the Pi image trusts the server's certificate. A
   self-signed home CA would need `server.ca_file` in `config.yaml`.
