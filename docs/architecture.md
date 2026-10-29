# Software Architecture

> Status: describes the current code (simulator on macOS and Linux, headless Linux front end,
> streaming from a Subsonic server, with a local cache and downloads, and one library of local files and server albums: phases 1 to 4 of [streaming.md](streaming.md)),
> plus phases 1 and 2 of [queue.md](queue.md): volume and Options buttons, the step unit, menus that jump, separate music and book players,
> and the music queue (local and server tracks mixed, skipped when unavailable, a server that fails checked until it answers), Options (Play now, Play next, Add to queue, Go to album, Album info),
> Shuffle and Repeat (off, all, album, track; Add to queue mixed into a shuffled queue), the Queue list, the queue kept in `queue.tsv` and Save as playlist, and, from [fixes.md](fixes.md) phase 2, playlists in `library.db` and in Music, Download in Options and the player's name in "where am I?". Download quality, caching at the opened stream quality and optional cached-copy upgrades with local fallback are built (fixes.md phase 5); real-server listening checks remain pending. Star is not built (streaming.md 2a.1). Several servers, Audiobookshelf books, progress sync and Continue listening (audiobookshelf.md phases 1–3) are implemented; phase 3 real-server and device checks are pending. Last updated: 2026-10-29.
> For product goals, hardware and the overall plan see [../SPEC.md](../SPEC.md).

## 1. Overview

The software is one C++20 process per running front end, in layers. It has worker
threads for audio, decoding, scanning and network I/O, but no interprocess bus.
A front end wires the layers together;
core depends on none of them:

```
┌───────────────────────────────────────────────────────────────────────┐
│ Front end (per target)                                                │
│   app/sim/       desktop simulator: SDL2 window, Dear ImGui, keyboard │
│   app/headless/  device: evdev buttons, terminal keys, no screen      │
│   app/frontend/  shared: scripted presses, terminal keys, gamepad map │
│                  makeServers(): Subsonic / Audiobookshelf per server │
│                  ThreadedScanner: background scan of the files        │
├───────────────────────────────────────────────────────────────────────┤
│ Core  (app/core/, plain C++, no dependencies, library "oapcore")      │
│   App ── menus, player screen, power states, settings                 │
│    ├── GestureDetector   press/release → short / long press           │
│    ├── Library           the files on disk as books and music         │
│    ├── LibraryIndex      books, albums, server items in memory,       │
│    │                     saved through LibraryStore, merged views     │
│    ├── Settings          dotted keys in memory, SettingsStore for I/O │
│    └── services.h        Speech · MediaPlayer · SoundEffects ·        │
│                          VolumeControl · Clock · MusicServer ·        │
│                          Downloader · TextFiles · LibraryStore ·      │
│                          LibraryScanner · TagReader (interfaces)      │
│   TrackCache             downloaded and cached tracks, manifests      │
│   Earcons                synthesized  → effects stream                │
│   Mixer                  content + speech + effects → PCM, ducking    │
├───────────────────────────────────────────────────────────────────────┤
│ Config  (app/config/, library "oapconfig", yaml-cpp)                  │
│   YamlStore : SettingsStore   config.yaml (settings, server)          │
│   TsvStore  : SettingsStore   state.tsv (positions, what to resume)   │
│   DiskTextFiles : TextFiles   cache manifests, written atomically     │
├───────────────────────────────────────────────────────────────────────┤
│ Library  (app/library/, library "oaplibrary", SQLite)                 │
│   SqliteLibraryStore : LibraryStore   library.db, one transaction     │
├───────────────────────────────────────────────────────────────────────┤
│ Net  (app/net/, library "oapnet", libcurl + yaml-cpp)                 │
│   Subsonic : MusicServer   URLs with token auth, JSON answers         │
│   Audiobookshelf : MusicServer   Bearer key, books and chapters       │
│   HttpClient               libcurl on one worker thread, timeouts     │
│   HttpDownloader : Downloader   track files to *.part, own thread     │
├───────────────────────────────────────────────────────────────────────┤
│ Media  (app/media/, library "oapmedia")                               │
│   Player : MediaPlayer   FFmpeg decode → content stream               │
│   FfmpegTagReader : TagReader   tags of local files                   │
│   Tts    : Speech        eSpeak NG    → speech stream                 │
├───────────────────────────────────────────────────────────────────────┤
│ Platform  (app/platform/, library "oapaudio")                         │
│   AudioOutput            CoreAudio · PulseAudio · ALSA · null         │
└───────────────────────────────────────────────────────────────────────┘
```

Only `app/platform/`, `app/sim/` and `app/headless/` know which OS they run on.
The two front ends wire the same core, config, net, media and audio code together;
they differ only in where button presses come from and whether there is a window.
Config and net read YAML and JSON with yaml-cpp, net uses libcurl and library uses SQLite; core sees
none of them, only `SettingsStore`, `TextFiles`, `LibraryStore`, `MusicServer` and `Downloader`.

`App` has no audio code. It only sees the interfaces in `services.h`, and the
front end wires in the real implementations (`Tts`, `Player`, `Earcons`, `Mixer`,
`SystemClock`, `DiskTextFiles`, and from `makeServers()` a `Subsonic` or `Audiobookshelf` plus an
`HttpDownloader` per supported entry in `config.yaml` when built with `OAP_BUILD_NET`, else no server at all:
`App` then uses a `NoMusicServer` and a `NoDownloader`).
The unit tests wire in fakes instead (section 12).

## 2. Source layout

