# Audiobookshelf and several servers: plan

> Status: phases 1 (several servers) and 2 (Audiobookshelf books: Library, the Books menu, server chapters, downloads) are built; phase 3 (progress sync) is a plan. The code is described in [architecture.md](architecture.md),
> the menu rules in [navigation.md](navigation.md) (see [Lists from the server](navigation.md#lists-from-the-server)
> and [Music and Library](navigation.md#music-and-library)). The Subsonic plan this follows is
> [streaming.md](streaming.md). Last updated: 2026-10-29.
> Target server: Audiobookshelf 2.37 (API keys exist since 2.26). Test server:
> `https://audiobooks.microserver` (a Books library with two books, a Podcasts library with none;
> its certificate verifies without extra setup). The credentials are in `.env` (`OAP_BOOKSHELF_*`;
> git ignores it) only so they are at hand during development; the program does not read it.

Before this plan `config.yaml` had one `server:` section, and only `type: subsonic` worked. `App`
held one `MusicServer` and one `Downloader`. Server books came from one music folder of the
Subsonic server (`server.audiobook_folder`), and their chapters were the album's tracks.

The plan added (1 and 2 are built, 3 is not):

1. **Several servers** in `config.yaml` (`servers:`, a list), each with its own type. Every
   server gets its own Home entry, its own catalogue sync, cache folder and downloads. Phase 1
   does this with Subsonic only, so it can be tested before a second type exists.
2. **Audiobookshelf** (`type: audiobookshelf`) as the second type: its books are listed in
   Library, play in the book player with the server's chapters, and can be downloaded (phase 2).
3. **Progress sync** with Audiobookshelf, both ways, as SPEC §6.3 asks (phase 3).

Podcasts from Audiobookshelf come later (see [Later](#later)).

---

## What Audiobookshelf gives us (checked against the test server)

All API requests carry `Authorization: Bearer <api_key>`. A wrong or missing key gives HTTP 401.

| Need | Endpoint | Notes |
|------|----------|-------|
| Check the server | `GET /status` | No auth. `{"app":"audiobookshelf","serverVersion":"2.37.2",...}`. |
| Check the key | `GET /api/me` | 200, or 401 for a wrong key. **The answer holds `token`, the user's own token**: it is never logged or saved, and a fixture of it has it removed. |
| Libraries | `GET /api/libraries` | `libraries[]` with `id, name, mediaType` (`book` or `podcast`), `displayOrder`. |
| Books | `GET /api/libraries/<id>/items?limit=&page=&sort=&desc=` | `results[]`, `total`. `sort=media.metadata.title` for A to Z, `sort=addedAt&desc=1` for newest. Each result: `id`, `addedAt` (ms), `media.metadata.{title, authorName, genres[], publishedYear}`, `media.{numAudioFiles, numChapters, duration, size}`. |
| One book | `GET /api/items/<id>?expanded=1` | `media.tracks[]` in play order: `index, ino, duration, startOffset` (seconds into the book), `metadata.{filename, ext, size}`, `mimeType`, `contentUrl` (`/api/items/<id>/file/<ino>`). `media.chapters[]`: `id, start, end, title`, **on the book's timeline**, not per file. |
| Authors | `GET /api/libraries/<id>/authors` → `GET /api/authors/<id>?include=items` | `authors[]` with `id, name, numBooks`; `libraryItems[]` like the book list. |
| Audio | `GET /api/items/<id>/file/<ino>` | The original file. HTTP Range works (206 and `Content-Range`). Auth by the header or by `?token=<api_key>` in the URL; with neither, 401. |
| Progress | `GET /api/me/progress/<id>`, `PATCH /api/me/progress/<id>` | **404 means no progress yet**, not an error. Body: `currentTime` (seconds on the book's timeline), `duration`, `progress`, `isFinished`, `lastUpdate` (ms). |
| In progress | `GET /api/me/items-in-progress` | `libraryItems[]`, for "Continue listening" (phase 3). |

Seen on the test server, and the reason for some decisions below:

- The single-file book (`.m4b`, 23 chapters) has one track and the chapters inside it. The
  24-file book has **14 chapters**: chapters span files. So a chapter is not a file, unlike a
  Subsonic server book.
- The library's `lastUpdate` is older than its items' `updatedAt`. There is no reliable cheap
  "catalogue changed?" check, so a sync always fetches the book list (see Decisions).
- Titles and authors are Cyrillic; file names carry them too.

---

## Configuration: a list of servers

```yaml
servers:
  - type: subsonic
    name: Music               # spoken on Home; see "Names" below
    url: https://navidrome.microserver
    user: ice
    password: "..."
    audiobook_folder: Books   # optional, Subsonic only, as before
  - type: audiobookshelf
    name: Audiobooks
    url: https://audiobooks.microserver
    user: ice                 # optional; with the url it names the server in saved state
    api_key: "..."            # Settings → API Keys in the Audiobookshelf web page
    library: Books            # optional: the book library to use; default the first by display order
```

- **Flattened as today.** `YamlStore` already turns a list into `servers.0.type`,
  `servers.1.url`, ... and writes it back as a list (`ListsAreWrittenBackAsLists`). Core reads
  them with a new `serverConfigs(const Settings&)` in `app/core/settings.{h,cpp}`, which returns
  `std::vector<ServerConfig>`:

  ```cpp
  struct ServerConfig {
      std::string type;            // "subsonic" (also when left out) or "audiobookshelf", lower case
      std::string name;            // as set; empty when not set
      std::string url, user;       // url without trailing slashes
      std::string password;        // subsonic
      std::string apiKey;          // audiobookshelf
      std::string audiobookFolder; // subsonic
      std::string library;         // audiobookshelf
  };
  ```

  An entry without a `url` is left out (as "not configured" is today).
- **The old `server:` section still works.** When there is no `servers:` list, a `server:`
  section is read as a list of one. The device never rewrites it into `servers:`; the keys are
  written back as read, like all server keys. With both, `servers:` wins and a line on stderr
  says `server:` is ignored.
- **Names.** The Home entry is the server's `name`. Without one it is "Server" when only one
  server is set up, else "Server 1", "Server 2", ... by its place in the list.
- **The identity of a server** stays `TrackCache::serverKey(url, user)` (host + hash, no
  secret): cache folders, `seenOn` references, `pos:` keys, `catalogue.*` keys. `queue.tsv`
  keeps url + user per remote entry, as today. Two entries with the same url and user are one
  server: the second is left out with a line on stderr.
- **Secrets.** `api_key` is always written quoted, like `password`, and never logged or
  spoken. An Audiobookshelf stream or download URL carries `token=<api_key>`, so it falls
  under the rule for stream URLs in CLAUDE.md.
- **Factory reset** keeps `servers.*` as well as `server.*` (`Settings::clearExcept` takes a
  list of prefixes).
- **Header of `config.yaml`** names the new keys: `servers:` (a list), `type`, `name`, `url`,
  `user`, `password`, `api_key`, `audiobook_folder`, `library`. Key order in each entry:
  `type, name, url, user, password, api_key, audiobook_folder, library`.

## Architecture

```
app/core/      App ── takes std::vector<ServerLink>; one Home entry per server
                │     services.h: MusicServer gets book(), progress(), saveProgress(); RemoteBook, RemoteChapter
                └── settings: ServerConfig, serverConfigs()
app/net/       Http: GET with headers, plus PATCH with a body (phase 3)
               Subsonic        : MusicServer (unchanged but for book())
               Audiobookshelf  : MusicServer (new) + audiobookshelf_json.{h,cpp}
app/frontend/  makeServers(config) → Servers: one MusicServer + one Downloader per entry
```

- **One interface, `MusicServer`.** Audiobookshelf maps onto it: a book is a `RemoteAlbum`,
  an audio file a `RemoteTrack`, an author a `RemoteArtist`, the chosen library the only
  `RemoteFolder`. What it cannot do answers at once through `poll()`: `playlists` an empty list,
  `createPlaylist` `Error::Server`, `scrobble` nothing. Two additions, which Subsonic implements
  too:

  ```cpp
  struct RemoteChapter { std::string title; double start = 0; }; // seconds on the book's timeline
  struct RemoteBook { std::vector<RemoteTrack> tracks; std::vector<RemoteChapter> chapters; };
  // A book's files and chapters in one request. Subsonic: getAlbum, no chapters (they are the tracks).
  virtual RequestId book(std::string albumId, std::function<void(Error, RemoteBook)> done) = 0;
  // What the server's lists are made of: Music (Subsonic) or Books (Audiobookshelf).
  enum class Catalogue { Music, Books };
  virtual Catalogue catalogue() const = 0;
  ```

- **`ServerLink`** (in `services.h`) is what `App` gets per server:
  `{ ServerConfig config; std::string key; std::string name; MusicServer& server; Downloader& downloader; }`
  (`key` is the server key, `name` the spoken name after the rules above). The front ends own
  the objects in a `Servers` (`app/frontend/make_music_server.{h,cpp}`, `makeServers(config)`),
  which replaces `makeMusicServer` and `makeDownloader`. With no server, `App` gets an empty
  list. `App::tick()` polls every server and downloader.
- **Ids on Audiobookshelf.** A book (`RemoteAlbum.id`) is the library item id. A file
  (`RemoteTrack.id`) is `<item id>/<ino>`, which builds the file URL with no lookup. It must
  survive `TrackCache::safeName`, `seenOn` references and `queue.tsv` (a test in 2.2).
- **Streaming quality does not apply to Audiobookshelf.** It always streams the original file
  (Range seeks); its `stream()` ignores `maxBitRate`.
- **Chapters from the server.** `Book` gets `std::vector<Book::Chapter> chapters`
  (`{title, file, start}`, `start` in seconds into that file), made from `RemoteChapter`s with
  the tracks' `startOffset`. When it is not empty, `App::chapters()` uses it before the
  per-file and embedded chapters. The place is still `pos:<id>` = file index and seconds into
  the file, so nothing in `state.tsv` changes.

## Menus

Home has one entry per server, in config order, between Music and Podcasts, each named as
above. A Music server (Subsonic) keeps today's Streaming menu. A Books server
(Audiobookshelf) gets:

```
<server name>
├── Recently added     → <books>      sort=addedAt desc, one page of 500
├── Books A to Z       → <books>      sort=media.metadata.title, pages of 500
├── Authors            → <author> → <books>
├── Downloaded         (as in Streaming, only where a cache folder is set up)
└── Server status      "Connected to Audiobookshelf 2.37.2." / the error

<book> → Play book · Download book · <chapters>
```

- A book in these lists says its title and author ("Neuromancer, William Gibson").
- **Play book** opens the book player at the book's place (or the start) like a book in
  Library. OK on a chapter plays the book from that chapter. **Download book** behaves like
  Download album ("Downloading, 0 of 24 files."), counting files, not chapters.
- **Library** lists the books of every Books server after the local books, with the value
  "on the server", as server books are listed today. Two servers' books are not merged.
- All "Lists from the server" rules hold (Loading, errors, Try again, not rebuilt on return).
- **Refresh library** syncs every server. It says "Updating the library." and then
  "Library updated, 12 albums, 2 books." (counts left out when 0). A server that fails adds
  its name and the error: "Audiobooks: server not reachable." With every server failing it
  says only the errors.
- "No server set up" and "Server not supported" stay as today for a list with no usable
  server. The help text for an unsupported type names both: "This player streams from Subsonic
  servers, such as Navidrome, and from Audiobookshelf, and only when it is built with network
  support. Check the type in the servers section of config.yaml."

---

## Phase 1: several servers

Goal: with two Subsonic servers in `servers:`, Home has an entry for each, Music lists the
albums of both, and a queue that mixes tracks from both plays and resumes after a restart.
An old `server:` section keeps working unchanged.

| # | Step | Tests (each checked with a sabotage, as CLAUDE.md requires) |
|---|------|------|
| ✅ 1.1 | **Docs first:** navigation.md (one Home entry per server, the names, Refresh with several servers, the Books server menu above marked "phase 2"), SPEC §6.3 (Audiobookshelf as a second server type), streaming.md open question 1 answered with a link to this plan. | – |
| ✅ 1.2 | **`servers:` in the config.** `ServerConfig` and `serverConfigs()` in `app/core/settings.{h,cpp}` as in [Configuration](#configuration-a-list-of-servers); `serverConfigured()` becomes "`serverConfigs()` is not empty". `Settings::clearExcept` takes several prefixes and factory reset keeps `servers.` and `server.`. `stores.cpp`: `api_key` always quoted, the key order in an entry, the header. Nothing reads the second server yet. | A `servers:` list of two gives two configs in order; a missing type is `subsonic`; `Subsonic` in any case is lower-cased; an entry with no url is left out; a lone `server:` gives one config; `servers:` and `server:` together use `servers:`; the same url and user twice gives one. An all-digit `api_key` stays a quoted string after a round trip. Factory reset keeps both sections and clears `volume`. |
| ✅ 1.3 | **`App` takes a list of servers.** `ServerLink` in `services.h`; `makeServers(config)` in `app/frontend/` builds a `Subsonic` + `HttpDownloader` per Subsonic entry (other types: the stderr line and no link, as today); both front ends and every test helper pass the list. `App` uses only the first link everywhere it used `server_` / `downloader_`; with an empty list it behaves as with `NoMusicServer` today. No behaviour change. | `makeServers`: two entries give two links with their names and keys; an unknown type gives no link and its stderr line; no network build gives none (both branches of `OAP_TESTS_EXPECT_NET`). The App tests pass unchanged with a one-link list, and an empty list says "No server set up". |
| ✅ 1.4 | **One Home entry per server.** Each opens the Streaming menu of its own link; requests, Server status and the "Loading" state are per screen and per link. Names as above. | Two servers: Home lists "Music" and "Audiobooks" between Music and Podcasts in config order; unnamed, "Server 1" and "Server 2"; one unnamed server is "Server". Recently added on the second server asks only the second fake; Server status names that server's type and version. Leaving a list of one server cancels only its request. |
| ✅ 1.5 | **Play from the right server.** A remote track carries its server key; streams, scrobbles, read-ahead and the automatic cache go to that link. `queue.tsv` entries find their link by url + user; an entry whose server is no longer in the config is skipped like a track that cannot play ("Cannot play Storm. Server not set up."). | A queue with a track from each server opens each stream from its own fake. A scrobble goes to the server that played. After a restart the mixed queue resumes from `queue.tsv` on both. A server removed from the config: its entries are skipped with that phrase and the others play. Nothing spoken or saved holds a URL. |
| ✅ 1.6 | **Downloads per server.** Download album / playlist and the automatic cache use the link's downloader and its cache folder (`<cache>/<server key>/...`, as today). Downloaded in a server's menu lists only that server's downloads; one download at a time across all servers stays the rule. | Downloading an album on the second server calls only the second downloader and writes under its key. Each server's Downloaded lists its own. Starting a download on one server while another server downloads says "Another download is running." |
| ✅ 1.7 | **Catalogue sync per server.** Start-up and Refresh sync every link. The state keys become `catalogue.<server key>.version` and `catalogue.<server key>.folder`; the old `catalogue.server` / `.version` / `.folder` are removed on the first sync (one extra fetch). Items of a key that is no longer configured are dropped; items of every configured key are kept. `audiobook_folder` is per server. Refresh texts as in [Menus](#menus). | Two servers: Music lists the albums of both; a changed version on one fetches only that one; a server removed from the config loses its items on the next sync; one server failing at start-up keeps its saved items and says nothing; Refresh with one failing says the count and "Music: server not reachable."; the old catalogue keys are gone after a sync. |
| ✅ 1.8 | **Docs for phase 1 as built:** architecture.md (servers list, `ServerLink`, per-server state keys), navigation.md where the build differs, README config example, SPEC. | – |

**Device check (manual):** put the Navidrome server in `servers:` twice under two users (or
the same server under another name in a second config), restart: Home says both names, both
menus list albums, and Music lists the albums once per server. Queue a track from each, restart,
"Now playing" resumes the queue. With the old `server:` file the device works as before.

## Phase 2: Audiobookshelf books

Goal: with an Audiobookshelf server in `servers:`, its books are in Library and in its own
Home menu, they play in the book player with the server's chapters and keep their place, and a
book can be downloaded and played offline.

| # | Step | Tests (each checked with a sabotage, as CLAUDE.md requires) |
|---|------|------|
| ✅ 2.1 | **Request headers.** `Http::get` takes a list of headers (`HttpClient` sets them on the curl handle). `Subsonic` passes none. The headers are never logged. | Net test with the local socket server: a header is sent; a request without headers sends none of it. A failed request logs no header value. |
| ✅ 2.2 | **`MusicServer::book()` and `catalogue()`.** `RemoteBook` and `RemoteChapter` in `services.h`; `Subsonic::book` is `getAlbum` with no chapters, `catalogue()` is `Music`; `FakeMusicServer` and `NoMusicServer` follow. No App change. | Subsonic `book()` gives the tracks of the `getAlbum` fixture and no chapters. The fake answers `book()` only when told to. An id `<uuid>/<ino>` round-trips through `TrackCache::safeName` (a file name with no `/`), a `seenOn` reference and `queue.tsv`. |
| ✅ 2.3 | **`Audiobookshelf : MusicServer`** in `app/net/audiobookshelf.{h,cpp}` and `audiobookshelf_json.{h,cpp}` (yaml-cpp, as Subsonic). `Config { url, user, apiKey, library }`. `ping`: `/status` then `/api/me` (401 → `BadLogin`), type "Audiobookshelf", `serverVersion`. `musicFolders`: the `book` libraries, or only the one named by `library`. `albums(listType, offset, size, folder)`: `newest` → `sort=addedAt&desc=1`, `alphabeticalByName` → `sort=media.metadata.title`, others an empty list; `page = offset / size`, `more` while `offset + size < total`; `RemoteAlbum` from `id, title, authorName, genres[0], publishedYear, addedAt` (as ISO 8601 in `created`). `artists` / `artistAlbums`: authors. `book`/`albumTracks`: `/api/items/<id>?expanded=1`. `stream`: `<url>/api/items/<item>/file/<ino>?token=<key>`, no transcode. `catalogueVersion`: an empty version (always fetch). Errors: no answer → `Unreachable`, 401 → `BadLogin`, any other non-2xx or bad JSON → `Server`. Fixtures in `tests/data/audiobookshelf/`, saved from the test server **with `token` removed from `me.json`**. | `oap-net-tests`: every request carries the Bearer header and no key in its URL; the stream URL is right and is the only one with `token=`; ping gives "Audiobookshelf" and "2.37.2"; 401 is `BadLogin`; the book list fixture gives Cyrillic titles and authors and `more` on a full page; the expanded item gives 24 tracks in order with sizes and suffixes and 14 chapters; the m4b gives 1 track and 23 chapters; `library: Books` picks that library and a podcast library is never listed; a fixture contains neither the test key nor `"token"`. A cancelled request's callback does not run. |
| ✅ 2.4 | **Build it from the config.** `makeServers` builds `Audiobookshelf` + `HttpDownloader` (its URL source is the stream URL) for `type: audiobookshelf` with a url and an `api_key`; without the key, the stderr line "servers: Audiobookshelf needs api_key" and no link. Server status works. The help text for an unsupported type changes. | `makeServers`: an Audiobookshelf entry gives a link whose `catalogue()` is `Books`; one without `api_key` gives none and the line. App: Server status on it says "Connected to Audiobookshelf 2.37.2.". |
| ✅ 2.5 | **Books in Library.** The catalogue sync of a `Books` server fetches the whole book list (pages of 500) at every start-up and Refresh and stores items `<key>:album:<id>` with `kind: book` (saved only if changed, as today). Library lists them after the local books with "on the server". They open as server books do today, but through `book()`. | With the fake Books server, Library lists its books after the local ones; a book dropped by the server is gone after a sync; a sync that changes nothing writes no index; Music never lists them. Refresh says "Library updated, 2 books.". |
| ✅ 2.6 | **Server chapters in the book player.** `Book::chapters` from `RemoteBook`; `App::chapters()`, `currentChapter()`, `gotoChapter()`, Next / Previous chapter, the Chapters list, "where am I?" and the sleep timer's end of chapter use them. The place stays `pos:<id>` = file and seconds. | A two-file book with three chapters, one starting in the middle of file 1: the Chapters list has three; Next chapter from chapter 1 seeks inside file 1; from chapter 2 it opens file 2 at the right second; "where am I?" names the chapter, not the file. A one-file book uses the server chapters. A book with no server chapters is one chapter per file, as today. Pause, another book, then this one resumes the place. |
| ✅ 2.7 | **The Books server menu.** As in [Menus](#menus): Recently added, Books A to Z (paged), Authors → books, Downloaded, Server status; a book opens on Play book, Download book, then its chapters. | Each list: Loading, filled with the first item spoken, error with Try again, empty ("No books", "No authors"). Books A to Z asks the second page while `more`. Play book resumes the saved place; OK on chapter 3 starts there; an author lists only their books. |
| ✅ 2.8 | **Download a book.** Download book fetches the files into the server's cache folder, pinned, as Download album does; a whole download plays offline from Library, from the Books menu's Downloaded and from "Now playing" after a restart (`last_book`). Progress counts files. | The downloader gets the files in order; "where am I?" says "Downloading, 3 of 24 files."; with the fake server down a whole download plays from its files with no "Loading."; a partial one gives "Server not reachable." as server books do today. |
| ✅ 2.9 | **Docs for phase 2 as built:** architecture.md (`Audiobookshelf`, chapters from the server, ids), navigation.md (the Books server menu), SPEC §6.3, README config example. | – |

**Device check (manual):** with the Audiobookshelf test server in `servers:`, Library lists
"Вперед в прошлое" and "Нейромант" with "on the server". The m4b plays and Next chapter
moves through its 23 chapters. "Нейромант" has 14 chapters across 24 files, and Next chapter
crosses files at the right second. Pause, restart, "Now playing" resumes the place. Download
"Нейромант", pull the Wi-Fi, and it still plays from Downloaded. Server status names
Audiobookshelf 2.37.2.

## Phase 3: progress sync

Goal: a book listened to on the device continues where it stopped in the Audiobookshelf app on
a phone, and the other way round. Offline listening is sent when the server can be reached
again.

| # | Step | Tests (each checked with a sabotage, as CLAUDE.md requires) |
|---|------|------|
| 3.1 | **PATCH with a body.** `Http` gets `patch(id, url, headers, body, handler)` (curl `CUSTOMREQUEST`, JSON content type). | Net test with the local socket server: method, header and body arrive; a cancelled PATCH's handler does not run. |
| 3.2 | **Progress in `MusicServer`.** `struct RemoteProgress { double seconds = 0, duration = 0; bool finished = false; std::int64_t updated = 0; /* wall, ms */ };` `virtual bool syncsProgress() const`, `virtual RequestId progress(std::string albumId, std::function<void(Error, std::optional<RemoteProgress>)>)`, `virtual void saveProgress(std::string albumId, RemoteProgress)` (no callback, a failure only on stderr without the URL). Audiobookshelf: `GET` / `PATCH /api/me/progress/<id>`; 404 is "none". Subsonic: `syncsProgress()` false. | Net tests: the progress fixture parses; 404 gives none and no error; the PATCH body has `currentTime`, `duration`, `progress`, `isFinished`. The fake records saves. |
| 3.3 | **Push the place.** For a server book on a server that syncs: the place on the book's timeline (start of the file + seconds) is sent on pause, on leaving the book, on sleep, on exit, and every 60 s of playback when it moved by 5 s or more. `state.tsv` gets `posat:<id>` (wall seconds when the place was recorded, kept with `pos:`) and `pushed:<id>` (the `posat` sent last). A send that fails leaves `pushed` behind; the start-up sync sends every place newer than its `pushed`. "End of book" sends `finished`. | Pause sends the timeline seconds of file 2 + 30 s. 59 s of play sends nothing new, 60 s does. A failed send is sent again at the next start-up; a successful one is not. End of book sends `finished`. Playback writes nothing to disk until exit or sleep, as today. |
| 3.4 | **Pull the place.** Opening a server book (Library, Books menu, "Now playing") plays at the local place at once and asks the server. If the server's place is newer than `posat` and more than 30 s from it: with no local place it moves there and says the chapter; otherwise it says "The server has a later place, chapter 5. It is in Options." and the book player's Options gets a first item "Go to the server's place, chapter 5, 12 minutes". The answer is dropped when the book is left. | No local place: the book moves to the server's place. A local place older than the server's: the notice and the Options item, and OK on it moves there. A server place older than the local one or within 30 s: nothing said. The server down: nothing said, the book plays. |
| 3.5 | **Continue listening.** First item of the Books server menu, from `/api/me/items-in-progress` (only this server's library). | Loading, filled, error, "Nothing in progress". A book listed opens at the server's place. |
| 3.6 | **Docs for phase 3 as built:** architecture.md, navigation.md (the notice and the Options item), SPEC §6.3. | – |

**Device check (manual):** listen 10 minutes on the device, pause, open the book in the
Audiobookshelf web page: it is at the device's place. Listen on the web page, open the book on
the device: it says the server has a later place, and Options moves there. Listen offline,
reconnect and restart: the web page has the offline place.

## Later

- **Podcasts** from an Audiobookshelf `podcast` library: the Podcasts Home entry, episodes,
  played state. Needs its own plan with SPEC §6.2.
- **Series** (`/api/libraries/<id>/series`) in the Books menu: the test server has none to
  check the answer against.
- **Bookmarks** sync (SPEC §6.3): Audiobookshelf has `/api/me/item/<id>/bookmark`; the device
  has no bookmarks yet.
- Listening sessions (`/api/items/<id>/play`) for the server's statistics.

---

## Decisions

- **Several servers in a `servers:` list**, each with a `type`. The old `server:` section is
  read when there is no list and is never rewritten.
- **One `MusicServer` interface** for both types, with `book()`, `catalogue()` and (phase 3)
  progress added. `App` asks `catalogue()` which menu a server gets; it never checks a type name.
- **Audiobookshelf logs in with an API key** (`api_key`), sent as a Bearer header; stream and
  download URLs carry it as `token=`. No user name / password login.
- **One book library per Audiobookshelf server**: the one named by `library`, else the first
  book library by display order. Podcast libraries are not read until podcasts are planned.
- **No cheap catalogue check on Audiobookshelf**: every sync fetches the book list and saves
  only what changed.
- **Original files only** from Audiobookshelf; Streaming quality applies to Subsonic.
- **Chapters come from the server** for an Audiobookshelf book; the place stays file + seconds.
- **The server key (`url` + `user`) is the identity** everywhere, as today.
- **Books of two servers are not merged** with each other or with local books.
- `.env` holds the test credentials for development only; the program does not read it.

## Open questions

1. **How to offer the server's later place** (step 3.4). Chosen: say it once and add an
   Options item, so playback is never interrupted by a question. SPEC §6.3 says "Server position
   is further, jump there?"; confirm on the device that the notice is enough.
2. **User name and password login** for Audiobookshelf (`POST /login`, short-lived tokens with
   a refresh token since 2.26). Chosen: not now, the API key is enough. Revisit with the setup
   web page (SPEC §6.3), where typing a password is easier than creating a key.
3. **Sending the place every 60 s** (step 3.3) costs a request per minute of playback over
   Wi-Fi, not flash. Chosen: 60 s; measure the battery cost on the device.
