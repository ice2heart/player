# Fixes from NOTES.md: plan

> Status: **a plan, nothing built.** It turns the notes in [NOTES.md](../NOTES.md) into work groups:
> discovery first where the cause or the design is not known, fixes where it is. The rules are in
> [navigation.md](navigation.md), the code is described in [architecture.md](architecture.md),
> streaming in [streaming.md](streaming.md), the queue in [queue.md](queue.md).
> Last updated: 2026-10-28.

Each note was checked against the code (2026-10-28). What the device does today, and where
the work goes:

| Note | Today | Kind | Where |
|------|-------|------|-------|
| Where am I (hold Play) should say the path | In a menu it speaks the path ("Music, Artists, Dune. ..."). In a player it speaks only the status (`playerStatus()`): no player name, no artist. | fix | 2.3 |
| Japanese, Chinese, Cyrillic need transliteration when there is no TTS | Every front end calls `tts.start("en")`. eSpeak's English voice spells Cyrillic letter by letter ("a, ge, a, te...") and reads every Han character as "Chinese letter" and kana as "Japanese letter". eSpeak NG 1.52 has `ru`, `cmn`, `yue`, `ja`, `ko` voices, and `<voice xml:lang="ru">…</voice>` in SSML switches voice within one utterance and back. Kanji gets no reading from any voice. | fix, then discovery | 4.3–4.5, 1.3, 6.5–6.6 |
| Sim (and core) must work with UTF-8 | Core sorts and groups by code point (`library.cpp`: `nextCodePoint`, `fold`, Cyrillic and Greek included). The sim uses Dear ImGui's default font (`AddFontDefault`), which has ASCII only: Cyrillic and CJK show as `?`. `album_merge.cpp` has its own `fold` that lowercases only Latin-1, so "АГАТА КРИСТИ" and "Агата Кристи" are two artists. Tags in a legacy code page (cp1251 in ID3) are not checked. | fix, then discovery | 4.1–4.2, 1.4, 6.7 |
| Not all artists | Music → Artists is built from the album artist of the albums in the index (`artistsMenu()`), never from track artists or the server's `getArtists`. The cause is not known yet (see 1.1). | discovery | 1.1, then 6.1 |
| Play modes: random, repeat, repeat album | Shuffle (off · tracks · albums) and Repeat (off · all · track) are built (queue.md 2.6). Repeat album is missing. | fix | 2.2 |
| (added) Shuffle does not work after the queue is added to | `Queue::append()` and `playNext()` (`insertAt()`) put the new entries into the play order in their own order, at the end or after the current entry. Only `setShuffle()` shuffles, when it is chosen. With Shuffle on, an album added with Add to queue plays in order after everything else. | fix | 2.7 |
| Deep press Down to jump lines? Jog dial? | Only Left / Right have a deep stage. Left / Right in menus already jump by letter or by 10, and holding Up / Down goes to the first or last item. No rotary input exists. A hardware decision. | discovery | 1.7, then 6.4 |
| Scrobble not sent when the app closes | `HttpClient`'s worker stops at once when it is destroyed: a scrobble that is queued or running at exit is dropped. A scrobble that fails (offline, server down) is only logged and is lost. | fix, then discovery | 3.1–3.2, 1.2 |
| Playlists should be in the Music menu, not the server's | Playlists exist only in Streaming → Playlists. They are not in the index, so they need the network to be listed. | fix | 2.4–2.5 |
| Playlist menu should have Download | A server playlist's list already has "Download playlist" after "Play playlist", but only when a cache folder is set up (`canDownload()`). Its Options have only Play now, Play next and Add to queue. | fix | 2.6 |
| An async platform for background tasks (libuv?) | Each service has its own `std::thread` (HTTP, downloader, scanner, player, TTS, audio) and hands its results to `App` through `poll()` in `tick()`. There is no shared task runner. | discovery | 1.5, then 6.2 |
| Download quality settings (raw, 320, 128) | Streaming quality is a setting (`streaming.bitrate`: original · 320 · 192 · 128). Downloads and the automatic cache always fetch the original (`download?id=`). The manifest records no quality. | fix | 5.2–5.3 |
| A track streamed in a low quality is kept as it is; the original only when asked | The automatic cache downloads the original a second time after 10 s, whatever the streaming quality. | fix, then discovery | 5.4, 1.6, 6.3 |
| Replace a local copy when a better quality is available | No such option. | fix | 5.5–5.6 |

---

## Work groups