| Path | Contents |
|------|----------|
| `CMakeLists.txt` | Builds `oapcore`, `oapconfig`, `oapfrontend`, `oaplibrary` (SQLite from the system, else the amalgamation is fetched), `oapnet` (`OAP_BUILD_NET`, libcurl via pkg-config; on by default when a front end is built), `oapmusicserver` (`makeServers()`, linking `oapnet` when it is built), `oapmedia`, `oapaudio` (backends from `OAP_AUDIO_BACKENDS`), `imgui` and `oap-sim` (`OAP_BUILD_SIM`), `oap-headless` (`OAP_BUILD_HEADLESS`, Linux), plus `oap-tests`, `oap-net-tests` (with `OAP_BUILD_NET`) and `oap-media-tests` (with a front end) (`OAP_BUILD_TESTS`). ImGui and GoogleTest are fetched at configure time, and yaml-cpp and SQLite too when the system has none. |
| `app/core/app.{h,cpp}` | `App`: the UI state machine and all menus. |
| `app/core/buttons.{h,cpp}` | `Button`, `Gesture` (`Short`, `Long`, `Deep`, `Repeat`, `Light`, `Release`, `Cancel`), `GestureDetector` (with `kRepeatDelay` 400 ms and `kRepeatInterval` 150 ms for the volume keys). |
| `app/core/menu.h` | `MenuItem` (no `adjust` part: a value is changed only in its list), `ListOrder` (`None`, `ByName`, `ByArtist`: how a list is sorted, for the letter jump), `Screen` data model. |
| `app/core/services.h` | Interfaces `App` depends on: `Speech`, `MediaPlayer`, `SoundEffects`, `VolumeControl`, `Clock` (+ `SystemClock`), `TextFiles` (whole text files for the cache manifests: `read()` and `writeAtomically()`, which leaves the old or the new contents after a crash; core does no file I/O of its own), `TagReader` (tags and length of a local audio file, `TrackTags`, none if unreadable; callable from any thread; `FfmpegTagReader` in `app/media/`, whose constructor lowers FFmpeg's log level to errors), `LibraryScanner` (`ScanRequest` in, local `IndexItem`s out, asynchronous like `MusicServer`: `scan()` / `cancel()` / `poll()`; `ThreadedScanner` in `app/frontend/`), and `Downloader` (queues a track download into `<dest>.part`, renamed when complete; the same `RequestId` / `poll()` / `cancel()` contract as `MusicServer`, results `Ok` / `NotConfigured` / `Unreachable` / `Server` / `Disk`; `App` takes one for Download album / playlist), and `MusicServer` (with `musicFolders(done)` and `albums(..., musicFolderId)`, and `catalogueVersion(done)`: a token that changes when the server's catalogue does, empty when it says none) with its `RemoteTrack` (with the original's `suffix` and `size`) / `RemoteAlbum` (with `genre`, `year`, `created`, `musicBrainzId`) / `RemoteArtist` / `RemotePlaylist` structs, and `book(albumId, done)` (a `RemoteBook`: the tracks and the `RemoteChapter`s the server lists, none for Subsonic, whose `book()` is `getAlbum`) and `catalogue()` (`Catalogue::Music` or `Books`; Subsonic and `NoMusicServer` say `Music`), and `createPlaylist(name, trackIds, done)` (a new server playlist; at most `kMaxPlaylistTracks` = 150 ids, as they all go in the GET URL; `done` gets only an `Error`) (asynchronous: a request returns a `RequestId`, its callback runs from `poll()` on the UI thread, and `cancel(id)` drops it). `syncsProgress()` says whether progress sync is supported. `progress(albumId, done)` returns an optional `RemoteProgress` (seconds on the book timeline, duration, finished, updated in wall ms); `saveProgress(albumId, progress, done = {})` optionally acknowledges completion from `poll()`. `inProgress(done)` returns the selected book library’s in-progress books. Subsonic answers unsupported progress requests with `Server`; `NoMusicServer` answers `NotConfigured`. `App` takes a `std::vector<ServerLink>` (`ServerLink`: the server's `ServerConfig`, its key `TrackCache::serverKey(url, user)`, its spoken name, its `MusicServer&` and its `Downloader&`; the objects belong to the front end and outlive `App`). With an empty list it behaves as with a server that is not configured. Browsing (step 1.4) and playing (step 1.5: streams, scrobbles, read-ahead, the automatic cache, the marked-down state) use the link of the list or of the queue entry; downloads (step 1.6) and the catalogue (step 1.7) use the link of the list or of the item too. Its identity in saved state (queue entries, the cache folder, `catalogue.*` keys) comes from the link, not from `server.*` in the settings. The Streaming menu asks the `MusicServer` for lists, `tick()` calls its `poll()`, and opening a remote track asks it for `stream(id, maxBitRate)`: the URL, and for a transcode a `Transcoded` (offset URL, and the length `App` fills in from the listing) that `MediaPlayer::open()` takes. |
| `app/net/http_client.{h,cpp}` | `Http`: asynchronous GET and JSON PATCH (`patch(id, url, headers, body, handler)`, with `Content-Type: application/json`) with optional extra request headers (`Headers`, "Name: value" lines; values can be secrets, so they are never logged; a request with headers does not follow redirects, and a line break in one fails it; `Subsonic` passes none) whose handler runs on the client's thread (tests use a fake). `HttpClient`: libcurl multi on one worker thread; 10 s connect timeout, 10 s without data or 60 s in all ends a request, answers over 16 MB fail; cancellable; the destructor aborts transfers and never waits on a socket. `finish(deadline)` (also on `Http`, a no-op in fakes) is the way out at exit: it rejects new requests, waits until the queued and running ones are answered or the deadline has passed, then aborts the rest. |
| `app/net/subsonic.{h,cpp}` | `Subsonic : MusicServer`: API 1.16.1, token auth (`u`, `t` = md5(password + salt), a fresh random `s` per request, never `p`), percent-encoded URLs, `stream()`: the original file with no `format` / `maxBitRate`, or an MP3 transcode (`format=mp3&maxBitRate=<kbps>`) whose offset URL adds `&timeOffset=` once `getOpenSubsonicExtensions` (asked with the first transcode) lists `transcodeOffset`. `catalogueVersion()` sends `getArtists` and gives its `lastModified` as text. `createPlaylist()` sends `createPlaylist?name=&songId=...` (one `songId` per track; more than `kMaxPlaylistTracks` fail with `Server` and send nothing; a failure is never logged). `scrobble(id, submission, timeMs, done)` sends `scrobble?id=&submission=` (and `&time=<ms>` when `timeMs` is set), logs a failure to stderr (by its kind, never the URL) and gives the error to `done` (may be empty; not called when no server is set up). `finish(deadline)` forwards to `HttpClient::finish()` (not on `MusicServer`); the answers to scrobbles that come in during it are delivered to their callbacks (no other callback runs), so the outbox drops what the server took. `downloadUrls(Config)` gives the `download?id=` URL of a track for `HttpDownloader`. `configFrom(ServerConfig)` takes the url, user and password of an entry. `finish(deadline)` forwards to `HttpClient::finish`; the front ends call it through `Servers::finish()` (`make_music_server.h`, `kServerFinishDeadline` = 3 s) after `app.shutdown()`, so `App` and the `MusicServer` interface do not know about it. Answers are parsed on the HTTP thread; callbacks run from `poll()`. |
| `app/net/subsonic_json.{h,cpp}` | Pure parsers for ping, `parseCatalogueVersion()` (`lastModified` of getArtists), getAlbumList2 (with genre, year, created and MusicBrainz id), getArtists, getArtist, getAlbum, getPlaylists, getPlaylist and getOpenSubsonicExtensions answers, and `parseStatus()` for an answer with only a status (scrobble) (yaml-cpp). Error 40 is `BadLogin`, other Subsonic errors and non-Subsonic bodies `Server`. `parseMusicFolders()` (getMusicFolders). `jsonForYaml()` rewrites `\uD83C\uDFB5` surrogate pairs, which yaml-cpp 0.8 rejects. |
| `app/net/audiobookshelf.{h,cpp}` | `Audiobookshelf : MusicServer` (step 2.3; `makeServers()` builds it): every request carries `Authorization: Bearer <api_key>` and no key is in any URL but `stream()`'s (`<url>/api/items/<item>/file/<ino>?token=<key>`, always the original file; the id is `<item id>/<ino>`). `ping` is `/status` then `/api/me` (401 is `BadLogin`; type "Audiobookshelf"). `albums("newest" / "alphabeticalByName", offset, size, ..., library)` pages with `page = offset / size` and `more` while `offset + size < total`; other list types are empty. The library comes from the folder argument, else from `Config::library` (name or id), else the first book library by display order; it is asked for first (a second step under the same request id, run from `poll()`) and remembered. `musicFolders` lists the book libraries only (a podcast library never), or the one named. `artists` / `artistAlbums` are the authors. `book` / `albumTracks` read `/api/items/<id>?expanded=1` (tracks and the server's chapters). `catalogueVersion` is always empty, `playlists` empty, `createPlaylist` `Server`, `scrobble` nothing. `syncsProgress()` is true: `progress()` reads `/api/me/progress/<id>` (404 is no saved place); `saveProgress()` PATCHes `currentTime`, `duration`, the fraction `progress` and `isFinished`, and optionally acknowledges success or error from `poll()`. `inProgress()` reads `/api/me/items-in-progress` and filters to the selected book library. Save failures log only their error kind. No answer is `Unreachable`, 401 `BadLogin`, any other non-2xx or bad JSON `Server`. Answers are parsed on the HTTP thread; callbacks run from `poll()`. |
| `app/net/audiobookshelf_json.{h,cpp}` | Pure parsers (yaml-cpp) for `/status`, `/api/me` (checked, `token` never read), libraries, book lists, authors, an author's books and an expanded item, progress (`RemoteProgress`, with `lastUpdate` in wall milliseconds), a JSON progress writer (finite nonnegative seconds and duration, progress fraction clamped to 0–1), and `isoTime()` (`addedAt` ms as ISO 8601). The progress fixture uses synthetic values. Tests run on `tests/data/audiobookshelf/`, saved from the test server with `token` removed. |
| `app/net/percent_encode.{h,cpp}` | `percentEncode()` (RFC 3986), shared by `Subsonic` and `Audiobookshelf`. |
| `app/net/http_downloader.{h,cpp}` | `HttpDownloader : Downloader`: libcurl on its own worker thread (apart from `HttpClient`, so a long download never delays a list request), one download at a time in queue order. The URL comes from `Subsonic::downloadUrls()` (`download?id=`, same token auth as `stream()`); nothing here logs or reports it. Writes `<dest>.part` (flushed per chunk) and renames it to `dest` only when curl reports the transfer complete; folders are created. A `.part` is continued with `Range: bytes=N-`; a 200 answer (no ranges) or 416 starts the file over. An HTTP error, or a text / JSON / XML body with status 200 (a Subsonic error), is `Server` and leaves the `.part` as it was; a broken connection or 10 s without data is `Unreachable` and keeps it; a write or folder failure is `Disk`. Progress (latest only) and the result are delivered from `poll()`; `cancel(id)` stops the transfer and its callbacks never run. The destructor wakes the transfer and returns at once. |
| `app/net/md5.{h,cpp}` | `md5Hex()` for the auth token. |
| `app/media/player.{h,cpp}` | `Player`: one audio file or stream URL, decoded on its own thread (FFmpeg). A local file opens in `open()`; a URL opens on the decoder thread (`opening()` until it is done), with a 10 s read timeout and HTTP reconnects, and the whole open gives up after 10 s (`kStreamOpenTimeout`). `preload()` opens the next track's stream the same way in the background, and `open()` of that URL takes it over. A stream whose connection is lost while it plays is retried with backoff at the same place for 30 s (`kStreamRetryGiveUp`); `buffering()` says when it has run dry meanwhile. A transcode (`open()` with `Transcoded`) has no byte ranges: a seek, a retry and an open at a place open it again (see Streams in section 4), and its length is the server's. An interrupt callback lets `close()` abort any network wait at once, and `close()` or a newer `open()` drops an open still running. Its error messages print a local path but never a URL (`://`), which would hold the server login, and FFmpeg's own messages pass through `redactUrls()`. |
| `app/media/tag_reader.{h,cpp}` | `FfmpegTagReader : TagReader`: title, artist, album artist, album, genre, track and disc number ("3/12" is 3), year, duration and MusicBrainz album id of a local file, from the container's and the audio stream's metadata (ID3, Vorbis comments, MP4 atoms), keys compared without case, spaces, `_` and `-`. None for a file that cannot be opened or has no audio; a file without tags gives its duration and empty fields. Each call has its own `AVFormatContext`, so it may run on any thread, also several at once (the library scan of 4.5 reads off the UI thread). `FakeTagReader` (tests/fakes.h) maps path to tags and counts reads per path. |
| `app/media/tts.{h,cpp}` | `Tts`: eSpeak NG on a worker thread. `start()` reads the language codes of every installed voice into a `VoiceSet` before the worker starts; the worker turns each utterance into `speechMarkup(text, voices)` and passes `espeakSSML` to `espeak_Synth` when `ssml` is set (section 5). |
| `app/core/earcons.{h,cpp}` | `Earcons`: UI sounds made from sine tones. |
| `app/core/mixer.{h,cpp}` | `Mixer`: the three streams mixed into the output. |
| `app/core/audio_stream.{h,cpp}` | `AudioStream`: thread-safe PCM FIFO with flush epochs. |
| `app/core/library.{h,cpp}` | `Book` (also a remote list or, with `serverBook`, an audiobook of the server: the server's track ids in `files`, server titles in `titles` and lengths in `durations`, `remote` set; `albums` marks where each album of Play all starts, which the Album step unit uses), `scanLibrary(root, skip)`, which leaves out the cache folder `skip`, `appendBook()` (one more album at the end of a list like Play all, titles kept with their files), `naturalLess()` and `nameGroup()` (the letter group of a name, for the menu jump). |
| `app/core/track_cache.{h,cpp}` | `TrackCache`: downloaded tracks of one server in `<cache>/<server key>/<kind>%3A<list id>/`, where a list is an album or a playlist (the kind in the folder name keeps an album and a playlist with one id apart). Each list folder has the audio files and a `manifest.tsv` (header `oap-cache 2`, then a line `list<TAB>album|playlist<TAB>id<TAB>name<TAB>artist`, then for a downloaded server book a line `chapter<TAB>start seconds<TAB>title` per server chapter (`CachedList::chapters`; the book plays offline with them), then per track tab-separated: id, file, title, duration, a `changed` column (empty, or `kPartialMark` = "partial" on the tracks of a download that is not over; a download is never compared with the server), pinned `0`/`1`, last-played wall time, bitrate in kbps (0 = original; older seven-column rows read as original and gain the column on save); `\\`, `\t`, `\n`, `\r` escaped), read and written through the `TextFiles` interface of `services.h` (core does no file I/O of its own; `DiskTextFiles` in `app/config/` writes with `writeFileAtomically()`: temp file, `fsync`, rename, mode 0600; tests use the same on a temp folder). If `save()` finds a manifest that is there but not valid, it deletes the files in the folder that the new manifest does not name (never a `.part`, never another folder), so a damaged manifest leaves no orphans; a manifest that cannot be read at all (an I/O error) is not replaced (`save()` fails). A missing or corrupt manifest reads as empty, and so does a manifest of format 1 (it had no list line; no device ever held one) or one whose list line does not match its folder (a copied folder). `lists()` gives the lists with at least one complete track, sorted by name (`lists(true)`: only those with a complete pinned track, for Streaming > Downloaded); `trim(limit, played, keep, info)` writes play times into `lastPlayed` (the latest play replaces the old time; a time before `kEarliestPlayTime`, 2020-01-01, is a clock that was never set and is ignored) and evicts unpinned tracks and `.part` files (a `.part` first, then least recently played first), above the limit; `info` gives the evicted ids and the bytes left (section 5, "Automatic cache"); `remove()` deletes one list folder, only ever a direct child of the server's folder. `find(trackId)` returns the complete file of a track from any list (`index()` does it for every track in one pass, `completeFile()` for one manifest entry), never a `.part` file, a missing or empty file or a file named outside the folder. The server key is the host plus a hash of the URL (no login, no trailing slash) and the user name; list ids and the key are made filename-safe (`safeName()`), never holding a password or token. `App` takes the cache root, leaves it out of Library and Music, and plays from it (section 5, "Play from the cache"). |
| `app/frontend/threaded_scanner.{h,cpp}` | `ThreadedScanner : LibraryScanner` (both front ends construct it with an `FfmpegTagReader` and pass it to `App`): one worker thread runs `scan(ScanRequest, Done)` jobs in order. A `ScanRequest` holds the library root, the music root, the cache folder to skip and the `previous` local items. The worker lists the roots with `scanLibrary()` (the same files the menus show, cache folder skipped), keeps the track of a file whose path, size and file time (`stat`, whole seconds) equal an entry in `previous` (same kind), and reads tags through `TagReader` only for the others; a file that is not a regular file is never opened (a FIFO would block). Output, all `Source::Local`, `firstSeen` and `lastPlayed` 0: a `Book` item per folder or loose file under the library root, id `local:book:<path>`, titled by the folder, a track per file (title from the tags, else the file name); `Music` items grouped by tags: id `local:album:<album artist or artist>\x1f<album>` (an album in several folders is one item; with no artist at all the id ends in `\x1f<folder>`, the album folder or a loose file's folder, so artistless albums of two folders stay two), and files without an album tag by the folder under the music root, id `local:folder:<path>`, titled by the folder name. `joinAlbums()` then makes one item of the items whose album, folded with `artistKey()`, is the same and whose artist is the same so folded and not empty, or which share a folder and one artist is the other with more words ("Band" and "Band feat. Guest"), or which share a folder and have no artist while the others there are one album. Two different album artists in one folder stay two items; a compilation without an album-artist tag stays split by track artist. The rule reads only what the index keeps, so kept tracks give the same items. The joined item keeps the id of its part with the most tracks in `previous` (else the first id), the artist with the most tracks, the `seenOn` of every part, and the earliest `firstSeen` and latest `lastPlayed` of the parts in `previous` (otherwise those are 0; `LibraryIndex::upsert` keeps the stored `firstSeen` of the kept id). A file whose tags cannot be read is listed with its file name as title. A book's tracks are in file order (`pathLess`); an album's are ordered by folder, then track number (none: last), then file name (the disc number is not stored). A scan that throws (the reader or an allocation) delivers nothing and the worker goes on. A file with a tag edit that keeps both size and whole-second time is not noticed; the books' files are probed too, once. Results are delivered from `poll()`; `cancel(id)` never waits (a queued scan is dropped, a running one stops after the file it reads) and the callback never runs; the destructor does the same and joins, so a read stuck on a hung mount can delay it. `FakeLibraryScanner` (tests/fakes.h) records requests and delivers only on `finish()` + `poll()`. |
| `app/core/library_index.{h,cpp}` | `LibraryIndex` (owned by `App`, built on the `LibraryStore` it is given; see section 8): the library in memory, `IndexItem` (id, source local/server/cached, kind book/music/podcast, title, artist, album, genre, year, MusicBrainz id, first-seen and last-played wall times, tracks, `seenOn`) with `IndexTrack` (location, title, artist, number, duration, size, file time, `seenOn`). `playlists()` holds the server's playlists (`IndexPlaylist`: server key, id, name, song count, `changed`), kept apart from the items: `replacePlaylists(key, list)` replaces one server's, `addPlaylist()` adds one (Save as playlist), `playlistServers()` names the servers that have some, and `saveItems()` / `savePlaylists()` write the two parts (`save()` tries both). `seenOn` holds server references `<server key>:<album\|playlist\|track>:<id>` (`seenRef()`; `refOnServer()` finds the one for a server key), each at most once. Lookups by id, by track location, and by reference. `upsert()` merges (keeps first-seen, later last-played, and the references already there), `replaceSource()` is a scan, `replaceServer()` a sync of one server key. It loads and saves through `LibraryStore` in `services.h` (`load()`; `save(changed, removed)`), and `save()` hands over only the items that changed and the ones removed, nothing when none did. The SQLite store is `SqliteLibraryStore` in `app/library/`. |
| `app/core/settings.{h,cpp}` | `Settings`: dotted keys in memory, loaded and saved through a `SettingsStore`; the `state.tsv` line format; `ServerConfig` and `serverConfigs()` (the `servers:` list, else a lone `server:` section as a list of one; an entry with no url or the same server key as an earlier one is left out; core prints nothing, a `warnings` list goes to the caller); `serverConfigured()` is "`serverConfigs()` is not empty". `clearExcept` takes several prefixes (factory reset keeps `server.` and `servers.`). |
| `app/library/sqlite_library_store.{h,cpp}` | `SqliteLibraryStore : LibraryStore` (both front ends construct it on `--library-db` and pass it to `App`): `library.db` with tables `schema_version` (3: 2.4 added the `playlists` table, which a version 2 file gains when it is opened; 2: 4.8 added `items.track_count` and `items.created`; a version 1 file is moved aside and rebuilt by scan and sync), `items`, `tracks` (by item and position), `refs` (the `seenOn` of an item, or of its track by position) and `playlists` (server key, id, name, song count, `changed`; `savePlaylists()` replaces them as a whole in one transaction). `save()` is one `BEGIN IMMEDIATE` transaction that deletes and rewrites each changed item and deletes the removed ones, and rolls back on any error. Rollback journal (no WAL), `synchronous=FULL`, file mode 0600 (also set on an existing file). A file that is damaged (`quick_check`), not a database, or of another schema version is renamed to `library.db.bad` (replacing an earlier one) and the store starts empty; a file with no tables is new. Front ends take `--library-db FILE` (default `library.db` next to `state.tsv`, so `/data/config/library.db` on the device). |
| `app/config/stores.{h,cpp}` | `YamlStore` (`config.yaml`, nested YAML ⇄ dotted keys) and `TsvStore` (`state.tsv`). |
| `app/config/atomic_file.{h,cpp}` | `writeFileAtomically()`: temp file with mode 0600, `fsync`, `rename`. |
| `app/config/disk_text_files.h` | `DiskTextFiles`: the `TextFiles` of the real file system, for the cache manifests (`App` takes it with the cache root; without one the cache is off). |
| `app/config/settings_files.{h,cpp}` | `SettingsFiles`: what a front end hands to `App` (both files, loaded). |
| `app/core/queue.{h,cpp}` | `Queue`, the music queue (docs/queue.md), a plain class with no `App` and no I/O; `App` plays it (step 2.2). `QueueEntry` is a local track (path) or a remote one (server url + user, track id, length; never a password or stream URL), with title, album and artist. It keeps the entries in the original order and a play order (`playOrder()`, indexes into `originalEntries()`), the current position, `QueueShuffle` (off / tracks / albums), `QueueRepeat` (off / all / album / track) and `edited()` (set by `playNext`, `append`, `remove`, `move`; cleared by `replace`, which also resets shuffle and repeat). `advance()` is a track that ended (repeat track: `Repeated`; repeat album: the next entry of the current album in the play order, the album's first after its last, `Repeated` for an album of one entry), `next()` / `previous()` are the user moving (`Moved`, or `End`: past the last entry with repeat off, which makes the first entry current). `setShuffle(mode, random)` keeps what was played and the current entry and shuffles what follows (albums: runs of the same album name stay together); `append(entries, random)` with Shuffle on mixes the new entries into what has not played yet the same way (tracks one by one; albums run by run between two runs, cut by the same `cutRuns()`), and `playNext` keeps its entries in order right after the current one; the `QueueRandom` is passed in. `restore()` rebuilds a saved queue and says whether it kept the saved order and position (a play order other than the original one with shuffle off is refused: `move()` relies on it); `setTitle()` renames an entry and nothing else, not even the revision (for a queue just read: `App::retitleDownloads()`); `revision()` is a stamp that is new at every change and shared by copies, the key of `App::remoteAheadMemo()` (so `checkServerMark()` does not scan the queue at every tick). |
| `app/core/scrobble_file.{h,cpp}` | `formatScrobbles()` / `parseScrobbles()`: the scrobble outbox as the text of `scrobbles.tsv` and back, with no I/O. Section 9 has the format. |
| `app/core/queue_file.{h,cpp}` | `formatQueue()` / `parseQueue()`: the queue as the text of `queue.tsv` and back, with no I/O (`App` reads and writes the file through `TextFiles`). Section 9 has the format. |
| `app/core/text.{h,cpp}` | Text for speech and names, no I/O. `spokenDuration()`; `parseIsoTime()` (an ISO 8601 time as seconds since 1970, 0 if it is not one; pure arithmetic, used for a server album's `created`); `nextCodePoint()` / `appendUtf8()` (UTF-8 decoding and encoding; a malformed byte stands for itself); `foldCase()` (the shared case fold, section 8); `Script`, `ScriptRun` and `scriptRuns()`; `transliterate()`; `VoiceSet`, `SpeechMarkup` and `speechMarkup()` (section 5). |
| `app/core/album_merge.{h,cpp}` | Duplicates (4.8), plain functions on the index. `mergeAlbums(items)` groups the `Music` items that share a `seenOn` reference into `MergedAlbum`s (`primary`: local, else cached, else server; `items`; `onServer`): the merged view, computed when a menu is built, never stored. `sameAlbumByName(a, b)` is the name rule (same MusicBrainz album id, else artist + album + track count with case and spaces ignored, ASCII, Latin-1 and Cyrillic folded; unknown count or empty name never match); `linkByName(key, items)` gives the `AlbumLink`s (local or cached item, server reference) for items with no reference to that server, matched against server albums that no local item holds; `albumInfoText(merged)` is what Album info says. `trackCountOf()` is a server item's `trackCount`, else its tracks. |
| `app/platform/audio_output.{h,cpp}` | `AudioOutput` interface and `open(name)`, which picks a built-in backend at run time (`--audio`). |
| `app/platform/audio_thread.h` | Base for blocking-write backends: a thread renders 10 ms blocks and writes them. When a write fails it closes the device, keeps discarding the mix in real time (so speech and playback still finish) and reopens the device with a growing delay (250 ms up to 4 s). |
| `app/platform/audio_coreaudio.cpp` | macOS backend (AudioUnit default output). |
| `app/platform/audio_pulse.cpp` | PulseAudio simple API (also PipeWire via pipewire-pulse), about 50 ms buffer. |
| `app/platform/audio_alsa.cpp` | ALSA PCM (`default` or `alsa:DEVICE`), 50 ms latency. |
| `app/platform/audio_null.cpp` | No device: consumes audio in real time. |
| `app/frontend/presses.{h,cpp}` | `PressSequencer` (timed presses), `parseScript()`, `deepOf()`. |
| `app/frontend/terminal_keys.{h,cpp}` | `TerminalKeyDecoder`: terminal bytes → presses (`+` / `=` and `-` volume, `o` Options). Keeps an escape sequence split across reads; a lone Esc is Home only after `kEscTimeout` (200 ms) without more bytes, checked by `poll(now)`. |
| `app/frontend/held_buttons.{h,cpp}` | `HeldButtons`: counts the inputs holding each button, so App gets `keyUp` only when the last one lets go. |
| `app/core/no_music_server.{h,cpp}` | `NoMusicServer`: the `MusicServer` `App` uses when it is given no server (config.yaml sets up none, or none this build can use: another `type`, or a build without `OAP_BUILD_NET`); `configured()` is false and every request finishes with `NotConfigured` on the next `poll()`. |
| `app/core/no_downloader.{h,cpp}` | `NoDownloader`: the `Downloader` `App` uses with no server; every download finishes with `NotConfigured` on the next `poll()` and writes nothing. |
| `app/frontend/make_music_server.{h,cpp}` | `makeServers(config)` gives a `Servers` that owns the objects behind the `ServerLink`s both front ends give `App` (`links()`): for each entry of `serverConfigs()` a `Subsonic` and an `HttpDownloader` (type `subsonic`), or an `Audiobookshelf` and an `HttpDownloader` whose URL source is the server's `stream()` URL (type `audiobookshelf` with an `api_key`; without one, no link and the line `servers: Audiobookshelf needs api_key`), when built with `OAP_BUILD_NET` (`OAP_HAVE_NET`). An entry of another type, or any entry in a build without network support, gets no link and a line on stderr (`server: type 'plex' is not supported; streaming is off`, or the one about `OAP_BUILD_NET=OFF`); what `serverConfigs()` ignored (a `server:` next to `servers:`, a repeated server) is printed too. A link's name is the entry's `name`, else "Server" when one entry is set up, else "Server N" by its place among the entries; its key is `TrackCache::serverKey(url, user)`. `Servers::finish(deadline)` lets every Subsonic and Audiobookshelf server finish its requests at exit; Audiobookshelf save acknowledgements are delivered on the caller after transport threads join. Its own library, `oapmusicserver`, so `oapfrontend` keeps no dependencies. |
| `app/frontend/gamepad.{h,cpp}` | `GamepadMapper`: game controller buttons → device buttons, with the layout in `defaultGamepadLayout()`. Used by the simulator (SDL) and headless (evdev). |
| `app/frontend/pick_font.{h,cpp}` | `pickFont(candidates, exists)`: the first of the candidates for which `exists` is true (a parameter, so tests need no files). The simulator's font choice is in section 10. |
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
| **HTTP worker** | `HttpClient` | Runs every libcurl GET/PATCH transfer and parses Subsonic and Audiobookshelf answers. Finished results go into each server’s mutex-protected inbox that its `poll()` empties on the UI thread. At exit the front end calls `Servers::finish()` after `app.shutdown()`: `HttpClient::finish()` stops new requests, lets the queued and running ones end for up to `kServerFinishDeadline` (3 s), aborts the rest and joins this thread (a scrobble sent just before exit reaches the server and its answer reaches `App`, which drops the entry from `scrobbles.tsv` without sending the next one). |
| **Scan worker** | `ThreadedScanner` | Walks the library and music roots, reads the tags of new or changed files through `TagReader`, one scan at a time. Finished scans go into a mutex-protected list that `ThreadedScanner::poll()` empties on the UI thread. |
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
  any other request; a submission's answer (`done`) is polled like the others.
  Only at exit does a front end wait, for at most 3 s, in `Servers::finish()`, after
  `App::shutdown()` and outside `App`.
- Threads exchange data only through `AudioStream` (mutex-protected), atomics,
  and mutex-protected queues: the `Tts` text queue, the `HttpClient` job queue,
  the Subsonic and Audiobookshelf inboxes of finished requests and the `HttpDownloader` list of progress and results.
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
  interruptible in the mixer. This flushes queued speech and cancels synthesis;
  it does not retract samples already handed to an output backend. PulseAudio
  and ALSA are configured for roughly 50 ms latency, so audible cancellation
  must be measured at the headphone output. The acceptance boundary and target
  workload are in [SPEC.md](../SPEC.md#13-design-principles).
- Rate is applied per utterance (`Settings → Speech rate`).
- **Names in other scripts (4.5).** The core speaks any string; `Tts::run()` first calls
  `speechMarkup(text, voices)` (`app/core/text.{h,cpp}`, plain functions, no eSpeak). `scriptRuns()`
  cuts the text into runs of Latin, Cyrillic, Greek, Han, Japanese (kana, and Han touching kana),
  Hangul, Hebrew, Arabic and Other. Digits, spaces and punctuation have no script: between two letters of
  the same script they stay in that run, otherwise they join a Latin run. Latin runs stay in the
  engine's own voice (English, from `tts.start("en")`). Every other run is spoken by the first of:
  - **Plan A, its own voice:** a `<voice xml:lang="CODE">` element around the run when that code is in
    the `VoiceSet`: Cyrillic `ru`, Greek `el`, Han `cmn`, Japanese `ja`, Hangul `ko`, Hebrew `he`,
    Arabic `ar`.
  - **Plan B, transliteration:** `transliterate(run)` in Latin letters for the English voice. Cyrillic
    is BGN/PCGN without diacritics ("Жуков" is "Zhukov"), Greek ELOT 743 without accents, kana
    Hepburn without macrons, Hangul Revised Romanization of each syllable on its own. A letter with a
    capital keeps it (first letter of a word, "Zh", "ZH" in an all-capitals word). Spaces, digits and
    punctuation pass through; letters with no entry (ъ, ь, half-width kana, lone jamo) give nothing.
    Han, Hebrew, Arabic and Other give "".
  - **Named once:** "Chinese text", "Japanese text", "Korean text", "Hebrew text", "Arabic text",
    "Greek text", "Cyrillic text", "foreign text" (Other).
  `ja` reads kanji as "Japanese letter" and there is no reading table, so a Japanese run is split at
  its kanji: each stretch of kanji takes the `cmn` voice when it is installed (as Han without kana
  does), else it is named "Japanese text" (once, with a neighbouring kana stretch that has no
  reading); the kana stretches take the `ja` voice or their transliteration.
  Pieces of different scripts that touch get a space between them. When no run needs a voice element
  the result is the input, plain, with `ssml` false. Otherwise `ssml` is true, the XML characters
  `& < > " '` are escaped, a malformed UTF-8 byte becomes a space, and there is no `<speak>` root.
  `Tts::start()` collects the language codes of every voice eSpeak lists
  (`espeak_ListVoices`, plus the part before "-" so "en-gb" counts as "en") once, before the worker
  starts; the worker only reads the set. A machine without the `ru` voice therefore transliterates
  Cyrillic, with no setting. The speech log and the `say:` lines show the original text, not the markup.
- Every spoken string goes to `App::speechLog()`, which the simulator displays.
  `Tts` also prints it to stdout (`say: …`).

## 6. Input: buttons and gestures

```
key / GPIO ─► App::keyDown(b) ─► GestureDetector.press   ─► Deep / Short ─┐
          └─► App::keyUp(b)   ─► GestureDetector.release ─► Short / Light / Release ─┤
              App::tick()     ─► GestureDetector.poll    ─► Long / Repeat ─┤
              App::cancelGestures() ─► GestureDetector.cancel ─► Cancel ─┘
                                               App::onGesture() ─► App::onButton()
```

- **Long press** fires once while held after `Long press time` (default 600 ms).
  Ordinary keys suppress their release; Back/Forward report `Release` after a hold.
- **Two-stage Back/Forward:** raw `BackDeep`/`ForwardDeep` immediately report `Deep`
  on the light key, including after a light hold. `Long` still fires at the initial
  contact's deadline after a deep press. Opening the deep contact while light stays
  down reports `Light`; opening the last contact reports `Release`, never a trailing
  `Short`. Either release order is accepted; duplicate downs and light-contact bounce
  do not restart the shared deadline. A deep-only input starts the same lifecycle.
- **Cancellation:** `GestureDetector::cancel()` reports `Cancel` for active navigation
  gestures and suppresses held contacts until both stages are up. `App::cancelGestures()`
  applies this on screen push/pop, sleep/wake, Hold and shutdown; Hold preserves Power.
  Shutdown makes subsequent input inert; ticks still poll exit acknowledgements
  before returning without player work. Already-polled events from
  keys cancelled during the same tick are suppressed by the App's held-key locks.
  Step 1.2 of [button-remap.md](button-remap.md) installs this lifecycle first;
  the existing menu/player actions still use one action per hold/deep sequence via
  `navigationActed_` until steps 1.3 and 1.4 replace those handlers. `Light`, `Release`
  and `Cancel` currently have no speech/action side effects in those old handlers.
- **Repeat**: the volume keys (`VolumeUp`, `VolumeDown`) send a `Short` as soon as they go
  down, then a `Repeat` every `kRepeatInterval` (150 ms) after `kRepeatDelay` (400 ms), until
  they are let go. They have no long press and the release says nothing. `App` plays the
  edge sound once per press at a volume limit, not on each repeat, and a repeat never wakes
  the device or speaks in the key describer. A key that wakes the device stays quiet until let go.
- **Step unit:** Up / Down in a player call `changeStepUnit()`, and a light press of Left / Right
  calls `stepByUnit()` (`stepUnit()` reads the unit of the loaded player). The units are an index:
  0 Skip (`skip()`, the Settings skip), 1 Track (music) or Chapter (book), 2 Album (music,
  `stepAlbum()`: the first entry of the previous / next album in the queue, an album being a run of
  entries with the same album name in the play order; a queue of one album gives the edge sound and "Last album.") or
  10 minutes (book, `skip()` by `kLargeBookStep`, 600 s). Down moves to the next unit and Up to
  the previous, speaking it; past the first or last unit the edge sound plays and the unit is
  spoken again. `Deep` and `Long` on Left / Right go to the previous / next track or chapter
  whatever the unit. The unit is kept in memory in `state` as `unit.music` / `unit.book` (section 9).
- **Hold switch** (`App::setHold`) blocks every button except Power and says "Locked".
  A key that is already down when Hold goes on is ignored until it is let go:
  no long press while held, no short press on release.
- **Buttons:** `PlayPause, Back (left), Forward (right), Up, Down, Ok, Home, Talk, Power`,
  plus the raw `BackDeep, ForwardDeep`, and `VolumeUp, VolumeDown, Options`.
- `App` counts every gesture per button (`buttonUsage()`). The simulator shows
  the counts, so we can see which buttons are actually needed.

### Button behaviour

The rules behind this table, and the checklist for new menus, are in
[navigation.md](navigation.md).

| Button | Anywhere | Menu | Player screen |
|--------|----------|------|---------------|
| Play/Pause short | play / pause | | |
| Play/Pause long | "Where am I?" | menu path + item | the player ("Music player." / "Book player."), the album and artist, chapter or track, time left (`playerIdentity()` + `playerStatusBody()`) |
| Up / Down short | | previous / next item (wraps) | previous / next step unit (edge sound at the ends) |
| Up / Down long | | first / last item | the same, once (no repeat) |
| Left / Right short | | jump: previous / next letter in a list sorted by name, else 10 items | move back / forward by the step unit (skip, track or chapter, album or 10 minutes) |
| Left / Right long or deep | | the same jump, once | previous / next chapter or track, whatever the step unit |
| OK short | | open / activate | player menu |
| OK long | | previous screen | previous screen |
| Home short | | previous screen | previous screen |
| Home long | | home menu | home menu |
| Volume up / down | content volume ± 5 % (`kVolumeStep`) on every screen, held repeats; edge sound once per press at 0 and 100 % | | |
| Options | "Options, <item>" on an item with actions; in the music player "Options, <track>" (Go to album, Album info), in the book player a later server place when offered; otherwise "No options." with the edge sound | | |
| Talk | "not available yet" | | |
| Power short | sleep | | |
| Power long | power menu | | |
| *any key while sleeping* | wake and announce context | | |

In **key describer** mode (Help → Key describer), every key says what it
does and does nothing else. Holding Home leaves the mode. The same texts
(`kButtonHelp` in `app.cpp`) are read out by Help → Buttons.

## 7. App state machine

```
 start(): [Home]  or, with a last local book, [Home, Book player]
          (also starts the background scan of the local files, see section 8)
          a saved music queue or a server book played last: [Home], "Now playing" first, nothing loaded
          (a local book resumes into its player, unless the music player was used last and a queue is saved)

 screens_:  Home ─► Library ─► Book player ─► Player menu ─► Chapters
            ◄────────── Home / hold OK pops one ─────────────
            hold Home: back to [Home]

 Power short / sleep timer / auto sleep ─► Sleeping ── any key ──► same screen
```

- `App` keeps a **stack of `Screen`s**. A screen is a menu (`title`, `items`,
  `index`, `build`) or a program (`Screen::Kind::MusicPlayer` or `BookPlayer`; `Screen::isPlayer()`).
  OK pushes and Home pops. Home is always at the bottom. Popping back to a menu rebuilds it with
  `build`, keeping the selected label. There is at most one player screen, of the loaded player:
  `enterPlayer()` returns to it if it is on the stack.
- **Two players.** `book_` / `music_` / `fileIndex_` are the loaded player: the music player or the
  book player (a local or server book). There is one
  `MediaPlayer`, so the other player is **parked** in `parked_` (its `Book`, queue, file and seconds), and
  starting a player of the other kind parks the loaded one in `switchTo()` (undone, with the old
  `parked_`, if the open fails or is dropped). `continueParked()` (Home "Continue", or Play with
  nothing loaded) opens the parked book again at its place and plays it. Each player has its own
  Player menu (`musicPlayerMenu()` has Queue, Shuffle, Repeat, Go to first track and Sleep timer; `bookPlayerMenu()` has
  Chapters, Speed and Add bookmark, and neither). Home (`homeMenu()`) lists the player used last as "Now
  playing" and the other as "Continue", each when it has something: `playerTitle()` gives the
  title of the loaded book, the parked one, or what is saved for a restart (the queue, or
  `last_book`); `openPlayer()` enters, continues or resumes it. A parked place is kept across a restart in
  state.tsv (the book: `pos:` + `last_book`) and queue.tsv (the music queue, loaded or parked).
  `Add bookmark` in the book player is currently a spoken placeholder; no bookmark
  storage or navigation is implemented.
- **The music queue.** The music player's tracks are `queue_` (`app/core/queue.h`), local and remote
  entries mixed; `fileIndex_` is the current position in its play order (`setFile()` keeps the two
  together). For the music player `book_` is only the list the queue came from: its title, its
  server `source` and the track details the automatic cache needs
  (`listIndexOf()` finds a queue entry in it). Playback goes through `trackCount()`,
  `trackRemote()`, `trackKey()` (the file, or the server's track id), `trackTitle()`,
  `trackDuration()` and `upcomingTrack()`, which read the queue for music and `book_->files` for a
  book, so a queue entry decides per track whether it streams, is read ahead or is scrobbled
  (only remote ones). OK on a track, Play album / playlist / all replace the queue
  (`playEntries()`; `entriesOf()` makes the entries of a local list, `playRemote()` those of a
  server list); `playQueue(title, entries, start)` plays any mix and is what the tests use until
  Options exist. Read-ahead pre-opens `upcomingTrack()` when it is a remote entry (a local
  track can be the one playing). At the end of the queue (repeat off) it says "End of queue.
  Press play to start again.", pauses and loads the first entry (a remote one, or one that cannot
  play now, is opened only on Play).
- **Unavailable entries and a server marked down (step 2.3).** `openFile()` asks `availability()`
  of every music entry before it opens it: a local file that is gone, a remote entry of another
  server (url or user) or with no server set up (`linkOf()` finds no configured link: `Availability::NoServer`), or of
  a server while its `LinkState::down` is set and the track has no complete cached copy (`cachedFile()`; not for `Origin::Menu`:
  the user chose it from what the server just listed, so it is tried, and an open that works clears the
  mark) is skipped during automatic queue traversal. A user-selected remote entry
  is tried even when its server is marked down. An automatic failed open marks
  that server down and tries the next playable queue entry; if none remains,
  playback parks at the first skipped entry for an explicit Play retry. The entries skipped in a row
  are a `SkippedRun`, said once when an entry plays ("Storm, source unavailable." or "3 tracks,
  source unavailable."; when every entry of the run was skipped because its server is not set up,
  `SkippedRun::noServer == count`: "Cannot play Storm. Server not set up."); Previous (`stepChapter()`) goes back over them the same way. A remote
  entry that does not open (`onCannotPlay()`, or at the end of a track `failOpen()`), or a stream
  that breaks off after `Player`'s retries (`onStreamFailed()`), calls `markServerDown(link)` for the server of that track. At the
  end of a track (`Origin::Next`) a failed open is not an error to stop on: `continueAfterFailure()`
  goes on with the entries after it. When nothing from there on can play and the server is the
  reason, `parkAtSkipped()` closes the player at the first skipped entry, says "Server not
  reachable. Press play to try again." and `openFile()` returns `OpenResult::Stopped`. While
  a server is down (`LinkState::down`) and `remoteAhead(link)` (a remote entry of that server from the current one on),
  `checkServerMark()` (in `tick()`, for every link) pings through its `MusicServer::ping()` when its `pingDue` has passed:
  `kPingFirst` after the mark, `kPingSecond` after the first unanswered ping, then every
  `kPingEvery`; no ping in flight at the same time, none while asleep. An answer calls
  `clearServerDown(link)`. Play on an entry that cannot open now (`play()`) says "Checking the server."
  and pings at once the server of the current entry if it is down, else the first down server with entries ahead (`linkToCheck()`, `startMarkPing(link, true)`); `resumeClosedTrack()` runs when it answers, or fails,
  and then skips what is still down. `cancelMarkPing()` (new queue in `switchTo()`, `sleep()`,
  `powerOff()`) cancels it, so a late answer never runs. The book player is not touched: a book
  stops at the chapter that failed and Play tries it again. Server status shows "not reachable"
  as the item's value while that link is down, and a ping from there that answers clears it. **Repeat all (step 2.6):** `openFile()` scans from
  the target and wraps past the last entry, trying each entry once (`skipped.count` from a failed
  open counts as tried), so a queue with nothing that plays ends with "End of queue" instead of going
  round; `remoteAhead()` looks at the whole queue; `stepChapter()` wraps Left / Right on the
  first / last track and Previous goes back over unavailable entries past the first. With Repeat
  track a track that is gone is skipped like any other: the entries after it are tried, and the
  last one ends the queue.
- **Shuffle and Repeat (step 2.6).** `shuffleItem()` and `repeatItem()` are "Setting with list" items
  of `musicPlayerMenu()` whose value and list come from `queue_.shuffle()` / `queue_.repeat()`
  (OK opens the list on the current value; choosing works at once and goes back). `setShuffle()`
  calls `Queue::setShuffle()` and sets `fileIndex_ = queue_.position()` (Shuffle off moves the
  current entry back to its original place; the end-of-track sleep timer follows it). Repeat is read
  when a track ends (`upcomingTrack()` = `Queue::upcoming()`: repeat track gives the same entry, so
  `onFileFinished()` opens it again and `checkReadAhead()` pre-opens it; repeat album gives the next
  entry of the same album in the play order, the album's first after its last, and the same entry
  for an album of one; an album is the entries with the same `albumId`, else the same album name;
  the list is off, all, album, track, and `queue.tsv` writes `album`, which an older build reads as off).
  **Added entries:** `addToQueue()` passes the same `random_` to `Queue::append()`, so with Shuffle on
  Add to queue mixes the new entries into what has not played yet; Play next keeps its entries in
  order right after the current one. An Add to queue entry may land before entries put there by
  Play next, because those count as not played yet. **Randomness:** `App` has
  no source of its own that tests cannot reach: `App::setRandom(QueueRandom)` replaces the default,
  a `std::mt19937_64` seeded once from the `Clock` (core reads no other source of chance); tests set one that makes the shuffle
  known (all 0 turns what follows the current entry left by one place). A new queue (`playEntries()` builds a fresh
  `Queue`) starts with both off; "Previous queue" keeps
  the shuffle and repeat of the queue it brings back.
- `mode()` is derived from the stack: `Player` when the top screen is a
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
    std::function<std::vector<MenuItem>()> submenu;  // built when opened (fresh data)
    std::function<size_t()> selected;                // submenu opens on this entry
    ListOrder submenuOrder;                          // how the submenu's list is sorted (see below)
    std::function<std::vector<MenuItem>()> options;  // built when Options is pressed: "Options, <label>"; none: "No options"
    std::string okHint;                              // Beginner hint: what OK does ("play"); empty: open / select
    std::function<void()> action;                    // leaf
};
```

Menus are built by `App::*Menu()` functions and created lazily, so a list such
as Library or Chapters always reflects the current state. `App::choice()`
builds a setting from a settings key and a list of `(value, spoken name)`
options. OK opens the list of values on the current one, which is spoken as
"current". Choosing a value there saves it, applies it and goes back. Left / Right
never change a value; they jump in the list (`App::jump()`): to the first item of the
previous / next letter when the list's `Screen::order` is `ListOrder::ByName` or
`ByArtist` (artists ignore a leading "The"), else by `kMenuJump` (10) items. `order` is
set by the item that opens the list (`MenuItem::submenuOrder`) or by `openRemoteList()`;
a list that is not marked jumps by 10. The letter comes from `nameGroup()`
(`library.h`); the status rows of a list from the server and "Unknown artist" are
groups of their own with no spoken letter. The item types are listed in [navigation.md](navigation.md).

**Options (step 2.4).** `App::openOptions()` opens `item.options` as the screen "Options, <label>".
Lists attach it with `withOptions(item, Pick)`. A `Pick` is either Ready (`make()` builds a
`Selection`: the list `Book` and the `QueueEntry`s) or a remote album, playlist or artist that
`choose()` loads first (`loading_`, tied to the Options screen id: leaving it, sleep and power off
cancel the request, so a late answer does nothing). `perform()` pops the Options screen, then
`playNow()` (replaces the queue, says "Playing X." once it opens) or `addToQueue()` (queue edit; with
no music queue loaded a closed queue is made, like one that ended: `reopenAt_` set, Play opens it; while a book
plays the queue is parked for Continue). `queuedFrom()` keeps a downloaded list that is in the queue from being deleted.

The spoken form of an item is `label[, value][, i of n][. hint]`. The position
("i of n") is left out at Expert verbosity, and the hint ("Press OK to open")
is added only at Beginner.

### Current menu tree

```
Home
├── Now playing: <book or album>   (the player used last: loaded, parked, or saved for a restart: the music queue of `queue.tsv`, a book `last_book`)
├── Continue: <book or album>      (the other player, when it has something)
├── Library → <books>              (OK plays/resumes; then the books of the server's audiobook folder, value "on the server": "Loading.", then they play; "Scanning" while the first scan runs)
├── Music
│   ├── Play all                   (the files on the device, albums in the order of Albums A to Z; with only server albums it says "No music")
│   ├── Artists → <artist> → <albums>   (local and server artists once each, sorted without "The"; "Unknown artist" last)
│   ├── Albums A to Z → <albums>   (every merged entry, server-only ones too)
│   │   <album on the device> → <tracks>   (OK plays; Album info is in its Options) from that track to the end; a loose track plays at once)
│   │   <server-only album> → Loading, then Play album · Download album · <tracks>   (as in Streaming)
│   ├── Playlists → <playlist> → Play playlist · Download playlist · <tracks>   (`playlistsMenu()`: the index's playlists and the downloaded ones from the cache manifests, one entry for a playlist in both, sorted by name, "12 tracks"; a local menu, works offline; "No playlists". A playlist whole on the disk opens from its files, else its tracks load from the server; with no music yet the item is there only when a playlist is)
│   ├── Genres → <genre> → <albums>   (tags and server genre, names merged without case; albums with no genre are left out)
│   ├── Recently added → <albums>      (newest first, at most 50; "No new albums")
│   ├── Recently played → <albums>     (last played first, at most 50; "Nothing played yet")
│   └── Refresh library            (rescans the files and syncs the server catalogue, see section 8)
├── <server name>, one per server in config order (Streaming: lists from the server, see below; Home builds the entries from `servers_`, `serverMenu(link)` opens `booksMenu(link)` when the link's `catalogue()` is `Books`, else `streamingMenu(link)`; default name Server / Server N)
│   ├── Recently added → <albums>  (one page of 500)
│   ├── Albums A to Z → <albums>   (pages of 500, the next one fetched while the server has more)
│   ├── Artists → <artist> → <albums>
│   ├── Random albums → <albums>   (one page of 50)
│   ├── Downloaded                 (only with a cache folder; built from the manifests, not the server: <name> → Play album|Play playlist · Delete download → [Cancel, Yes, delete] · <tracks>)
│   └── Server status              ("Checking the server.", then "Connected to Navidrome 0.64.2." or the error; the item says "not reachable" while the server is marked down)
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
│   ├── Download quality original file / 320 / 192 / 128 kbps (independent of Streaming quality; captured when an album/playlist download starts; Books providers keep originals)
│   ├── Cache size       off / 1 / 4 / 16 GB (automatic cache of streamed tracks; default 1 GB; a lower size is applied on the next exit; off keeps what is cached and caches nothing new)
│   ├── Replace cached copies off / on (default off; a better stream replaces the cached quality after listening)
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

Player menu of the book player (OK on its screen; `bookPlayerMenu()`)
├── Chapters → <chapters>          (opens on the current chapter)
├── Sleep timer → …
├── Book info
├── Go to start of book
├── Speed                          (placeholder)
└── Add bookmark                   (placeholder)

Player menu of the music player (`musicPlayerMenu()`)
├── Queue → <entries>              (`queueMenu()`; opens on the current entry, "Storm, current, 5 of 30")
│     ├── <entry>                  OK plays it (`gotoChapter`); Options: Play next · Move · Remove
│     │     ├── Play next          (`queuePlayNext()`: after the current entry)
│     │     ├── Move → <places>    (`queueMovePositions()`, opens on the entry's own place; "Moved to 3 of 30")
│     │     └── Remove             (`queueRemove()`; the current entry: stopped, closed, the next one current)
│     ├── Save as playlist         (`saveAsPlaylist()`: "Saving.", then `createPlaylist` with the ids of the server set up now, in play order; the server's answer carries the new playlist's id, which goes into the index at once and shows in Music → Playlists)
│     ├── Previous queue           (only when `previous_` holds an edited queue)
│     └── Clear queue → Cancel · Clear queue   (`clearQueue()` / `unloadQueue()`: nothing loaded, Home)
├── Shuffle, off                   list: off · tracks · albums (backed by the queue, not config.yaml)
├── Repeat, off                    list: off · all · album · track     (the same)
├── Go to first track
└── Sleep timer → …

Options in the music player (`playerOptionsMenu()`, "Options, <track>")
├── Go to album            (local: the index item of the file; remote: the album id of the entry; opens on the playing track)
└── Album info             (title, number of tracks, status)

The Options of a server album or playlist (Music and Streaming) have Download album / Download playlist after
Add to queue (`optionsMenu()`), where `offersDownload()`: `canDownloadFrom(pick.link)` and the list is not already whole on the
device at Download quality (every manifest track pinned and complete, `isWholeDownload()`,
and `qualityAtLeast()` for each track). `choose()` with `QueueAction::Download`
loads the tracks ("Loading."), then `onPickLoaded()` calls `startDownload()` with a `started` callback that pops the
Options screen and plays the close sound only once the download has started; a refusal is spoken on the Options.
While a download runs `choose()` does not load: `sayDownloadBusy()` gives the progress for the same list, else
"Another download is running." A list that is whole on the disk (`isWholeDownload()`: every manifest track pinned and
complete, and none marked `kPartialMark`; `startDownload()` marks the manifest's tracks and `addToManifest()` the ones it
adds, `finishDownload()` clears the marks, so a failed or cut-short download is not whole) needs no server for Play now /
Play next / Add to queue: `choose()` takes its tracks from the disk (`completeTracks()`), with no "Loading."

"Where am I?" in a player (`whereAmI()`) speaks `playerIdentity()` then `playerStatusBody()`: "Music player." or
"Book player.", then the album and artist of the current queue entry (not the queue's title) or, for a book, its
title and author ("Dune, by Weather."; the artist alone as "By Weather."; nothing when both are empty), then the track,
the time left and the state. A server book carries its author in `Book::artist` (`serverBookOf()`); a local book has none.

A change to the order of the queue (Shuffle, Move, Remove, Play next in the Queue list) calls `followQueue()`: `fileIndex_`
is the play order position of the current entry, so it, the end-of-track sleep timer and `pendingOpen_->file` follow it.
Removing the current entry (`stopPlayer()`) drops its open without the open's undo and leaves the next entry closed
(`reopenAt_ = 0`), as a queue that ended waits; it also cancels an end-of-track sleep timer set for it. Clear queue and
removing the last entry call `clearQueue()`: an edited queue goes to `previous_`, then `unloadQueue()` stops, resets `book_`
and `queue_`, forgets `last_stream.*` (and queue.tsv is emptied at the next exit) and goes back to Home. While a track chosen by the user (Origin::Menu or Key) is
opening, Play next / Move / Remove say "Loading." and wait (`queueLocked()`): the open's undo holds an old queue position.

Power menu (hold Power): Sleep · Power off · Restart · Cancel
```

**Several servers (step 1.4):** the browsing menus of each server's Streaming menu carry the index of
their link (`streamingMenu(link)`, `openRemoteList(link, ...)`, `Pick::link`, `RemoteList::link`,
`pingLink_`) and ask `serverAt(link)`; a request is cancelled on the link it was made on. `tick()` polls
every link. **Playing from the right server (step 1.5):** a remote `QueueEntry` names its server by `serverUrl` + `user`
(`queue.tsv` keeps exactly that); `linkOf(entry)` finds the configured link, `trackLink(i)` the link of track `i`. Everything
that plays asks the link, never `server_`: `newStream()` (`serverAt(link).stream()`), `checkReadAhead()` (`ReadAhead::link`, so
the same track id on another server is not taken for the pre-opened one), `TrackPlay::link` (the "now playing" scrobble and the
submission go to that server), `AutoCache::link` (its downloader, its cache folder `<cache>/<server key>/...`, its manifest), and the
`LinkState` of each link (`links_`): marked-down state with its ping schedule, `AheadMemo`, the play times for the cache, the
cache byte count, the `TrackCache` and its index (`trackCacheOf(link)`, `cachedFile(link, id)`; the same track id on two servers
is two tracks). `trimCache()` trims each server's cache on its own, to the same limit (so the automatic caches of N servers can take N times the Cache size; unpinned tracks only). `playRemote()`, `trackItems()`, `remoteSelection()` and
Options picks carry the link, so a list of the second server queues entries of the second server. A track of a server
that is not set up cannot play ("Cannot play Storm. Server not set up."); its entry stays in the queue and in `queue.tsv`.
Downloads are per link too (step 1.6): `Download::link` is the server a download is fetched from (`servers_[link].downloader`) and
written for (`trackCacheOf(link)`, `<cache>/<server key>/...`); `trackItems()` and `offersDownload()` ask `canDownloadFrom(link)`;
Downloaded (`downloadedMenu(link)`, `downloadedListMenu(source, link)`, `hasDownloads(link)`) lists that server's cache only, and
Delete download (`deleteDownload(source, title, link)`) touches its list, its cache index and its automatic jobs only
(`dropAutoCache(list, link)`; `queuedFrom()` and the loaded-list check compare links), because two servers can have a list
with the same id. One download runs at a time across all servers (`download_` is a single optional): starting another says
"Another download is running." unless it is the same list of the same server (`downloading(source, link)`), which says its
progress. The older `last_stream.*` state finds its link by url and user (`StreamPlace::link`, `resumeStream()`).
**Catalogue per server (step 1.7):** see Server catalogue below. Music's playlists list those of every server (`playlistEntries()`, each with its link; `openPlaylist(link, ...)`), Save as playlist goes to the server of the first remote track of the queue (`Saving::link`; the other servers' tracks count as not saved), and a server book knows its link (`Book::link`, set by `serverBookOf()`; `trackLink()` returns it for the book player, `resumeLink_` is it while the book loads). There are no first-link shortcuts left (`server_`, `downloader_`, `trackCache()` are gone; with no list the stand-ins `noServer_` / `noDownloader_` are only polled). A scrobble in the outbox
whose server is not set up stays in `scrobbles.tsv` (bounded by `kMaxScrobbles`) and is sent when that server is back.

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

- **One case fold (4.2).** `foldCase()` in `app/core/text.{h,cpp}` lowers ASCII, Latin-1, Latin
  Extended-A, Greek and Cyrillic; ё and Ё become е, Ό Ύ Ώ their lower forms and the final ς σ;
  other scripts are unchanged. `library.cpp` sorts with it and then drops Latin-1 accents and folds
  ß to s ("Ä" sorts as "a"); `album_merge.cpp` builds the album and artist keys with it, accents kept
  ("é" and "e" stay two artists). Both decode with `nextCodePoint()` and encode with `appendUtf8()`
  from the same file. The keys are not persisted.
- **The index.** `App` takes a `LibraryStore&` and a `LibraryScanner&` (after the
  `Downloader`) and owns a `LibraryIndex` loaded from the store when it is built.
  Library and Music are built from the local items of that index, never from the disk:
  Library lists the `Book` items and Music the `Music` items, both sorted by title
  (`naturalLess`), so browsing is instant and a new start rescans nothing on the UI
  thread. `start()` asks the scanner for a scan (library root, music root, cache folder to
  skip, the local items of the index as `previous`) and returns at once; `tick()` calls
  `scanner.poll()`. A finished scan runs `replaceSource(Local, items, clock.wallTime())`
  and `save()`, which writes only what changed, so a scan that finds nothing new writes
  nothing. A list is built when it is opened, so a scan that ends while it is open shows
  from the next time it is opened. Until the first scan is done an empty list says
  "Scanning" instead of "Library is empty" / "No music". `shutdown()` cancels a scan still
  running. The index save runs on the UI thread, inside `tick()`.
- **Server catalogue (4.7, per server since 1.7).** `start()` also calls `startSync()`: it
  drops what was seen on any server key that is not a link now (`dropUnsetServers()`: `replaceServer(other, {})`, its
  playlists, its `catalogue.<key>.*` state; with no server
  set up that is every key; a server this build cannot use drops nothing) and removes the older single-server keys
  `catalogue.server` / `.version` / `.folder` (the first sync of each server then fetches once more). Then every link
  that is configured is synced in turn, in config order and one request at a time (`syncNext()`; `finishSync()` goes
  on to the next link whatever the result). For each it asks
  `MusicServer::catalogueVersion()` (Subsonic: `lastModified` of `getArtists`) and compares it
  with `catalogue.<server key>.version` in `state.tsv` (never in `config.yaml`). It fetches the albums
  (`alphabeticalByName`, 500 per page, while the server says `more` and the page is not empty,
  at most 200000) only when the version differs, is empty, or the index holds no server
  item of that key. When every page is in, `replaceServer(key, items)` runs and `save()`
  writes only what changed; the version is stored only after a save that worked. Each
  server item is `Source::Server`, `Kind::Music`, id and `seenOn` `<server key>:album:<id>`, with
  title, artist, genre, year and MusicBrainz id; its tracks are not stored. An error at any
  step keeps the saved items and the old version and is silent at start-up. **Playlists (2.4):** the
  last step of the sync is `fetchPlaylists()` (`getPlaylists`, after the catalogue pages; it also runs when the
  catalogue version is unchanged, so the playlists are fresh without a new catalogue). The answer replaces the
  server's stored playlists (`replacePlaylists()`, `savePlaylists()`); a playlist saved with Save as playlist while
  the request is out is kept (`catalogue_.savedPlaylists`). A failing `getPlaylists` ends the sync with the error:
  the albums are already replaced and the old playlists stay. The version in `state.tsv` is stored after the
  items are saved (`saveItems()`), whatever happens to the playlists table. `dropUnsetServers()` also clears the
  playlists of servers not set up. A server item
  also has `trackCount` (`songCount`) and `created` (`parseIsoTime()` of the server's text, 0 if
  unknown; used by Recently added). **Music -> Refresh
  library** (last item of Music, also under "Scanning" / "No music") says "Updating the
  library.", starts a scan and a sync unless the start-up ones still run (it joins them), and
  when both are done says "Library updated, N albums, M books." (merged entries: an album that is local and on the server counts once;
  M is the server books in the index, local books are not counted; either count is left out when 0, "Library
  updated." with none), then for each server that failed its name and the error ("Music: server not reachable.";
  with one server only the error, "Server not reachable."). When every server tried failed it says only the errors.
  It speaks only if the Music screen it was pressed on is still on
  top and the device is awake. A second press meanwhile says "Still updating.".
  `shutdown()` cancels the sync.
- **Books of a Books server (audiobookshelf.md 2.5).** A server whose `catalogue()` is `Books`
  skips the version check and `audiobook_folder`: `onCatalogueVersion()` fetches its whole book list
  every time (`fetchCatalogue()`, pages of 500, `catalogue_.books`), every item is `Kind::Book`, and no
  version or folder is stored. `replaceServer()` / `saveItems()` write only what changed, so an unchanged
  list writes no index. Library lists them like folder books; `openServerBook()` asks `book()` for these
  servers and `albumTracks()` for the others.
- **The Books server menu (audiobookshelf.md 2.7).** `booksMenu(link)` is Continue listening, Recently added and Books A to Z
  (`openAlbumList(..., books=true)`: the Streaming list machinery with `bookItems()`, "No books"), Authors
  (`openAuthors()`: `artists()`, then `artistAlbums()`), Downloaded and Server status. A book opens
  `openBookPage()`, a remote list whose request is `book()`; `bookPage()` builds Play book, Download book
  (`startDownload(..., unit "file")`, so the progress counts files) and the chapters from the already loaded
  `RemoteBook`, with no second request. It plays through `serverBookOf()` under the Library id
  `<server key>:album:<id>`, so the place (`pos:` + id) is shared with Library; a chapter plays at its file and start.
- **Continue listening (audiobookshelf.md 3.5).** `MusicServer::inProgress(done)` returns books from the
  selected library. Audiobookshelf resolves that library through `withLibrary()` and reads
  `/api/me/items-in-progress`; `parseInProgress()` keeps only `libraryItems` with that `libraryId`
  and `mediaType: book`. A missing list is `Server`, an empty list succeeds. Subsonic reports
  `Server` (unsupported), and `NoMusicServer` reports `NotConfigured`, both through `poll()`.
  `openInProgress()` uses `openRemoteList()` ("Nothing in progress" when empty). Its book actions
  call `openServerBook(..., followServer=true)`, then `pullProgress(..., followServer=true)` applies
  the progress GET regardless of a local place, timestamp or 30-second distance; this explicit
  choice uses the same cancellation and timeline mapping as ordinary pulls. Playback starts at
  the local place while it waits, and errors remain silent. Ordinary book opens retain the
  later-place notice and Options action.
- **Download a book (audiobookshelf.md 2.8).** Download book is `startDownload(..., unit "file", chapters)`: the files go
  one by one into `<cache>/<server key>/album%3A<id>/`, pinned, as for an album, and the server's chapters are written to
  the manifest with the first file (`ManifestUpdate::chapters`, the `chapter` lines of `CachedList::chapters`). A file that
  fails is named by its title, else by the unit ("file 3", `failDownload()`). `bookPage()` offers Download book only
  while the book is not whole on the disk, and Delete download (`deleteDownloadItem()`) once any file is. A whole download
  (`isWholeDownload()`) opens without waiting for book metadata (progress is asked in the background): `openServerBook()` (Library, "Now playing" after a restart,
  `last_book`) builds the book from `completeTracks()` and the manifest's chapters, and Books > Downloaded
  (`downloadedMenu()` on a link whose `catalogue()` is `Books`) opens `downloadedBookMenu()`, the same `bookPage()` built
  from the disk; a book that is cut short opens `openBookPage()` as on the server. All three play under the Library id, so
  the place is one. `syncDownloads()` makes no Cached (Music) item for a Books server: its downloads are books.
  `openServerBook()` saves the place of the book that plays before reading the saved place, so reopening it from Library
  keeps the progress made since it opened.
- **Pushing a server book's place (audiobookshelf.md 3.3).** A server that `syncsProgress()`
  receives the position on the book's timeline (preceding file durations plus seconds in the current
  file), its duration and completion on pause, replacement, sleep, exit and each 60 seconds of
  playback with at least 5 seconds of movement. Paused and buffering time do not count. All position
  state stays in memory until exit or sleep. `posat:<id>` is the wall second of the recorded place;
  `pushed:<id>` is the timestamp last successfully acknowledged. `pushdirty:<id>` preserves pending
  changes made in that same second or after a clock correction; a matching acknowledgement clears it.
  `finished:<id>` keeps an offline completion even though local resume resets to the beginning; actual
  playback clears it. These keys share the existing book id (`<server key>:album:<id>`), never a URL.
  Startup silently asks `book()` for pending places' file durations and sends the stored timeline
  position; failed or stale metadata answers leave it pending. A late save answer can acknowledge
  only the same timestamp, file position and completion, and never touches a destroyed App (weak
  lifetime token). Saves of one book are serial; a newer snapshot waits behind an outstanding save.
  Returning to the outstanding place drops a queued detour. An in-app restart keeps this ordering
  while its transport remains alive; factory reset invalidates outstanding save callbacks and queues.
  If exit ends that queue before the newest snapshot is sent, it remains dirty for startup retry.
  `Servers::finish()` drains Audiobookshelf transports in parallel with Subsonic; after joins it
  delivers only progress-save acknowledgements on the caller thread (`Audiobookshelf::finish()`),
  so completed exit saves can update the persisted marker without concurrent state-map writes.
  Browsing callbacks never run during this drain. Real-server listening and offline retry need a
  device check.
- **Pulling a book's place (audiobookshelf.md 3.4).** Once `playServerBook()` has opened local
  playback, `pullProgress()` asks the book's server without Loading or a wait. The opening remembers
  whether a local position existed (an explicit chapter is also a local choice); `openPlayer()`
  also pulls when returning to a loaded book.
  `serverPlace()` maps timeline seconds to file and chapter, rejecting unknown multi-file timelines.
  A newer `lastUpdate` than `posat` and a difference greater than 30 seconds moves an unpositioned book
  automatically; with an existing local place it offers the place once in speech and as the first
  book-player Options action: "The server has a later place, chapter 5. It is in Options." then
  "Go to the server's place, chapter 5, 12 minutes" (the actual chapter and time). Older, nearby,
  missing and failed answers are silent. `useServerPlace()` preserves play/pause,
  seeks or opens the target file, and says its chapter. The accepted place is kept in memory with the
  server timestamp; accepting an Options offer also serializes a save after any older local save, so
  pausing while the notice is pending cannot overwrite the chosen place remotely. The pull has a request
  id, generation and weak App lifetime guard. `cancelProgressPull()` drops it and its offer on replacement,
  player-screen removal, sleep, power off, restart or shutdown. Whole downloads play at once even if
  the background progress GET fails offline. No state is written to disk by a pull.
- **Server chapters (audiobookshelf.md 2.6).** `openServerBook()` passes `RemoteBook::chapters` to
  `serverBookOf()`, which places them with `serverChaptersOf()`: a file starts where the `durations`
  before it add up to (what Audiobookshelf calls `startOffset`), so `Book::chapters` holds
  `{title, file, start}` with `start` in seconds into that file (sorted; a start before 0 is 0, one at
  or past the end of the book is dropped, one within `kChapterSlack` before a file's start belongs to
  that file; a multi-file book with a file of unknown length keeps no server chapters). When
  `hasServerChapters()`, `chapters()`, `currentChapter()` and `gotoChapter()` use them before the
  per-file and embedded chapters, so a chapter may begin in one file and go on in the next.
  "Where am I?" and Previous count time over files with `bookTime()` / `bookLeft()`, and the sleep timer's
  end of chapter ends with a file only when `chapterEndsWithFile()` (the next chapter starts there).
  The place is still `pos:<id>` = file index and seconds. A book played from a whole download has the same chapters:
  they are kept in its manifest (2.8) and handed to `serverBookOf()` as when the server answers.
- **Audiobooks from the server (4.11).** `MusicServer::musicFolders()` (`getMusicFolders`,
  `RemoteFolder{id, name}`) and a trailing `musicFolderId` on `albums()` (`getAlbumList2`; empty:
  all). `server.audiobook_folder` in `config.yaml` (the folder's name, exact match; set by the
  user, never made by the device) makes the sync ask for the folders, take the ids of the
  folder's albums (paged like the catalogue), then the whole catalogue, and give the albums in
  the folder `Kind::Book` (the rest `Music`). A folder the server does not have makes no books and the
  sync is not remembered (the next start asks again). `catalogue.<server key>.folder` in `state.tsv` is stored with
  `catalogue.<server key>.version` and compared with that server's `audiobook_folder`, so changing the folder syncs again (it is
  absent when no folder is set). Music, its lists and `mergeAlbums()` only look at `Kind::Music`, so
  a book is never in Music. Library lists the local books and then the server's `Kind::Book` items
  with the value "on the server". OK calls `openServerBook()`: "Loading." and
  `albumTracks()` (the request of `resumeStream()`, `resume_`, dropped by a new screen, by
  leaving the Library screen (`resumeScreen_`), by sleep and power off), then a `Book` with `remote` and
  `serverBook` set, `path` the item id (`<server key>:album:<id>`, no URL) and `source` the
  album, played with `music_` false (so it says "Chapter") from the place in `pos:<item id>`
  (`"<file index>:<seconds>"`, as a local book's; an index past the chapters starts at the first).
  `rememberPosition()` writes that key (on pause, on opening another book, on exit and sleep, and
  at the end of the book, when it saves chapter 0 at 0) and no `last_stream`. A server book is not
  cached by the automatic cache, and is scrobbled like any remote track. A book is not merged with a
  local copy or a download of it: `syncDownloads()` makes no Cached item for a download whose server
  item is a book, so a downloaded book stays out of Music.
- **Resume a server book (4.12).** Once a server book plays (not before: a failed open changes
  nothing), `playServerBook()` sets `last_book` to its item id (`<server key>:album:<id>`, no URL; a
  local book's `last_book` is its absolute path, so the two cannot be mixed up) and
  leaves `last_stream.*` alone (the music player keeps its own place). `savedServerBook()` is the index item (`Source::Server`,
  `Kind::Book`) of the server set up now that `last_book` names; none when `last_book` is a local path,
  another server's id or a book that left the index (then `last_book` is not resumed). `start()` does not
  look for a server book on disk and loads nothing: it opens Home, where "Now playing" (shown after the
  loaded book and after `savedStream()`, so a stream played later wins) and Play with nothing loaded call
  `resumeServerBook()` -> `openServerBook()`, the same code as Library's OK. A book downloaded whole
  (`isWholeDownload()`, the rule of 3.6) plays from its files at once: no question to the server, no
  "Loading.", also offline. Any other book says "Loading." and asks the server; with the server down
  "Server not reachable." is said and Home stays (a partly downloaded book does not fall back to the
  files on disk, since its chapter numbers would shift). The place is the file index saved in
  `pos:` against the list as it is now (the server's, or the download's manifest order, which
  follows the server's); an index past the end of the list starts at the first chapter at 0. Limits:
  a download of a book that left the index is listed in Music as an album and "Now playing" for it
  goes; local music played after a server book leaves `last_book` alone, but `last_player`
  determines which player is offered first on restart (section 9).
- **Duplicates and downloads (4.8).** Merging is stored as references, not as a merged item:
  an album that is on the device and on the server is a local item holding the server item's
  reference in its `seenOn`, and the same for a download. `replaceServer()` keeps a reference
  of a local or cached item that the new server items still carry, so a renamed album is still
  matched by reference. After a scan and after a sync, `linkToServer()` runs `linkByName()` and
  `addSeenOn()` for local and cached items that have no reference to this server (the name rule
  is only for them). An album that left the server loses the reference (local and cached
  items stay). `syncDownloads()` makes the pinned album lists of `TrackCache` the `Cached`
  items (id `cached:<ref>`, the track files as locations, `seenOn` the album's reference and a
  track reference per track id, from the manifest). It runs whenever Music is built and before
  Refresh counts, so a download made or deleted meanwhile shows; the reference is kept only while
  the server item exists (or no server catalogue is known yet). A cached album plays as a local `Book`
  with its titles from the manifest; Play all joins the albums with `appendBook()` (`library.h`), which
  carries each file's title with it (empty for a local file), so a download is never spoken by its file name. Album info is in that album's Options (`Pick::info`).
- **The Music tree (4.9).** `musicMenu()` is Play all, Artists, Albums A to Z, Refresh library
  (Genres, Recently added and Recently played, below); an empty index gives "Scanning" or
  "No music" and Refresh. Every list is a `submenu()` whose builder runs `mergeAlbums()` again,
  so it is a local menu rebuilt when you come back to it, with no "Loading". `albumList()` sorts
  merged entries by title and `albumEntry()` makes one item: an album whose primary is local or
  cached opens `albumMenu()` at once (a loose file plays at once), and a server-only album calls
  `openRemoteAlbum(album, playedId, focusTrack)`, the same code as Streaming (`openRemoteList()`: "Loading", the
  error with "Try again", "Play album" first); `focusTrack` opens it on that track (`MenuItem::key`, `RemoteList::focusKey`); the server's album id
  is cut from the item's reference `<key>:album:<id>`. A merged album plays from its device files
  and never asks the server. `artistsMenu()` groups the merged entries by `artistKey()` (case and
  spacing ignored, so "Band" and "band" are one; `foldCase()`, accents kept) and sorts with `artistLess()`: natural,
  case-insensitive order without a leading "The" (`kIgnoredArticles`; the server's own
  `ignoredArticles` is not read yet), Cyrillic and other scripts by code point after Latin,
  entries with no artist last as "Unknown artist". An artist's list is its merged entries. Play
  all is the files of the entries whose primary is not a server item (a server item holds no
  tracks) and says "No music" when there are none. Closures keep copies, never pointers into
  the index.
- **Genres, Recently added, Recently played (4.10).** `genresMenu()` files each merged entry under
  every distinct genre of its items (`genresOf()`, keyed by `artistKey()`: case and spacing
  folded; the spoken name is the first spelling met), so a local "Rock" that the server calls
  "Alternative" is in both; entries with no genre are left out. A genre's list is `albumList()` of its entries.
  `recentAlbums()` serves both recent lists. `recentlyAddedMenu()` sorts by `addedOf()` newest first: per item `firstSeen`, or `created`
  for a server item, the earliest of an entry's items; `recentlyPlayedMenu()` by `playedOf()`, the
  latest `lastPlayed` of the items; both ties by title, at most `kRecentAlbums` (50), times
  below `kEarliestPlayTime` (`track_cache.h`, 2020-01-01) ignored, `albumList(..., false)`
  keeps the order. An album that plays from Music calls `markPlayed(item id)` (the primary item:
  `albumEntry()` passes its id to `albumMenu()` / `openRemoteAlbum()` / `trackItems()`), which
  reads `clock_.wallTime()`, drops a time before the floor and calls `LibraryIndex::markPlayed()`.
  That sets `lastPlayed` in memory and remembers the id, but not as changed, so `save()` (after a
  scan, a sync, building Music) does not write it; `saveOnExit()` calls `savePlayTimes()`, which
  adds those ids and saves (exit, sleep, power off, restart). An item that a scan or sync
  rewrites for another reason takes its play time along. The time sits on one item: if that
  item is removed and comes back under a new id (a moved folder, a download deleted and made
  again) the album is not in Recently played any more. `LibraryIndex::upsert` leaves
  `firstSeen` 0 while the clock was not set (before `kEarliestPlayTime`); the album is left out of Recently added
  until a later upsert with a set clock fills it in. Play all, Streaming and Downloaded mark
  nothing.
- **From item to `Book`.** `App::bookOf()` makes the `Book` that plays: its files are
  the item's tracks in order, with no part titles (a local file is named by its file
  name, as before). A book's `path` is its folder or file, the part of its id after
  `local:book:`, so `pos:<path>` and `last_book` keep their meaning. A folder of
  untagged music has the folder as its path, a tagged album the folder of its first
  file (music keeps no place). `start()` resumes `last_book` from the index when it
  lists the book and its files exist; a book the index does not know yet (the first
  start after an update) is looked up with `scanLibrary()`, the only call left.
- `scanLibrary(root)` (used by the scanner and for that fallback): each **sub-folder** is one book, containing all audio
  files under it sorted by path. Each **audio file at the top level** is also a
  book (typically M4B).
- Chapters: a multi-file book has **one chapter per file**, with leading track
  numbers removed from the title ("01 - Storm" becomes "Storm"). A single-file
  book uses the **embedded chapters** (M4B, MP3 CHAP via FFmpeg). With neither,
  the whole book is one chapter.
- **Music** lives in its own folder (`App`'s `musicRoot`, simulator `--music DIR`,
  default `./music`) and is scanned in the same background scan, but grouped by its tags
  into albums (see "The index" above): an untagged file is grouped by its folder under the
  music root, and a top-level untagged file is a loose track. Music reuses the book playback code (`book_` holds the album or the
  "Play all" list, `music_` is set), but it says "track" instead of "chapter", ends with
  "End of queue", and **saves its queue position in `queue.tsv`, not a book `pos:` key, and does not change `last_book`**. Playing music
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
- An explicitly selected remote track that cannot be opened reports the error without
  trying another selection. Automatic music-queue advancement instead skips unavailable
  entries as described in section 7; the book player stops for an explicit retry. What was current
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
  makes once per link, from the cache root and that server's url and user (its `ServerLink`): the
  servers cannot change while the player runs and a factory reset keeps `servers:`, so
  another server's downloads are never used: the index and the cache are per link, and a
  track id is looked up only in its own server's), else from `remoteStream()` as above.
  The file is a local path: it opens at once (no "Loading."), seeks by itself and
  has no transcode offset, whatever the Streaming quality. A file that is gone is
  not found, and one that fails to open falls back to the stream. With `cache.upgrade`
  on, a cached copy below Streaming quality is bypassed while that server is not
  marked down (`upgradeCached()`; original is best, then higher kbps). Read-ahead
  pre-opens that better stream too. If opening fails immediately, asynchronously or
  at the open deadline, `openCachedFallback()` marks the server down and opens the
  cached file at the requested position, preserving Play/Pause intent without an
  unreachable error. Cancelling the pending open never starts the fallback.
  Other cached tracks need no preload. A cached track is
  scrobbled like a streamed one (it is a play on the server's account; offline
  its submission waits in the scrobble outbox). The menus do not change.
- **Cache index:** `cachedFile()` is called once per tick near the end of a
  track, so it never reads the disk by itself. It keeps `TrackCache::index()` (track
  id to file, for the server in the config) in memory, with `LinkState::cacheQualities`
  (file path to bitrate) filled by the same manifest pass, built at the first call and
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
  "Play from the cache") and the queue is kept like for a server list. Delete
  download opens a list of "Cancel" (selected) and "Yes, delete". `deleteDownload()`
  refuses with a spoken reason when the list is the book loaded in the player; it
  cancels the download when that list is downloading; then `TrackCache::remove()`,
  `cacheIndex_.reset()`, two `popScreen()` (the rebuilt Downloaded list shows what is
  left) and "Deleted Weather.". The folder name keeps an album and a playlist apart;
  the manifest keeps the name and artist.
- **Offline (3.6):** `showStatus()` adds a "Downloaded" item after "Try again" in an error list when `hasDownloads()` (a cache folder and `TrackCache::lists(true)` not empty); it opens `downloadedMenu()`, which never asks the server. "Server status" is not changed: it sits in the Streaming menu, which already has Downloaded. `completeTracks()` builds the playable tracks of a manifest and is shared by `downloadedListMenu()` and `resumeStream()`. Scrobbles of a cached track are still sent; they are fire and forget and silent, so a failure offline says nothing and blocks nothing. A cached next track is not read ahead unless `upgradeCached()` selects a better stream; a server already marked down uses cached tracks directly.
- **Automatic cache (3.7):** `checkAutoCache()` (in `tick()`, after `checkScrobble()`)
  queues the streamed track that plays once it has been heard for `kAutoCacheAfter`
  (10 s of listening, the same count as scrobbling: paused or dry time does not count,
  so a skipped track costs no bandwidth), once per play, unless `cache.limit_gb` is 0.
  The job holds the list's `RemoteSource`, name and artist (`Book::artist`) and the
  track's title, length, `suffix` and `size` (`Book::suffixes`, `sizes`, set by
  `playRemote()`), and the Streaming quality captured when its stream opened.
  Changing either quality setting during playback or while a job waits does not
  change that job. Original streams cache originals; transcodes are fetched again
  at the streamed bitrate into `<safe id>~<bitrate>.mp3`, recorded in the manifest's
  `bitrate` column. `pumpAutoCache()` starts one transfer at a time into the list's
  folder, only while no explicit `download_` runs, the downloader works and the device
  is awake, and drops jobs for sufficient copies already on disk. With Replace
  cached copies enabled, a streamed upgrade queues a replacement after 10 s,
  subject to the cache limit and free-space reserve;
  local fallback playback never queues an upgrade, even after the server recovers.
  The replacement retains pinning and the old file until the manifest commits. A track bigger than the
  limit is never queued (it would be evicted at once and fetched at every play); for
  transcodes this estimate uses duration × bitrate / 8 (kbps converted to bits/s),
  falling back to the original's listed size when duration is unknown. Likewise, a
  track that would leave less than `kAutoCacheReserve` (256 MiB: state, config and
  manifest writes on a small card, with margin for an unlisted size) free is skipped
  silently (`setFreeSpace()` as for explicit downloads). `sleep()` calls `pauseAutoCache()` too (nothing is fetched
  for the cache while the device sleeps; an original continues its `.part` after
  wake, while a transcode restarts it). An explicit download is not paused by sleep
  and ends silently. `startDownload()` calls `pauseAutoCache()`:
  the running transfer is cancelled (its `.part` stays; originals resume and transcodes
  restart later), its job goes back to the head of the queue, and the queue waits
  until `download_` is gone.
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
  after the Play item when `canDownloadFrom(link)`. `startDownload(link, ...)` reads the list's
  manifest, pins existing complete copies and skips those at least as good as the requested
  Download quality (original is best, else higher kbps). A lower copy stays playable and
  protected from eviction while its replacement downloads. It checks the free
  space of the cache root against the sizes of the rest (`App::setFreeSpace()`,
  default `std::filesystem::space()`; transcodes use duration × bitrate × 1000 / 8,
  rounded up, when duration is known, else the original size; an unknown size counts
  as 0, a failed query
  as room enough) and holds one `Download` in `download_`. It asks the
  `Downloader` for one track at a time with the job's captured `downloads.bitrate`
  (0 original, else MP3 kbps). Originals go into `<list folder>/<safe id>.<suffix>`,
  transcodes into `<safe id>~<bitrate>.mp3` so a different quality never resumes
  incompatible bytes. Subsonic transcodes use `stream` with `format=mp3` and
  `maxBitRate`; original-only Books providers use bitrate 0. A transcode starts its
  `.part` over without Range and ignores the original size when checking a response
  of unknown length; original retries still resume. Each
  `Ok` (whole by the downloader's own check, see `Downloader` in `services.h`; `App`
  compares no sizes) is added to `manifest.tsv` pinned, and updates the cache index. The
  prior quality file is deleted only after the replacement manifest commits; a failed
  transfer or manifest save leaves the prior copy available. The
  loaded, parked and previous local music queues are retargeted to the replacement
  path without changing their order or settings; the saved queue is updated before
  the old file is retired. Actions in an already-open Music menu resolve their
  earlier paths to the replacement when played or added to the queue. The
  manifest then follows the server's order, and pinned tracks the server no longer lists
  are dropped with their files (`followServerOrder()`); a track listed twice counts once. A failure is spoken once and drops the `Download`; OK on the item starts again
  and skips sufficient whole copies; an original `.part` continues and a transcode restarts. The download belongs to no screen,
  so leaving menus never cancels it; `tick()` calls each link's `downloader.poll()`, and
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
  are never scrobbled. Nothing about a scrobble is ever spoken. "Now playing" is sent once
  with no time and no `done`, and is never kept. A submission goes through the **scrobble
  outbox** (`outbox_`, fixes.md "Scrobbles"): `submitScrobble()` puts it in with the time it
  was heard (`clock_.wallTime()` in ms) and the key of the server that played it
  (`servers_[link].key`, never the token), writes `scrobbles.tsv` at once, and only then
  `sendScrobbles()` sends the oldest entry of the first server in the list that has one and is not marked down,
  to that server, with `scrobble(id, true, timeMs, done)`, one at a time. The entry leaves the outbox
  (and the file) when the server answers: `Error::None` or `Error::Server` (a refusal is not
  sent for ever); `Unreachable`, `BadLogin` and `NotConfigured` keep it and stop the round for that server (`scrobbleFailed_`: the entries of the other servers still go; a server that answers, or `sendScrobbles()` from any event, tries all again).
  A round starts again whenever the server answers: `start()` (after `loadScrobbles()`),
  `clearServerDown()` (a ping, or a stream that opens), a finished sync, a loaded page or
  pick, and a successful scrobble (the next entry). More than `kMaxScrobbles` (1000) entries
  drop the oldest; entries of another server stay in the file and wait for it. A
  submission whose answer was lost (exit, power cut) is sent again at the next start, so
  the server may count it twice. `Subsonic` also logs a failure to stderr by its kind,
  never with the URL.

## 9. Persistence

`App` takes two `Settings`: `config` and `state`. Each is a flat map of dotted
keys in memory. Core never sees a file: `load()` and `save()` go through a
`SettingsStore`, which the front end gives it (`SettingsFiles`), and the tests use
`FakeStore` or no store at all.

| File | Store | Keys |
|------|-------|------|
| `config.yaml` (`--config`; simulator `~/.config/oap-sim/`, headless `~/.config/oap/` or `$XDG_CONFIG_HOME/oap/`) | `YamlStore` | `speech.rate`, `volume`, `verbosity`, `skip.back`, `skip.forward`, `rewind_on_resume`, `buttons.long_press_ms`, `menu.wrap`, `power.auto_sleep_min`, `streaming.bitrate` and `downloads.bitrate` (kbps, 0 = the original file; download quality defaults to original), `cache.upgrade` (0 or 1; default 0), `cache.limit_gb` (0, 1, 4 or 16; default 1), and `servers.*` (or legacy `server.*`; see [audiobookshelf.md](audiobookshelf.md#configuration-a-list-of-servers)) |
| `state.tsv` (`--state`, default next to `config.yaml`) | `TsvStore` | `last_player`: `music` or `book`, the player used last; `unit.music` / `unit.book`: the step unit of each player (0 Skip, 1 Track / Chapter, 2 Album / 10 minutes); `last_book`: the book to resume on start (never music): a local book's absolute path, or a server book's item id `<server key>:album:<album id>` (4.12); `catalogue.<server key>.version`: the catalogue version of the last sync with that server; `catalogue.<server key>.folder`: its `audiobook_folder` at that sync (only when one is set); `pos:<book path>`: `"<file index>:<seconds>"` (a server book's key is `pos:<server key>:album:<album id>`); syncing server books also use `posat:<id>` (recorded wall seconds), `pushed:<id>` (acknowledged timestamp), `pushdirty:<id>` (pending changed place) and `finished:<id>` (completion retained until playback restarts), where `<id>` is `<server key>:album:<album id>`; `last_stream.*` (`.server`, `.user`, `.kind`, `.id`, `.title`, `.track`, `.track_id`, `.pos`): only read, from a state.tsv of an older version (below); it is removed once a music queue plays |
| `scrobbles.tsv` (next to `state.tsv`; `defaultScrobbleFile()`) | `App` through `TextFiles` (`writeAtomically()`) | the scrobble outbox (`app/core/scrobble_file.{h,cpp}`), see below |
| `queue.tsv` (next to `state.tsv`; `defaultQueueFile()`) | `App` through `TextFiles` (`writeAtomically()`) | the music queue (`app/core/queue_file.{h,cpp}`), see below |

- `config.yaml` is nested YAML (`speech: {rate: 220}` is `speech.rate`). It is
  written only when a setting is chosen in its list (at once), on exit and sleep if the
  volume changed (the volume keys), and by Factory reset.
  Writing drops comments; the file gets a header naming the keys, and keys in a
  fixed order (in an entry of the `servers:` list: type, name, url, user, password, api_key,
  audiobook_folder, library). Numbers are written plain and everything else quoted, the password
  and the `api_key` always. A file that does not parse, or is there but cannot be read, is reported
  on stderr, the defaults are used, and it is never overwritten. A `config.yaml` or
  `state.tsv` that is a symbolic link stays one: the file it points to is replaced.
- **Listening positions are written on exit and on sleep**, to spare the flash: Power off,
  Restart, Sleep and `App::shutdown()` (the front ends call it on SIGTERM / SIGINT /
  SIGHUP and when the window closes). Pause and a book change only update the map
  in memory, so switching back to a book still resumes it. The file is written
  only when the map differs from what was last loaded or written (a write that
  failed counts as not written, so the next exit tries again). Factory reset also
  writes state immediately; an acknowledged remote progress save may write its
  acknowledgement while sleeping or shutting down. There is no periodic local
  position checkpoint. A chapter-boundary checkpoint is only a proposal pending
  design and write-wear measurements. A crash or power loss loses the local place
  since the last successful save.
- **scrobbles.tsv (fixes.md 3.2).** The scrobble outbox: a header `oap-scrobbles<TAB>1`, then one line per entry,
  oldest first: `server key<TAB>track id<TAB>time` (the key is `TrackCache::serverKey()`, the time ms since 1970;
  tab, line break and backslash escaped as in queue.tsv). Never a URL, a token or the password. `saveScrobbles()`
  replaces the file (atomically) each time the outbox changes and its text differs from what was read or last
  written (`scrobbleText_`): at most two writes per heard track. `loadScrobbles()` in `start()` reads it
  (a file over `kMaxScrobbleFileBytes`, not ours, or a damaged line gives no entries or skips the line; at most
  `kMaxScrobbles` newest). An empty path or no `TextFiles` keeps the outbox in memory only.
- **queue.tsv (queue.md step 2.8).** The music queue, written by `saveQueue()` in `saveOnExit()` (sleep, Power off,
  Restart, `shutdown()`) as text (`formatQueue()` in `app/core/queue_file.cpp`): a header `oap-queue<TAB>1`, then
  `title` (the list it came from), `shuffle`, `repeat`, `edited`, `position` (in the play order), `seconds` (in the
  current entry), `order` (the play order, indexes into the entries) and one line per entry in the original order:
  `L<TAB>path<TAB>title<TAB>album<TAB>artist` or `R<TAB>server url<TAB>user<TAB>track id<TAB>album id<TAB>title<TAB>album<TAB>artist<TAB>seconds`.
  Tab, line break and backslash in a text are written `\t`, `\n`, `\r`, `\\`. A remote entry holds the server key
  and the track id only, never the password, a token or a stream URL (a stream is made when the track plays), and
  the titles are there, so a restart lists the queue with no network. The queue that is saved is the loaded one, else
  the parked one (`Parked::queue`), else (nothing has played since the restart) none, because the restored queue is
  loaded. More than `kMaxSavedEntries` (5000) entries are cut to a window of the play order around the current
  entry (`Queue::around()`). The file is replaced (atomically) only when its text differs from what was read or
  last written (`queueText_`), so a queue paused where it was writes nothing; an empty file is written once when
  the queue is gone (Clear queue, Factory reset), and a failed write is tried again at the next exit. The previous
  queue (`previous_`) is not kept.
  `start()` calls `restoreQueue()`: a file that is missing, longer than `kMaxQueueFileBytes`, not ours or damaged
  (`parseQueue()` gives none) means no queue and is replaced later; a play order that is not each index once, or a
  position past the end, is repaired (`Queue::restore()`: original order, shuffle off, first entry, 0 seconds).
  `retitleDownloads()` then gives an entry in the cache folder whose title is its own file name the title the index
  has for that download (an older Play all saved a downloaded track under its file name, the server's track id).
  It looks at the index only when there is such an entry, and changes titles alone (`Queue::setTitle()`): the
  order, place, shuffle and repeat stay as read.
  The queue becomes the music queue in `book_` (only its title) with `music_` set, closed like a queue that ended
  (`reopenAt_` is its place, `restoredQueue_` is set); nothing plays. If a book is opened next (it played last) the
  music queue is parked as always. "Now playing" on Home, when the restored queue is chosen, opens its current entry
  at that place and enters the player (`openPlayer()`), and Play does the same without entering it; an entry
  that cannot play is skipped like in any queue. The `Book` of a restored queue has no `remote` or `source`, so the
  automatic cache does not run for it until another list is played.
- **Legacy `last_stream.*` migration only.** `rememberPosition()` no longer writes them. When a state.tsv has them and
  queue.tsv has no queue, `savedStream()` still makes "Now playing" on Home, and OK or Play runs `resumeStream()`
  (below) as before; the queue it plays is saved in queue.tsv from then on, and any music queue that plays removes the
  keys (`switchTo()`), as does reading a queue.tsv.
- **Which place start() resumes:** the one that played last. The books keep their place in `pos:` + `last_book`
  and the music queue in queue.tsv; neither removes the other. `last_player` (`music` or `book`, set when a player starts)
  says which was used last; without it (an older state.tsv) the music player
  counts as last when a music queue or a legacy stream is saved. A restored `queue.tsv`
  supplies the music player's normal restart path, including mixed local and remote
  entries; the following `savedStream()` path applies only when there is no saved queue.
  If `savedStream()` finds a place for the server set up
  now (same url and user, and `MusicServer::configured()`) and the music player was last, `start()`
  opens Home with "Now playing" first and loads nothing; otherwise `last_book` resumes as before,
  and the stream is "Continue" on Home. OK on it, or Play with
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
- The volume keys change the volume in memory (`volumeChanged_`); it is written on exit and sleep, with the state. The step unit of each player is kept in memory and written to `state.tsv` with the rest of the state. Factory reset also writes `config.yaml` (server keys kept).
- **Factory reset** clears both, but keeps the `server.*` keys, and restarts
  without saving the position first, so it does not come back.
- Every write is atomic (`writeFileAtomically`): `FILE.tmp` created with mode 0600,
  `fsync`, `rename`, then `fsync` of the folder. A power cut leaves the old file.
- The old single `settings.tsv` is not read any more; settings start from the defaults.
- **`library.db`** (`--library-db`, default next to `state.tsv`) is the library index, written by `SqliteLibraryStore` (section 8 and the source table): only what changed, in one transaction, and the play times (`lastPlayed`) only on exit and sleep. It is a cache of what the disk and the server hold (items and playlists), so a lost or damaged file (moved to `library.db.bad`) is rebuilt by the next scan and sync; Factory reset leaves it alone. Its first-seen and last-played history is not fully
  rebuildable; [fixes.md phase 7](fixes.md#phase-7-listening-reliability-and-durable-data)
  plans separating authoritative history/identity from regenerable metadata.

## 10. Simulator (`app/sim/main.cpp`)

- **Window:** SDL2 plus Dear ImGui (v1.91.8, downloaded by CMake into `build/_deps`).
  - Left: the **menu stack as columns**. The selected item is highlighted, `>`
    marks a submenu. On the player screen it
    shows the player key help instead.
  - Right: clickable **device buttons** (press and hold work), the hold switch,
    **now playing** with a progress bar, a table of **button usage**, and the
    **speech log**.
- **Keyboard:** Space = Play/Pause, arrows = Back/Forward/Up/Down, Enter = OK,
  Esc/Backspace = Home, T = Talk, P = Power, + / - = volume, O = Options, H = hold switch. Holding a key gives a long press.
- **Game controller:** SDL game controllers (DualSense, Xbox, ...) go through `GamepadMapper`,
  the same map as `oap-headless`: D-pad = arrows, Square = Play/Pause, Cross = OK,
  Circle = Home, Triangle = Options, R1 / R2 = volume up / down, L1 + Left/Right = deep press. Pads can be plugged in while it runs.
- **Arguments:** `oap-sim [--config FILE] [--state FILE] [--music DIR] [--cache DIR] [--library-db FILE] [--audio NAME] [--font TTF] [--script STEPS] [LIBRARY_DIR]`.
  `--library-db` moves the library index (default `library.db` next to the state file). `--audio` is `auto` (default), `coreaudio`, `pulse`, `alsa[:DEVICE]` or `null`, among those built in.
  `--font TTF` is the font of the window (below).
  `tools/make_sample_library.sh [LIBRARY_DIR] [MUSIC_DIR]` makes a spoken sample library and some tone "music".
- **Font (4.1):** names in any script must show, not "?". `addSimFont()` asks `pickFont()` for the first
  existing regular file of: the `--font` path, `/System/Library/Fonts/Supplemental/Arial Unicode.ttf` (macOS),
  `/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc`, `/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf`.
  ImGui 1.91 bakes a static atlas, so the glyph ranges are named: Latin-1 and Extended-A, Greek,
  Cyrillic, Hebrew, Arabic, Japanese, Chinese simplified common and Korean. A font draws only the ranges it has
  (DejaVu has no CJK). A `--font` path that does not exist prints `sim: font PATH not found` and the
  next candidate is tried; a file that is not a font prints `sim: cannot load font PATH` and the next
  is tried. With none left it prints `sim: no font with Cyrillic and CJK found; use --font` and uses
  ImGui's built-in font.
- **Script mode:** `--script "ok down ok wait:3000 right:long play:long quit"`
  presses buttons automatically, for repeatable walkthroughs and future UI tests.
  Steps: `play left right up down ok home talk power volup voldown options`, with `:long` for a long
  press, plus `wait:<ms>` and `quit`. An unknown step or a `wait` without a number is printed and skipped.
- **Several inputs at once:** keyboard, pads, on-screen buttons and `--script` share one
  `HeldButtons`, so a button held from two of them is released only when both let go.

## 11. Headless front end (`app/headless/`) and the Linux device

- **Arguments:** `oap-headless [--config FILE] [--state FILE] [--music DIR] [--cache DIR] [--library-db FILE] [--audio NAME] [--input LIST]
  [--evdev-device PATH] [--script STEPS] [LIBRARY_DIR]`. The config defaults to
  `~/.config/oap/config.yaml`, the state to `state.tsv` next to it (section 9), the cache of downloaded tracks to `cache/` next to the state, the library index to `library.db` and the music queue to `queue.tsv` next to the state.
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
    terminal sends no releases (so a volume key steps once and has no long press). `+` `=` / `-` are volume up / down, `o` is Options. Alt / `l` prefix = long, Shift / `d` prefix = deep.
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
| Paths | Library under `/data/library`, `config.yaml` and `state.tsv` under `/data/config` (`--config /data/config/config.yaml`), the download cache in `/data/library/cache` (`--cache /data/library/cache`; without `--cache` it is `cache/` next to `state.tsv`). `scanLibrary()` skips it, and so does the scan. The library index is `/data/config/library.db` (`--library-db`, or next to `state.tsv`). |

Nothing in `app/core/` uses macOS APIs.

## 12. Tests

```sh
ctest --test-dir build --output-on-failure    # or ./build/oap-tests
```

- `oap-tests` links only `oapcore`, `oapconfig`, `oaplibrary` (SQLite, system or fetched), `oapmusicserver` (and through it `oapfrontend`) and `oapaudio`, so it builds without FFmpeg, eSpeak NG,
  SDL2, audio libraries or libcurl (`cmake -DOAP_BUILD_SIM=OFF -DOAP_BUILD_HEADLESS=OFF`, where `OAP_BUILD_NET` defaults to `OFF`);
  yaml-cpp is downloaded if missing. With `OAP_BUILD_NET` it links `oapnet` and libcurl through `oapmusicserver`. Without a front end
  `oapaudio` holds only the null output; the device backends are added with the sim or headless build.
- `tests/fakes.h` provides `FakeSpeech` (records what was said), `FakePlayer`
  (position, end of file and chapters set by the test), `FakeSounds`, `FakeVolume`,
  `FakeClock` (time only moves when the test advances it), `FakeStore` (what was
  last saved to a settings file, and how often) and `FakeMusicServer` (records each
  request; answers only when the test calls `reply` / `replyPing` / `replyVersion` / `replyBook` / `replyProgress` / `fail`, and then
  only on the next `poll()`; honours `cancel`; its stream URLs show the quality,
  `fake://stream/<id>?maxBitRate=<kbps>`, and with `saltUrls` differ on each call, as a
  real server's do; `scrobble()` calls are recorded in `scrobbles` with their time, answered by the next `poll()` with `scrobbleError`, or held until `answerScrobble()`). `FakeLibraryStore` keeps the index in memory and counts saves, `FakeTagReader` maps path to tags and counts reads, and `FakeLibraryScanner` records each `ScanRequest` and delivers only on `finish()` + `poll()`. `FakeDownloader` records each `download()` and finishes only when the test calls `finish(id, result)` (which writes `dest` for `Ok`, or a `.part` for a failure, if asked) or `progress()`, then on the next `poll()`; honours `cancel`. `FakePlayer` records the `Transcoded` of the last open.
  `progressSaves` records progress writes; `progressSaveError` selects the next automatic result,
  or `holdProgressSaves` and `answerProgressSave()` control acknowledgements explicitly.
  `test_fake_music_server.cpp` checks that contract.
- `test_queue.cpp` tests `Queue` alone (order, shuffle, repeat, edits, `restore()`, `around()`) with a chosen random
  generator; `test_queue_file.cpp` tests `formatQueue()` / `parseQueue()` (round trip, escapes, damaged files, no
  stream URL or token). The queue in `App` (mixed play, skipping, server checks, Options, the Queue list, `queue.tsv`,
  Save as playlist) is in `test_app.cpp`; `test_fake_music_server.cpp` and the net tests cover `createPlaylist`.
- `test_config.cpp` tests YAML ⇄ dotted keys, `YamlStore` / `TsvStore` on real files
  (mode 0600, atomic replace, a broken file left alone) and `SettingsFiles`.
- `test_app.cpp` drives `App` the way a user does, through `keyDown`/`keyUp`/`tick`
  on a temporary library folder, and checks what is spoken, which file plays and
  what is saved. Long presses, sleep timers and auto sleep are tested by advancing
  the fake clock.
- `test_terminal_keys.cpp` tests the headless terminal key decoding, including sequences split across reads.
- `test_presses.cpp` tests `parseScript` and `PressSequencer` timing; `test_held_buttons.cpp` tests `HeldButtons`.
- `test_gamepad.cpp` tests the gamepad map, including the order of light and deep stages.
- `test_library_index.cpp` (items, `seenOn` lookups, what `save()` hands over), `test_sqlite_library_store.cpp` (round trips, one transaction, a damaged or newer file moved aside, mode 0600), `test_threaded_scanner.cpp` (real folders with `FakeTagReader`: unchanged files are not read again, grouping into albums, cancel) and `test_album_merge.cpp` (`mergeAlbums()`, the name rule, `albumInfoText()`) test the library without `App`; the menus built on them are in `test_app.cpp`.
- `test_no_music_server.cpp` tests `NoMusicServer` (answers only from `poll()`, `cancel`, also from a callback).
- `test_make_music_server.cpp` tests `makeServers()`: no server, two Subsonic entries (names, keys, each link on its own server), the lone server's name, another server type, the names with an unusable entry in between, and what the config ignores; links exist only in a build with `OAP_BUILD_NET` (CMake tells the test which build it is in).
  which streams only in a build with `OAP_BUILD_NET` (CMake tells the test which build it is in).
- `oap-net-tests` (built with `OAP_BUILD_NET`) tests `Subsonic` URL building, auth
  and error mapping over a fake `Http`, the parsers on saved answers in
  `tests/data/subsonic/` (non-ASCII and emoji titles, `\uXXXX` surrogate pairs, `\/`,
  an 80 kB single-line page), and `HttpClient` against sockets on 127.0.0.1 (refused,
  stalled, trickling, too large, cancelled, destroyed while stuck). `Audiobookshelf` is tested the same
  way (`test_audiobookshelf.cpp`, `test_audiobookshelf_json.cpp`) on `tests/data/audiobookshelf/`; a test checks that
  no fixture holds `"token"`, `token=`, a JWT start or the key from `.env`. Progress tests cover
  GET/404, PATCH JSON and save acknowledgements, save-only exit draining, and Continue listening’s
  library filtering. `HttpClient` socket tests check PATCH method, headers, body bytes and cancellation.
  App tests cover push timing/retry/completion, pull notices and seeks, stale callbacks and
  Continue listening across files and downloads. No internet.
- `oap-media-tests` (built with the simulator or headless front end, as it needs FFmpeg) tests
  `FfmpegTagReader` (`test_tag_reader.cpp`) on MP3, FLAC, M4A and Opus files written with the `ffmpeg`
  command (a format whose encoder is missing is skipped), and
  `Player` on WAV files it writes itself; the format-change tests make MP3s with the `ffmpeg`
  command and are skipped without it. Two tests capture stderr to check that a URL that fails
  to open is not printed.
- `test_audio_output.cpp` tests `AudioOutput::open()` with the null backend, and
  `ThreadedAudioOutput` with a fake device that is lost and comes back.
- The other files test `GestureDetector`, `AudioStream`, `Mixer`/`Earcons`,
  `Settings`, `scanLibrary` and `spokenDuration` directly.
- Every test has been checked with a sabotage: break the code it guards and
  watch it fail. See [REPO.md](../REPO.md).

## 13. Known gaps / next steps

- Speed control (libavfilter `atempo`, or Sonic), skip silence.
- Bookmarks and podcasts (RSS). Audiobookshelf books, chapters, downloads and listening-progress
  sync are implemented ([audiobookshelf.md](audiobookshelf.md)); phase 3 still needs real-server
  and device listening checks, including the later-place notice and offline retry.
- Streaming phases 2a and 5 in [streaming.md](streaming.md): star / unstar and a Starred
  list, and sync. The limits of phase 4 (one library) are listed under "Known limits after
  phase 4" there.
- STT (Talk button): push-to-talk capture plus sherpa-onnx, Vosk or whisper.cpp.
- Localisation of spoken strings; a choice of TTS voice. Kanji and Han text are read by the `cmn` voice; without it they are only named ("Japanese text", "Chinese text"): there is no reading table. Hebrew and Arabic need their voice, else they are named.
- Hardware abstraction (battery, headphone jack, Wi-Fi, Bluetooth, suspend): planned in [hal.md](hal.md).
- Queue: the Previous queue is not kept across a restart; a restored queue does not cache tracks
  automatically until another list plays; Star / Unstar in the player's Options (streaming.md 2a.1);
  Save as playlist saves at most 150 tracks (a GET request) and has no renaming until STT or a keyboard;
  play queue sync with the server and offline playlists are later (queue.md "Later").
- The `AudioStream` mutex in the audio callback could become a lock-free ring.