1. **Discovery** (phase 1): what is not known yet. Each step writes its findings into
   [Findings](#findings) in this file and the decision it suggests, and changes no product
   code. Phase 6 is planned from them.
2. **Player and menus** (phase 2): Repeat album, "where am I?" in a player, Playlists in
   Music, Download in Options.
3. **Scrobbles that arrive** (phase 3): finish requests before exit, and keep failed
   scrobbles until the server answers.
4. **Names in other scripts** (phase 4): the sim's font, one case fold, plan A (a voice for
   each script that has one installed) and plan B (transliteration to Latin when none is).
5. **Download quality and the cache** (phase 5).
6. **From the findings** (phase 6): artists, background tasks, cache from the stream, jog dial,
   voices to download, Han transliteration, tag encodings. Each needs its phase 1 step done and
   its open question decided.

Phases 2 to 5 do not depend on phase 1, and can be built before it or alongside it.

---

## Repeat album

- **Repeat** becomes off · all · album · track, in that order, in the music Player menu.
- **Repeat album** plays the current entry's album again when the last entry of that album
  ends. "The album" means the entries of the queue with the same album name as the current
  entry, in play order (a run of entries as for the Album unit, navigation.md). With Shuffle
  tracks they may be apart in the play order; they repeat in that order all the same.
- Left / Right, a deep press and Next / Previous move to other tracks as they do now, and
  the album of the track moved to is then the one that repeats (as for Repeat track).
- `queue.tsv` writes `repeat\talbum`. An older build reads an unknown value as off.
- A new queue starts with Repeat off, as now.

## Shuffle and added entries

- **Add to queue with Shuffle on mixes the new entries in.** With Shuffle tracks each new
  entry goes to a random place among the entries that have not played yet (after the
  current one). With Shuffle albums the new entries are cut into runs of one album, as
  `setShuffle()` does, and each run goes, whole and in its order, to a random place between
  two runs of what has not played yet. The random generator is passed in, as for
  `setShuffle()`.
- **Play next stays in order:** the user asked for those entries next, so they go right
  after the current entry in their own order, with any Shuffle.
- The original order gets the new entries at the end (Add to queue) or after the current
  entry (Play next), as now, so turning Shuffle off gives the order they were added in.
- With Shuffle off nothing changes.
- What is spoken does not change ("Added Weather, 12 tracks. 30 in queue.").

## "Where am I?" in a player

- Holding Play/Pause in a player speaks the player first, then the artist, then what it
  says now: "Music player. Dune, by Weather. Track 5 of 30, Storm. 3 minutes left in
  track. Playing." The book player says "Book player. <title>, by <author>. Chapter ..."
- The artist is left out when it is empty. Download progress follows as now.
- In a menu it stays as built: the path, the selected item, and what plays.

## Playlists in Music

- **Music → Playlists**, after Albums A to Z, lists the server's playlists by name, with
  the track count as the value ("Road Trip, 12 tracks"). It is a local menu (navigation.md,
  Music lists): it is read from the index, so it opens at once and works offline.
- **The sync stores playlists in the index** (`library.db`): id, name, song count and the
  server's `changed` time, from `getPlaylists`, at each sync and Refresh library. A
  playlist made with Save as playlist is added to the index at once.
- **Downloaded playlists** are listed too, from the cache manifests, so a downloaded
  playlist is there when the index has never synced. A playlist in both is one entry.
- **Opening a playlist** is as in Streaming now: whole on the disk, it plays from the files
  with no "Loading."; otherwise its tracks load from the server under the rules of "Lists
  from the server" ("Loading", the error, "Try again", "Downloaded" when offline).
  "Play playlist" first, then "Download playlist" where it can work, then the tracks.
- **Streaming no longer has Playlists.** Streaming → Downloaded still lists downloaded
  playlists.
- With no playlists the list has one item, "No playlists"; OK says "Make a playlist on the
  server, or save the queue as a playlist."

## Download in Options

- The Options of an album or playlist of the server (in Music and in Streaming) get
  **Download album** / **Download playlist** after Add to queue, only where it can work (a
  cache folder, a server and a downloader that are set up, as for the list item).
- OK starts the download, says "Downloading, 0 of 9 tracks." and goes back by itself, as
  the list item does. While it runs, OK on it gives the progress; another list says
  "Another download is running." Everything else is as for the list item (navigation.md,
  Download album).
- An album that is whole on the device has no Download in its Options.

## Scrobbles

- **Requests finish before exit.** After `App::shutdown()` the front end gives the server
  up to 3 s to send what is queued and running (`Subsonic::finish(deadline)`), then
  aborts the rest. `App` itself never waits.
- **A scrobble that cannot be sent is kept.** A submission (`submission=true`) that fails
  because the server cannot be reached goes into the **scrobble outbox**, with the time it
  was heard. While the server is marked down, submissions go straight into the outbox.
  "Now playing" (`submission=false`) is never kept.
- **The outbox is sent when the server answers:** after a ping clears the mark, after a
  sync, or after any request that works. One at a time, oldest first, with `time=` (ms
  since the epoch, from `clock_.wallTime()`). A server error (not a network one) drops the
  entry, so a scrobble the server refuses is not sent for ever.
- **`scrobbles.tsv`**, next to `state.tsv`: server key, track id, time. Written on sleep
  and on exit, only when it changed, like `queue.tsv`. At most 1000 entries; the oldest
  go first. Entries of another server (another url or user) stay until that server is set
  up again.
- Nothing is spoken: a scrobble is never heard (services.h).

This takes the scrobble part of the offline outbox of [streaming.md phase
5](streaming.md#phase-5-sync-and-playlists); stars, bookmarks and playlist edits stay there.

## Names in other scripts

- **The sim draws every script.** It loads a TTF font with Latin, Cyrillic, Greek and CJK
  glyphs: `--font PATH`, else the first that exists of a list of system fonts (macOS:
  `/System/Library/Fonts/Supplemental/Arial Unicode.ttf`, `/System/Library/Fonts/PingFang.ttc`;
  Linux: `/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc`,
  `/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf`), else the default font as now, with a
  line on stderr: "sim: no font with Cyrillic and CJK found; use --font".
- **One case fold.** `album_merge.cpp` uses the fold of `library.cpp` for artist and album
  keys, so names that differ only in case are one artist or album in every script
  ("АГАТА КРИСТИ", "Агата Кристи"). Accents still count in keys, as now.
- **Script runs.** `scriptRuns(text)` in `app/core/text.{h,cpp}` cuts a UTF-8 string into
  runs of one script: Latin (with digits, spaces and punctuation, which join the run
  around them), Cyrillic, Greek, Han, Kana, Hangul, Hebrew, Arabic, Other. A run of Han
  next to Kana is one Japanese run.
- **Plan A: each script is spoken with its voice.** `Tts` checks which voices are installed
  at start (`espeak_ListVoices`) and speaks each run that is not Latin with the voice of
  its script, through SSML, one `<voice xml:lang="…">` element per run (not
  `<s xml:lang>`, which leaks the language into the text after it): Cyrillic `ru`, Greek
  `el`, Han `cmn`, Japanese (kana, or kana with Han) `ja`, Hangul `ko`, Hebrew `he`, Arabic
  `ar`. The rest of the sentence keeps the English voice: "Artist Агата Кристи, 3 of 10".
  The voices that come with eSpeak are used first; more can be downloaded
  ([Voices to download](#voices-to-download)).
- **Plan B: a run with no voice installed is transliterated** to Latin and spoken with the
  English voice: "Агата Кристи" → "Agata Kristi", "ひらがな" → "hiragana", "아이유" →
  "aiyu". `transliterate(run)` in `app/core/text.{h,cpp}`, plain tables in core:
  Cyrillic (ISO 9 without diacritics, close to the common Russian romanisation), Greek
  (ELOT 743), kana (Hepburn), Hangul (Revised Romanization, computed from the syllable).
  Han needs a table of readings that is too big to write by hand: until 6.6 a Han run with
  no voice, and the kanji of a Japanese run (eSpeak's `ja` reads kana, not kanji), are
  spoken once by script ("Chinese text"), not letter by letter. Hebrew, Arabic and other
  scripts with no voice are spoken the same way ("Arabic text").
- Only names and titles from tags and the server reach this path; the device's own texts
  stay English.

## Voices to download

Plan A for a script whose voice is not installed: Settings → **Languages** lists the
languages of the scripts above, each with its state ("Russian, installed", "Japanese, not
installed, 12 megabytes"). OK on one that is not installed downloads its voice into
`/data/languages/<lang>/` (elsewhere `languages/` next to `state.tsv`), checks it by its
hash, and speaks the progress like a download. From then on that script is spoken with the
voice instead of being transliterated. Which voices can be downloaded (eSpeak data for a
language the image lacks, Piper voices, a kanji reading for `ja`), from where and how big is
step 1.3; the steps are in phase 6 (6.5).

## Download quality and the cache

- **Download quality** (Settings, `downloads.bitrate` in `config.yaml`): original file ·
  320 · 192 · 128 kbps, default original. It is a `choice()` like Streaming quality and is
  used by Download album / playlist. A transcode is fetched with `stream?id=&format=mp3&maxBitRate=N`,
  the original with `download?id=` as now.
- **The cache records the quality** of each file: a `bitrate` column in `manifest.tsv`, 0
  for the original, else the kbps of the transcode. Rows from an older manifest are 0.
- **A transcode is not resumed:** its `.part` starts over (the server cannot seek a
  transcode by bytes). Free space is checked against the duration × bitrate for
  transcodes, the server's size for originals.
- **The automatic cache keeps the quality that was streamed.** A track streamed at 128
  kbps is cached at 128 kbps (a second fetch at that quality, as now but smaller), not as
  the original. The original is fetched only when the user asks (Download, with Download
  quality original). Whether the stream itself can be kept without a second fetch is step 1.6.
- **Download keeps or replaces a copy.** Download album / playlist counts a whole track as
  done when its copy is at least the Download quality (original is best; else the higher
  kbps). A lower copy is fetched again; the new file replaces the old one once complete, and
  the old one plays until then.
- **Replace cached copies** (Settings, `cache.upgrade`, off · on, default off): when on and
  the server is not marked down, a track whose cached copy is below the Streaming quality
  is streamed instead of played from the cache, and the automatic cache replaces the copy
  after 10 s. A pinned copy is replaced too and stays pinned. When off, a cached copy plays
  whatever its quality, as now.

---

## Phase 1: discovery

Goal: the causes and designs that are not known are written down in [Findings](#findings),
with the decision each one suggests, so phase 6 can be planned. No product code changes;
throw-away scripts stay out of the repo (the scratch folder), tools worth keeping go in
`tools/`.

| # | Step | Tests (each checked with a sabotage, as CLAUDE.md requires) |
|---|------|------|
| 1.1 | **Missing artists.** Compare the test server's `getArtists` (and each album's OpenSubsonic `artists` / `displayArtist` in `getAlbumList2`) with the artists the index gives (`artistsMenu()` over `mergeAlbums`). Check the hypotheses: an artist only on tracks (featured), an album with several artists whose `artist` is one joined string ("A • B"), compilations without an album artist split per track, a sync that stopped early (`kCatalogueLimit`, an empty page with `more`), local albums without an album tag under "Unknown artist". Credentials from `.env`; never print or write the token or the password. Write the counts, the missing names (a sample) and the cause. | – |
| 1.2 | **Scrobbles at exit and "now playing".** Measure on the test server: is a scrobble queued just before exit lost (expected yes, 3.1 fixes it); does Navidrome's "now playing" stay after the app closes, and for how long; does Navidrome accept `time=` on a late submission and list it at that time. Decide whether exit should send anything more. | – |
| 1.3 | **Voices to download, and Han transliteration.** On the Pi image (Debian `espeak-ng-data`): which voices are installed, and how long a voice switch inside one utterance takes on a Pi Zero 2 W. Voices that can be downloaded (plan A): eSpeak data for one language, Piper voices per language (can the Pi Zero 2 W run one next to playback?), a kanji reading for Japanese (kakasi / a kanji→kana dictionary). Transliteration of Han (plan B): a Han→pinyin table (Unihan `kMandarin`) and ICU Any-Latin. For each: size, licence, quality on 20 real artist and album names, where it would be downloaded from, how it is checked (hash). Suggest the format of a downloaded voice and the Languages list. | – |
| 1.4 | **Tag encodings.** With FFmpeg's tag reading (`tag_reader.cpp`): what comes out of ID3v1 and ID3v2 tags written in cp1251 and Shift-JIS but marked Latin-1 (mojibake such as "Àãàòà Êðèñòè"), and of a file name in a non-UTF-8 encoding. Make sample files in the scratch folder. Suggest a repair (detect and re-decode) or none. | – |
| 1.5 | **Background tasks.** List the work that waits for a background runner (scrobble outbox, language pack download, cache replacement, the stream tee of 1.6, transliteration) and what the existing pattern (a thread per service, results through `poll()` in `tick()`) costs for each. Compare a small `TaskRunner` in `services.h` (a worker thread, callbacks run in `poll()`) with libuv: dependencies, size, threads on the Pi Zero, how tests drive it. Suggest one. | – |
| 1.6 | **Keep the stream as it plays.** Can `Player` (FFmpeg) write the bytes it reads to a `.part` (a custom `AVIOContext` that tees), so a streamed track is cached without a second fetch? What happens with seeks, Range retries, `timeOffset` transcodes and a stream that breaks off. Suggest the design or say why not. | – |
| 1.7 | **Jog dial and deep Up / Down.** What evdev gives for a rotary encoder on the Pi (`rotary-encoder` overlay: `EV_REL` / `REL_X`, or keys) and for a two-stage Up / Down. Write what each would do under the navigation rules (rule 1: Up / Down only move; Left / Right already jump) and suggest a mapping. The hardware choice is the user's (open question 3). | – |
| 1.8 | **Findings written:** each step's result in [Findings](#findings); phase 6 steps filled in or dropped; open questions updated. | – |

**Device check (manual):** read the Findings and decide open questions 2 to 5.

## Phase 2: player and menus

Goal: Repeat album works, Add to queue follows Shuffle, "where am I?" in a player says which player and whose music,
playlists are in Music and work offline, and an album or playlist can be downloaded from
its Options.

| # | Step | Tests (each checked with a sabotage, as CLAUDE.md requires) |
|---|------|------|
| 2.1 | **Navigation contract first:** navigation.md gets Repeat off · all · album · track, Add to queue mixing into a shuffled queue ([above](#shuffle-and-added-entries)), "where am I?" in a player ([above](#where-am-i-in-a-player)), Music → Playlists ([above](#playlists-in-music)) and Streaming without Playlists, Download in Options ([above](#download-in-options)). The menu tree in architecture.md. No code. | – |
| 2.2 | **Repeat album.** `QueueRepeat::Album` in `queue.{h,cpp}`; `advance()` at the end of the album's last entry in play order goes to its first; Next / Previous move as now. The Repeat list in the music Player menu; `queue.tsv` writes and reads `album`. | `test_queue.cpp`: at the end of the album's last track the first track of that album plays, with shuffle off and with shuffle tracks. Next on the last track of the album goes to the next album, which then repeats. `test_queue_file.cpp`: `album` round-trips; an unknown value reads as off. `test_app.cpp`: Repeat list says "album"; choosing it and letting the album end plays its first track. |
| 2.3 | **"Where am I?" in a player** names the player and the artist: "Music player. Dune, by Weather. Track 5 of 30, Storm. ..."; "Book player. <title>, by <author>. ..."; no "by" when the artist is empty. | Hold Play in the music player starts with "Music player." and has "by Weather"; in the book player "Book player."; a track with no artist has no "by". A menu still speaks the path. |
| 2.4 | **Playlists in the index.** `LibraryStore` stores playlists (id, name, song count, `changed`), schema version up by one, an older `library.db` gets the table. The sync fetches `getPlaylists` after the catalogue; Save as playlist adds the new one. | `test_sqlite_library_store.cpp`: playlists round-trip; an older database opens and gains the table. `test_app.cpp`: after a sync the index has the server's playlists; a sync that fails keeps the old ones; Save as playlist adds one without a sync. |
| 2.5 | **Music → Playlists.** After Albums A to Z, sorted by name (Left / Right jump by letter); from the index and the cache manifests, one entry for a playlist in both; opening as in [Playlists in Music](#playlists-in-music); Streaming loses its Playlists item. "No playlists" with its OK text. | Playlists lists the indexed and the downloaded playlists, sorted, with "12 tracks". With the server down, a downloaded playlist plays from its files and one that is not says "Server not reachable" with "Try again" and "Downloaded". An empty list says "No playlists" and OK gives the text. Streaming has no Playlists item. |
| 2.6 | **Download in Options** of server albums and playlists in Music and Streaming: "Download album" / "Download playlist" after Add to queue, only where `canDownload()`; OK starts it, says "Downloading, 0 of 9 tracks." and goes back; the same rules as the list item. | The item is in the Options of a server album and a playlist, and not without a cache folder, nor for an album whole on the device. OK starts the download (the fake downloader gets the tracks), speaks the count and goes back. A second list says "Another download is running.". |
| 2.7 | **Shuffle and added entries.** `Queue::append(entries, random)` mixes the new entries into what has not played yet when Shuffle is on, as in [Shuffle and added entries](#shuffle-and-added-entries); `playNext` is unchanged. `App` passes its random generator (the one `setShuffle()` gets). | `test_queue.cpp` with a fixed random: Shuffle tracks and append puts new entries among the unplayed ones, not all at the end, and never before the current entry; Shuffle albums keeps each added album whole and in order, between two runs; Shuffle off appends at the end as now; Play next with Shuffle on puts the entries right after the current one, in order; Shuffle off afterwards gives the original order with the new entries at the end. `test_app.cpp`: with Shuffle tracks, Add to queue on an album and then Next through the queue does not play its tracks one after the other in album order. |
| 2.8 | **Docs for phase 2 as built:** architecture.md (Repeat album, shuffle of added entries, `library.db` playlists, menu tree), navigation.md where the build differs from 2.1, queue.md (Phase 2 as built: added entries), SPEC. | – |

**Device check (manual):** Repeat album on an album in the middle of a queue: it plays
again from its first track, and Next leaves it. With Shuffle tracks, Add to queue on an
album: its tracks come up mixed with the rest, not in a block at the end. Hold Play in each player and hear which
player it is. Music → Playlists with the Wi-Fi off: a downloaded playlist plays. Download
an album from its Options and hear the progress with "where am I?".

## Phase 3: scrobbles that arrive

Goal: a track that was heard is scrobbled even when the app closes right after, or the
server could not be reached at the time.

| # | Step | Tests (each checked with a sabotage, as CLAUDE.md requires) |
|---|------|------|
| 3.1 | **Finish requests before exit.** `HttpClient::finish(deadline)`: no new requests, waits for the queued and running ones until the deadline, then aborts the rest; the destructor stays as it is. `Subsonic::finish(deadline)`. The sim and headless front ends call it with 3 s after `app.shutdown()`. | `test_http_client.cpp` with its local server: a request queued just before `finish()` reaches the server; a server that never answers makes `finish()` return after the deadline (a short one in the test); a request after `finish()` is not sent. |
| 3.2 | **Scrobble outbox.** `MusicServer::scrobble(trackId, submission, timeMs, done)` reports the error (`done` may be empty); `Subsonic` sends `time=` when `timeMs` is set; `FakeMusicServer` records it. `App` keeps failed submissions as in [Scrobbles](#scrobbles), sends them when the server answers, writes `scrobbles.tsv` on sleep and exit when changed, reads it at start. | Net test: the URL has `time=` and `submission=true`. `test_app.cpp`: a submission that fails as unreachable is sent again, with its time, after a ping clears the mark; one refused by the server is not sent again; with the server marked down a submission goes to the outbox without a request; "now playing" is never kept; the outbox survives a restart; a second exit with no change writes nothing; 1001 entries keep the newest 1000; entries of another server stay and are not sent. |
| 3.3 | **Docs for phase 3 as built:** architecture.md (threads, persistence: `scrobbles.tsv`), streaming.md phase 5 (the scrobble part of the outbox is built). | – |

**Device check (manual):** listen to half a track and power off at once: the play is on
the server. Turn the Wi-Fi off, listen to two tracks from the cache, turn it on: both plays
reach the server with the times they were heard.

## Phase 4: names in other scripts

Goal: names in Cyrillic, Greek, Chinese, Japanese kana and Korean are drawn in the sim and
spoken with a voice of their language when one is installed (plan A), else transliterated to
Latin (plan B); a script with neither is named once, not spelled.

| # | Step | Tests (each checked with a sabotage, as CLAUDE.md requires) |
|---|------|------|
| 4.1 | **The sim draws every script.** `pickFont(candidates, exists)` in `app/frontend/` returns the first font that exists; the sim adds it with Cyrillic, Greek and CJK glyph ranges; `--font PATH`; the stderr line when none is found. | `pickFont`: `--font` wins, else the first that exists, else none. |
| 4.2 | **One case fold** for artist and album keys (`album_merge.cpp` uses `library.cpp`'s fold; its own `fold` goes). | `test_album_merge.cpp`: "АГАТА КРИСТИ" and "Агата Кристи" are one artist; "ΑΒΓ" and "αβγ" one album; "Été" and "Ete" stay two. |
| 4.3 | **Script runs** (`scriptRuns()` in `app/core/text.{h,cpp}`), as in [Names in other scripts](#names-in-other-scripts). | `test_text.cpp`: "Artist Агата Кристи, 3 of 10" gives Latin, Cyrillic, Latin; "宇多田ヒカル" one Japanese run; "周杰伦" Han; digits and punctuation join the run around them; a broken UTF-8 byte does not crash and stays in its run. |
| 4.4 | **Transliteration (plan B)** (`transliterate()` in `app/core/text.{h,cpp}`): Cyrillic, Greek, kana and Hangul to Latin, as in [Names in other scripts](#names-in-other-scripts); any other script gives nothing (the caller names it). | `test_text.cpp`: "Агата Кристи" → "Agata Kristi", "Ёлка" → "Yolka", "Ωμέγα" → "Omega", "ひらがな" → "hiragana", "カタカナ" → "katakana", "아이유" → "aiyu", "한국" → "hanguk"; "周杰伦" gives nothing. |
| 4.5 | **Each script with its voice, else transliterated.** `speechMarkup(text, voices)` in `app/frontend/` builds the SSML: one `<voice xml:lang>` per run whose voice is installed (plan A), the transliteration for a run whose voice is not (plan B), "Chinese text" (and so on) for a run with neither; XML escaped; plain text when no markup is needed. `Tts` lists the installed voices at start and speaks with `espeakSSML` when the text has markup. | `speechMarkup`: Cyrillic with `ru` installed gives one `ru` voice element and the rest outside it; without `ru` it gives "Agata Kristi" with no markup; Han without `cmn` gives "Chinese text"; the kanji of a Japanese run give "Japanese text" and its kana the `ja` voice; `&` and `<` in a name are escaped; all-Latin text is unchanged. |
| 4.6 | **Docs for phase 4 as built:** architecture.md (speech: voices per script, transliteration, the sim's font), SPEC. | – |

**Device check (manual):** Music → Artists with Russian, Chinese, Japanese and Korean
artists: each is spoken in its language within the English sentence, Left / Right jump to
them, and the sim shows them. Remove the `ru` voice from the eSpeak data: "Агата Кристи"
is spoken "Agata Kristi". A kanji name says "Japanese text" once.

## Phase 5: download quality and the cache

Goal: downloads have their own quality, the cache keeps what was streamed at its quality,
and a better copy replaces a worse one when the user asks for it.

| # | Step | Tests (each checked with a sabotage, as CLAUDE.md requires) |
|---|------|------|
| 5.1 | **Navigation contract first:** navigation.md gets Download quality and Replace cached copies in Settings, and the cache rules of [Download quality and the cache](#download-quality-and-the-cache). No code. | – |
| 5.2 | **The manifest records the quality:** `bitrate` column in `manifest.tsv`; older rows read as 0 (original). | `test_track_cache.cpp`: the bitrate round-trips; an older manifest reads as 0 and is written back with the column. |
| 5.3 | **Download quality.** `choice("Download quality", "downloads.bitrate", ...)`; `Downloader::download` gets the bitrate; `Subsonic::downloadUrls` gives `stream?…&format=mp3&maxBitRate=N` for N > 0; a transcode's `.part` starts over; free space uses duration × bitrate for transcodes. | Net tests: bitrate 128 gives a `stream` URL with `maxBitRate=128` and no `p`; 0 gives `download?id=`; a transcode `.part` is not resumed with Range. `test_app.cpp`: Download album at 128 records 128 in the manifest; the free-space check uses duration × bitrate; the setting persists. |
| 5.4 | **The automatic cache keeps the streamed quality:** its job uses the Streaming quality and records it. | Streaming at 128: the cached track is fetched at 128 and recorded as 128; at original it is the original, as now. |
| 5.5 | **Download keeps or replaces a copy:** a whole track at or above the Download quality is done and pinned; a lower one is fetched again and replaces the old file once complete; the old one plays until then. | A track cached at 128 and Download at original: it is fetched again and the manifest says 0. Cached at 320 and Download at 128: not fetched, pinned. Before the new file is complete the old one plays. |
| 5.6 | **Replace cached copies** (`cache.upgrade`, off · on): when on and the server is not marked down, a copy below the Streaming quality is streamed and replaced after 10 s, pinned or not. | On: a copy at 128 with Streaming at original opens the stream URL and is replaced. Off: the copy plays. Server marked down: the copy plays. A replaced pinned copy stays pinned. |
| 5.7 | **Docs for phase 5 as built:** architecture.md (cache, manifest), navigation.md where the build differs from 5.1, streaming.md (phase 3 as built: quality), SPEC. | – |

**Device check (manual):** set Streaming quality to 128 and play a track for a minute:
the cache has it at 128 (no second download of the original). Download the album at
original: that track is fetched again. With Replace cached copies on and Streaming at
original, the 128 copy is replaced when it plays.

## Phase 6: from the findings

Not ready to build: each step depends on its phase 1 step and an open question. Phase 1.8
fills in the Step and Tests cells, or drops the step.

| # | Step | Tests (each checked with a sabotage, as CLAUDE.md requires) |
|---|------|------|
| 6.1 | **All artists** in Music → Artists, from the cause found in 1.1 (for example the album's `artists` list, track artists, or a sync that goes to the end). | From 1.1. |
| 6.2 | **Background tasks**, as chosen in 1.5 (open question 2). | From 1.5. |
| 6.3 | **Cache from the stream** without a second fetch, if 1.6 shows it can work. | From 1.6. |
| 6.4 | **Jog dial or deep Up / Down**, as chosen in 1.7 (open question 3); navigation.md first. | From 1.7. |
| 6.5 | **Voices to download (plan A):** Settings → Languages as in [Voices to download](#voices-to-download); a downloaded voice is used instead of the transliteration. The voices, their format and source from 1.3 (open question 4). Probably two steps: the list and the download, then `Tts` loading a downloaded voice. | From 1.3. |
| 6.6 | **Han transliteration (plan B):** a Han→pinyin table (built in, or downloaded with the Languages list), so a Han run with no `cmn` voice is read in pinyin instead of "Chinese text". From 1.3. | From 1.3. |
| 6.7 | **Tag encodings repaired**, if 1.4 suggests a repair (open question 5). | From 1.4. |

---

## Findings

Filled in by phase 1. Facts checked while writing this plan (2026-10-28):

- eSpeak NG 1.52 (macOS): the `en` voice spells Cyrillic ("a g e a t e ...") and reads Han
  and kana as "Chinese letter" / "Japanese letter". `<voice xml:lang="ru">` within an SSML
  utterance gives a Russian reading and switches back after `</voice>`; `<s xml:lang="ru">`
  leaks Russian into the text after it. `cmn` reads Han in pinyin with tones, `ja` reads
  kana but not kanji, `ko` reads Hangul.
- `HttpClient::~HttpClient` sets `stop_` and the worker breaks out at once, so queued and
  running requests are dropped at exit. `Subsonic::scrobble` only logs a failure.
- Downloads and the automatic cache use `download?id=` (the original); `CachedTrack` has no
  quality.

## Decisions

- **Discovery writes findings, not code.** Phase 6 is planned from them.
- **Plan A is a voice, plan B is transliteration.** A script is spoken with its voice when
  one is installed (eSpeak's own, or one downloaded from Settings → Languages); with no
  voice installed it is transliterated to Latin and spoken with the English voice.
- **The device's own texts stay English.** Only names and titles get other voices.
- **Repeat album** is a fourth Repeat value; it belongs to the queue like the others.
- **Add to queue follows Shuffle; Play next does not.** Entries added with Shuffle on are
  mixed into what has not played yet; Play next keeps its entries next and in order.
- **Playlists move to Music** and are kept in the index; Streaming no longer lists them.
- **Scrobbles are kept until sent**, in `scrobbles.tsv`; this part of streaming.md
  phase 5's outbox moves here. "Now playing" is never kept.
- **The automatic cache keeps the quality that was streamed;** the original comes only from
  a Download at original quality.
- **Download quality is its own setting**, apart from Streaming quality.

## Open questions

1. **"Where am I?" in a player** (chosen: the player's name and the artist before the
   status, [above](#where-am-i-in-a-player)). The other option: the menu path that led to
   the player as well ("Music, Artists, Weather, Dune, Music player..."), which is long.
2. **Background tasks:** a small `TaskRunner` of our own or libuv (1.5). Nothing in phases
   2 to 5 needs it.
3. **Jog dial or deep Up / Down:** which hardware, and what a turn or a deep press does
   (1.7).
4. **Voices to download:** which ones (eSpeak data, Piper, a kanji reading for `ja`), where
   they are downloaded from, and how big (1.3). And whether the Han→pinyin table of 6.6 is
   built in or downloaded.
5. **Tag encodings:** repair cp1251 / Shift-JIS mojibake or leave it (1.4).
6. **Han without kana** is read as Chinese (`cmn`) (chosen). A Japanese name written only
   in kanji is then read in Chinese until a kanji reading for `ja` is downloaded (6.5); check with real names in 1.3.
